# AGENTS.md

One Arduino sketch, no build system beyond the convenience `Makefile`:
`nano_esp32_usb_host_keyboard.ino` — an Arduino Nano ESP32 (ESP32-S3) as a
**USB host** for a HID keyboard via the **ESP-IDF USB Host Library**
(`esp_usb_host`), raw 8-byte reports on a PL2303 UART console, Wi-Fi OTA for
updates. `compile` is the only verification available headless; the real
check is typing on a keyboard while watching the console.

## Build

```bash
arduino-cli compile --profile nano_esp32 .          # or: make compile
arduino-cli upload  --profile nano_esp32 -p /dev/ttyACM0 .   # make usb (keyboard cable unplugged)
arduino-cli upload  --profile nano_esp32 -p nano-esp32-kbd.local --protocol network \
                    -F password= --discovery-timeout 15s .   # make ota (works with keyboard attached)
python3 tools/console.py --seconds 20               # make monitor (PL2303 on D0/D1/GND)
```

Verified: **776,613 B flash (24 % of 3,145,728) / 49,080 B RAM (14 % of
327,680)** (build 3, with the error re-arm), clean at `--warnings all`. 17
`usb_host_*` functions link from the core's prebuilt `libusb.a` — the ESP-IDF
host stack ships inside `arduino:esp32`, no vendoring, no component manager.
Confirmed running on the board: the console status line prints every 5 s with
`build 3 … kbd=mounted 046D:C313 low … net=connected … heap=269112 …
ota=nano-esp32-kbd.local:3232`.

- Core: `arduino:esp32` **2.0.18-arduino.5**, FQBN
  `arduino:esp32:nano_nora:USBMode=hwcdc` (pinned in `sketch.yaml`).
- **`USBMode=hwcdc` is not optional.** With `usb_mode=0` the core's
  `main.cpp` auto-starts TinyUSB (`Serial.begin()`, DFU) at boot and the IDF
  host stack cannot own the PHY. With hwcdc nothing USB-related starts.
- First flash goes over DFU (`2341:0070`) with the **keyboard cable
  unplugged**; every flash after that goes over the air. While the host
  sketch runs there is no CDC and no DFU on the port.

## Stack traps (all load-bearing, all verified against headers/libs or the
core's own `USBHostIDF` example on 3.3.12)

- **Never call `USB.begin()`, `USBHost.begin()`, or `Serial.begin()`** in
  this sketch. TinyUSB/device and esp_usb_host cannot share the peripheral;
  `Serial` is `HWCDC` here and would claim the same PHY the host needs.
  Console is `Serial1` only.
- **`enum_filter_cb`:** `arduino:esp32` 2.0.18 does **not** define
  `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` — its `usb_host_config_t`
  has only `skip_phy_setup` + `intr_flags`. Core 3.3.11+ enables it and then
  treats a NULL callback as "reject every device" (silent dead bus). The
  sketch's `enumFilterAllowAll` sits behind the same `#if` so it is correct
  on both cores; don't unguard it.
- **IN transfer sizes:** `usb_host_transfer_alloc(mps, …)` then
  `transfer->num_bytes = mps` — IN sizes must be integer multiples of the
  endpoint MPS.
- **Re-arm every completion.** `inTransferDone` re-submits; without that the
  keyboard reports exactly once. It runs on the client task (inside
  `usb_host_client_handle_events`), not an ISR, so a plain `xQueueSend` to
  `sReportQ` is fine; `loop()` drains and prints. Build 3 re-arms on
  **every** status except `NO_DEVICE` (device pulled) and after
  `IN_ERR_CAP` (50) consecutive non-`COMPLETED`, non-`STALL` failures, where
  it prints `[usbh] IN endpoint stuck after 50 errors — unplug the keyboard
  to recover` once and stops, so a present-but-dead endpoint cannot spin the
  client task forever. `STALL` halts + clears the endpoint (guarded on
  `sDev != NULL`) and then re-arms.
- **Open/claim/close only from the client task.** Event callbacks set
  `sActions` bits (`ACT_OPEN_DEV`/`ACT_CLOSE_DEV`) and return; the task does
  the work. Blocking in a client event callback stalls all client events.
- **`usb_host_interface_claim(client, dev, bInterfaceNumber,
  bAlternateSetting)`** — 4-arg form on this IDF (5.1.x); older IDF had an
  endpoint-address arg. `usb_host_endpoint_halt`/`clear` exist for stall
  recovery (see `inTransferDone`).
- **Console pins:** `Serial1.begin(115200, SERIAL_8N1, D0, D1)` — with
  `BOARD_HAS_PIN_REMAP` the nano_nora variant defines `D0 = 0`, `D1 = 1`
  (Arduino numbers, `// also RX` / `// also TX`); `HardwareSerial::begin`
  maps them through `digitalPinToGPIONumber` to GPIO44/GPIO43. Only the
  `byGPIONumber` build of the variant has `D0 = 44`. The IDF console
  (UART0, the ROM log) uses those same pins; `Serial1.begin()` detaches
  UART0 and re-attaches them to UART1, so the boot log is visible on the
  cable until the app takes the line.
- **PL2303 monitor gotchas:** the kernel's legacy pl2303 converter always
  reports **0 bytes waiting**, so pyserial's `in_waiting` cannot gate reads —
  `tools/console.py` block-reads with `timeout=0.2`. `str.partition` returns
  `(left, sep, right)`: unpack `line, _, pending = pending.partition("\n")`,
  not the other way round (the reversed form prints empty lines and keeps
  the text). `minicom`/`cat` work because they block-read. A second reader on
  the same tty (a stale background capture, or your own `minicom`) makes
  pyserial raise `device reports readiness to read but returned no data
  (device disconnected or multiple access on port?)`; `tools/console.py`
  reopens the port on that instead of dying, but only one reader gets the
  bytes — kill the stale capture (`ps -C python3 | grep console`, then `kill
  <pid>`) before starting a new one.
- **Unplug path (verified):** pulling the keyboard leaves the in-flight IN
  transfer completing with **status 7 = `USB_TRANSFER_STATUS_NO_DEVICE`**
  (console: `[usbh] IN transfer status 7`), and only then does the client
  receive `USB_HOST_CLIENT_EVENT_DEV_GONE` → `[kbd] removed`. Transfer-first,
  event-second — the status line is not an error, and `inTransferDone`
  correctly does **not** re-arm on it. Heap steps 269084 → 269984 (transfer
  freed) → 268900 after remount.
- **No per-mount leak (measured over 2 cycles):** heap is exactly binary —
  **268,900 B while mounted, 269,984 B while unmounted**, and it returns to
  268,900 after every remount. The 1,084 B swing is the IN transfer plus the
  device allocation. The one-time 184 B shift seen at the first remount
  following an OTA reboot does not recur.
- **Unplug teardown is nondeterministic:** one pull ended the in-flight IN
  transfer with **status 1 = `USB_TRANSFER_STATUS_ERROR`** (no response /
  CRC), the next with **status 7 = `NO_DEVICE`**; both are followed by
  `DEV_GONE` → `[kbd] removed`. Neither is a bug, but note that
  `inTransferDone` only re-armed on `COMPLETED` in build 2, so a live device
  that errors without going away would stop reporting — **build 3 closes that
  gap** by re-arming on every status except `NO_DEVICE` and past the
  `IN_ERR_CAP` streak.
- **Mount line anatomy:** `[usbh] device 1: 046D:C313, low speed,
  configuration 1` then `[kbd] mounted: … interface 0, IN ep 0x81, mps 8`.
  `0x81` = `0x80 | 1`: the high bit is the endpoint **direction** (IN), so
  the endpoint address is 1. `low speed` means the 8 ms/125 Hz polling
  interval, which is why `reports` climbs ~2/s.
- **Report display hides the reserved byte:** `printReport` prints modifiers
  and the six keycodes only, so two consecutive reports that differ **only in
  byte 1 (the reserved field)** look identical on the console while still
  passing the `memcmp` dedup — this Logitech varies there, which is why a few
  identical-looking `kbd  -  keys 00 …` lines print back to back.
- **IDE 2's OTA password prompt is core-driven, not sketch-driven:** `tools.esp_ota.upload.field.password=Password`
  in the core's `platform.txt` makes IDE 2 open the Configure-and-Upload dialog
  (arduino-ide #1770/#1488 — the IDE cannot know the sketch has no password).
  The pinned core's `platform.txt` is patched locally (`~/.arduino15/packages/arduino/hardware/esp32/2.0.18-arduino.5/`):
  the two `upload.field.password` lines removed, pattern keeps `--auth=` as an
  empty literal — `espota.py` needs the flag present (`None` crashes its digest
  path), and the password-less server accepts anything. Reinstalling the core
  restores the prompt; re-apply the patch, or type any text. The CLI never
  prompts: `-F password=` empty is the same empty auth.
- **VBUS:** the ESP32-S3 does not source VBUS as a host. The power-injection
  cable feeds the bus (and the board through the OTG cable) from a powered
  hub; the thin leg goes to the hub. `USBHostBoardInit()` does not exist on
  this core (3.3.x only) — don't call it.
- **No reset circuit on the console cable:** Rx/Tx/GND only. To catch the
  boot log, open the monitor first, then press RESET.
- **Console port name moves** (`/dev/ttyUSB0` → `ttyUSB1` on replug): always
  open `/dev/serial/by-id/usb-Prolific_Technology_Inc._USB-Serial_Controller-if00-port0`
  — the Makefile's `monitor` target does. `getent hosts <mdns>` can stall ~10 s
  on this host, so the Makefile wraps it in `timeout 5`; pass `make ota IP=…`
  to skip discovery entirely.
- **Boot loop with `rst:0xc (RTC_SW_CPU_RST)` and only ROM output** (no
  `I (n) boot:` lines, no app banner): the Arduino bootloader double-tap
  window is firing every cycle — the RESET tab is pressed/held (the OTG +
  power-injection assembly leaning on it is the usual culprit), **or the
  double-tap flag is stuck in RTC SRAM**. A CPU reset — which is what an
  OTA reboot does — preserves that flag, so OTA can never clear it: only a
  full power cycle (unplug, ~10 s, replug, no button touch) does. Symptom
  check: 98 cycles / 85 s on the cable, and `dmesg` shows a fresh USB
  attach every ~1.5 s. Fix: power cycle, or clear the button, or
  double-tap RESET (green pulsing) + `make usb`.
- **Which USB identity the board shows is the state readout** (`lsusb` /
  `dmesg`): `2341:0070` = an app that owns the port as a *device* (TinyUSB
  CDC — the old blink_ota sketch). `303a:1001` "USB JTAG/serial debug unit"
  = the Arduino/IDF **bootloader** or the hwcdc console. A `new full-speed
  USB device` line with **no `New USB device found`** follow-up = the
  connector is a bare host PHY with no descriptors, i.e. **this sketch is
  running in host mode** and the PC (also a host) sees nothing to identify.
  Absence of `2341:0070` while the sketch runs is expected and is the
  "it is alive" signal.

## Bench test (verified 2026-11-07)

The real keyboard chain works end to end: Logitech `046D:C313` mounts at
**low speed**, `reports` climbs ~2/s (8 ms low-speed endpoint), raw reports
decode exactly (`LS`+`04` = Shift+A, `LC`+`04` = Ctrl+A, `LA`+`04` = Alt+A),
heap stays flat at 269 kB, and `make ota` with the keyboard cable still
attached rebooted into build 2 with the keyboard remounted at uptime 50 s.
With no keyboard the console still proves the stack: `Host ready.` plus
`kbd=none` status lines forever. A second-hand composite device (mouse+
keyboard) exercises the descriptor walk: the first HID interface with an
interrupt-IN endpoint wins, which is the keyboard interface on most
composites.

## Style

Arduino community consensus, matching `~/nano_esp32_blink_ota` and
`~/nano_r4_usb_host_keyboard`: 2-space indent, camelCase locals, UPPER_SNAKE
defines, Allman-ish braces on functions, `Serial1.printf` for the console.
`extern "C"` host callbacks live in the `.ino`; the prototype-generator trap
applies to device-mode descriptor callbacks only, not this host sketch.

## Files

| File | Purpose |
|------|---------|
| `nano_esp32_usb_host_keyboard.ino` | host stack + HID client + console + Wi-Fi/OTA |
| `sketch.yaml` | pins `arduino:esp32:nano_nora:USBMode=hwcdc`, core `2.0.18-arduino.5` |
| `credentials.h` | Wi-Fi SSID/password — **gitignored** |
| `credentials.example.h` | template |
| `tools/console.py` | pyserial monitor for the PL2303 (legacy-mode) cable |
| `Makefile` | `compile` / `usb` / `ota` / `monitor` |
| `README.md` | top half = Arduino IDE 2 users (core to install, non-default board options, OTA via **Tools → Port**); below the `# Maintainer notes` rule = `arduino-cli`, internals, traps |
