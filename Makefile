NAME      := nano-esp32-kbd
PROFILE   := nano_esp32
MDNS      := $(NAME).local
# timeout: getent can stall for ~10 s when the resolver has no mDNS answer.
IP        := $(shell timeout 5 getent hosts $(MDNS) 2>/dev/null | awk '{print $$1; exit}')
PORT      := $(shell ls /dev/ttyACM0 2>/dev/null)
# The PL2303 USB-UART console cable. udev's by-id name beats a bare ttyUSB0.
CONSOLE   := $(shell ls /dev/serial/by-id/usb-Prolific* 2>/dev/null | head -1)
CONSOLE   := $(if $(CONSOLE),$(CONSOLE),/dev/ttyUSB0)

.PHONY: compile usb ota monitor

compile:
	arduino-cli compile --profile $(PROFILE) .

## Flash over the USB cable (DFU). Needed once, before the keyboard cable is
## attached, and any time Wi-Fi/OTA is unreachable. If the running sketch owns
## the USB port, double-tap RESET (RGB pulses green) to reach the bootloader.
usb: compile
	arduino-cli upload --profile $(PROFILE) -p $(if $(PORT),$(PORT),/dev/ttyACM0) .

## Flash over Wi-Fi — no cable, works while the keyboard cable is attached.
## Resolves the board's mDNS name first.
ota: compile
	@test -n "$(IP)" || { echo "no mDNS answer for $(MDNS) — is the board powered and on Wi-Fi?"; exit 1; }
	echo "OTA push to $(IP)"
	arduino-cli upload --profile $(PROFILE) -p $(IP) --protocol network -F password= --discovery-timeout 15s . 2>&1 | tail -2

## Watch the console: PL2303 USB-UART cable on D0 (RX), D1 (TX), GND. 115200.
monitor:
	python3 tools/console.py --port $(CONSOLE) --seconds 20
