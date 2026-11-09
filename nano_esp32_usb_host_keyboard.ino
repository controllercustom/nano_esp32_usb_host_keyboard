/* SPDX-License-Identifier: MIT */
/*
 * USB host keyboard — Arduino Nano ESP32
 * --------------------------------------
 * Reads a plain USB keyboard through the **ESP-IDF USB Host Library**
 * (esp_usb_host, "usb/usb_host.h") — not TinyUSB — and prints the raw HID
 * boot-protocol reports on a hardware-UART console.
 *
 *   USB-C port : host port. The keyboard hangs off it via an OTG Type-C→A
 *                cable plus a USB power-injection cable. While the host
 *                stack owns the PHY there is no CDC serial and no DFU:
 *                updates go over the air instead.
 *   Serial1    : console. The PL2303 USB-UART cable goes to D0 (RX, GPIO44),
 *                D1 (TX, GPIO43) and GND. 115200 8N1.
 *                The ROM/IDF boot console lives on UART0 at these same pins;
 *                Serial1.begin() re-routes them, so the boot log is visible
 *                on the cable until the app takes over.
 *   ArduinoOTA : Wi-Fi, mDNS nano-esp32-kbd.local, UDP 3232.
 *
 * Build with USBMode=hwcdc (see sketch.yaml) so TinyUSB never starts.
 * Never call USB.begin(), and never call Serial.begin()/Serial.print —
 * HWCDC would claim the USB peripheral that the host stack needs.
 */

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>

#include "soc/soc_caps.h"

#if !SOC_USB_OTG_SUPPORTED
#error "This sketch needs a target with a USB OTG peripheral, such as the ESP32-S3 on the Nano ESP32."
#endif

#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

#include "credentials.h"  // WIFI_SSID / WIFI_PASS — not tracked by git

// Bump on every OTA push: the console status line proves which build runs.
#ifndef SKETCH_VERSION
#define SKETCH_VERSION "3 (esp_usb_host keyboard demo)"
#endif

// mDNS name used as the upload port once OTA is live.
static const char OTA_HOSTNAME[] = "nano-esp32-kbd";
static const uint16_t OTA_PORT = 3232;

// Console UART1: D0 = RX (GPIO44), D1 = TX (GPIO43) — the PL2303 cable.
#define CONSOLE_RX_PIN D0
#define CONSOLE_TX_PIN D1
#define CONSOLE_BAUD   115200

static const uint32_t BLINK_MS     = 180;    // heartbeat half-period, Wi-Fi up
static const uint32_t FAULT_BLINK  = 100;    // heartbeat half-period, Wi-Fi down
static const uint32_t ASSOCIATE_MS = 20000;  // max wait for an association
static const uint32_t RETRY_MS     = 10000;  // pause between reconnection attempts
static const uint32_t STATUS_MS    = 5000;   // one status line per period

// A first limited run of Nano ESP32 boards has green/blue swapped on the RGB LED.
#ifndef RGB_SWAP_GB
#define RGB_SWAP_GB 0
#endif

// ---------------------------------------------------------------------------
// USB host: one HID keyboard, one client.
// ---------------------------------------------------------------------------

#define REPORT_MAX_BYTES 16  // boot keyboards send 8; allow a little more

struct KbdReport {
  uint8_t len;
  uint8_t bytes[REPORT_MAX_BYTES];
};

// Deferred work for the client task. Events must not do the heavy lifting:
// the ESP-IDF docs forbid blocking inside the client event callback.
#define ACT_OPEN_DEV 0x01u
#define ACT_CLOSE_DEV 0x02u

static usb_host_client_handle_t sClient = NULL;
static usb_device_handle_t sDev = NULL;
static usb_transfer_t *sInXfer = NULL;

static volatile uint32_t sActions = 0;
static volatile uint8_t sNewDevAddr = 0;
static usb_device_handle_t sGoneDev = NULL;

static uint8_t sKbdIface = 0;   // claimed HID interface number
static uint8_t sInEpAddr = 0;   // its interrupt IN endpoint address
static uint8_t sInEpMps = 0;    // endpoint max packet size

// Consecutive non-fatal IN failures, and the flag set once we stop re-arming.
// Only the client task touches these.
static uint32_t sInErrStreak = 0;
static bool sInErrGaveUp = false;

static QueueHandle_t sReportQ = NULL;
static volatile uint32_t sReportCount = 0;

// Last mount identity, for the status line.
static bool sKbdMounted = false;
static uint16_t sKbdVid = 0;
static uint16_t sKbdPid = 0;
static const char *sKbdSpeed = "";

static const char *speedName(usb_speed_t speed) {
  switch (speed) {
    case USB_SPEED_LOW:  return "low";
    case USB_SPEED_FULL: return "full";
    default:             return "unknown";
  }
}

// Find the first HID interface with an interrupt IN endpoint in the active
// configuration descriptor. Fills iface / epAddr / mps.
static bool findHidKeyboardInterface(const usb_config_desc_t *cfg, uint8_t *iface, uint8_t *epAddr, uint8_t *mps) {
  int offset = 0;
  const usb_standard_desc_t *cur = (const usb_standard_desc_t *)cfg;
  const usb_intf_desc_t *intf = (const usb_intf_desc_t *)usb_parse_next_descriptor_of_type(
    cur, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset);

  while (intf != NULL) {
    if (intf->bInterfaceClass == USB_CLASS_HID && intf->bNumEndpoints > 0) {
      // Walk the descriptors that belong to this interface: everything until
      // the next INTERFACE descriptor (or the end of the configuration).
      int epOffset = offset;
      const usb_standard_desc_t *next = (const usb_standard_desc_t *)intf;
      for (;;) {
        next = usb_parse_next_descriptor(next, cfg->wTotalLength, &epOffset);
        if (next == NULL || next->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
          break;
        }
        if (next->bDescriptorType != USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
          continue;  // HID class descriptor (0x21), report descriptor (0x22), ...
        }
        const usb_ep_desc_t *ep = (const usb_ep_desc_t *)next;
        if (USB_EP_DESC_GET_EP_DIR(ep) == 1 && USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_INTR) {
          *iface = intf->bInterfaceNumber;
          *epAddr = ep->bEndpointAddress;
          *mps = (uint8_t)USB_EP_DESC_GET_MPS(ep);
          return true;
        }
      }
    }
    intf = (const usb_intf_desc_t *)usb_parse_next_descriptor_of_type(
      (const usb_standard_desc_t *)intf, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset);
  }
  return false;
}

// Transfer completion. Runs on the client task (inside
// usb_host_client_handle_events) — keep it short, hand the report to loop().
static const char *xferStatusName(usb_transfer_status_t st) {
  switch (st) {
    case USB_TRANSFER_STATUS_COMPLETED: return "completed";
    case USB_TRANSFER_STATUS_ERROR:     return "error";
    case USB_TRANSFER_STATUS_TIMED_OUT: return "timed out";
    case USB_TRANSFER_STATUS_CANCELED:  return "canceled";
    case USB_TRANSFER_STATUS_STALL:     return "stall";
    case USB_TRANSFER_STATUS_OVERFLOW:  return "overflow";
    case USB_TRANSFER_STATUS_SKIPPED:   return "skipped";
    case USB_TRANSFER_STATUS_NO_DEVICE: return "no device";
    default:                            return "unknown";
  }
}

// A live device that keeps failing would otherwise stop reporting for good:
// re-arm on recoverable statuses, but give up after this many in a row so a
// dead-but-present endpoint cannot spin the client task forever.
#define IN_ERR_CAP 50

static void inTransferDone(usb_transfer_t *transfer) {
  const usb_transfer_status_t st = transfer->status;

  if (st == USB_TRANSFER_STATUS_COMPLETED) {
    sInErrStreak = 0;
    sInErrGaveUp = false;
    KbdReport r;
    r.len = (uint8_t)(transfer->actual_num_bytes > REPORT_MAX_BYTES ? REPORT_MAX_BYTES : transfer->actual_num_bytes);
    memcpy(r.bytes, transfer->data_buffer, r.len);
    xQueueSend(sReportQ, &r, 0);
    sReportCount++;
  } else if (st == USB_TRANSFER_STATUS_STALL) {
    Serial1.println("[usbh] IN endpoint stalled, clearing");
    if (sDev != NULL) {
      usb_host_endpoint_halt(sDev, transfer->bEndpointAddress);
      usb_host_endpoint_clear(sDev, transfer->bEndpointAddress);
    }
  } else if (st == USB_TRANSFER_STATUS_NO_DEVICE) {
    // The keyboard was pulled: DEV_GONE follows and closes the device, so a
    // re-submit here would only fail again.
    Serial1.println("[usbh] IN transfer no device (pulled) — not re-arming");
    return;
  } else {
    sInErrStreak++;
    Serial1.printf("[usbh] IN transfer %s (%u in a row)\n", xferStatusName(st),
                   (unsigned)sInErrStreak);
    if (sInErrStreak >= IN_ERR_CAP) {
      if (!sInErrGaveUp) {
        sInErrGaveUp = true;
        Serial1.printf("[usbh] IN endpoint stuck after %u errors — unplug the keyboard to recover\n",
                       (unsigned)IN_ERR_CAP);
      }
      return;
    }
  }

  // Re-arm: without this the keyboard reports exactly once.
  transfer->num_bytes = sInEpMps;
  if (usb_host_transfer_submit(transfer) != ESP_OK) {
    Serial1.println("[usbh] re-arm failed");
  }
}

static void openKeyboard(uint8_t address) {
  if (sDev != NULL) {
    Serial1.println("[usbh] keyboard already mounted, ignoring new device");
    return;
  }

  usb_device_handle_t dev = NULL;
  esp_err_t err = usb_host_device_open(sClient, address, &dev);
  if (err != ESP_OK) {
    Serial1.printf("[usbh] open device %u failed: %s\n", address, esp_err_to_name(err));
    return;
  }

  const usb_device_desc_t *desc = NULL;
  usb_device_info_t info;
  if (usb_host_device_info(dev, &info) != ESP_OK || usb_host_get_device_descriptor(dev, &desc) != ESP_OK) {
    Serial1.println("[usbh] could not read descriptors");
    usb_host_device_close(sClient, dev);
    return;
  }
  Serial1.printf("[usbh] device %u: %04X:%04X, %s speed, configuration %u\n",
                 address, desc->idVendor, desc->idProduct, speedName(info.speed), info.bConfigurationValue);

  const usb_config_desc_t *cfg = NULL;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
    Serial1.println("[usbh] could not read configuration descriptor");
    usb_host_device_close(sClient, dev);
    return;
  }

  uint8_t iface = 0, epAddr = 0, mps = 0;
  if (!findHidKeyboardInterface(cfg, &iface, &epAddr, &mps)) {
    Serial1.println("[usbh] no HID interrupt-IN interface — not a keyboard?");
    usb_host_device_close(sClient, dev);
    return;
  }

  err = usb_host_interface_claim(sClient, dev, iface, 0);
  if (err != ESP_OK) {
    Serial1.printf("[usbh] claim interface %u failed: %s\n", iface, esp_err_to_name(err));
    usb_host_device_close(sClient, dev);
    return;
  }

  // IN transfers must request an integer multiple of the endpoint MPS.
  err = usb_host_transfer_alloc(mps, 0, &sInXfer);
  if (err != ESP_OK) {
    Serial1.printf("[usbh] transfer alloc failed: %s\n", esp_err_to_name(err));
    usb_host_interface_release(sClient, dev, iface);
    usb_host_device_close(sClient, dev);
    return;
  }

  sInXfer->device_handle = dev;
  sInXfer->bEndpointAddress = epAddr;
  sInXfer->callback = inTransferDone;
  sInXfer->context = NULL;
  sInXfer->num_bytes = mps;

  sKbdIface = iface;
  sInEpAddr = epAddr;
  sInEpMps = mps;
  sInErrStreak = 0;
  sInErrGaveUp = false;

  if (usb_host_transfer_submit(sInXfer) != ESP_OK) {
    Serial1.println("[usbh] first IN submit failed");
    usb_host_transfer_free(sInXfer);
    sInXfer = NULL;
    usb_host_interface_release(sClient, dev, iface);
    usb_host_device_close(sClient, dev);
    return;
  }

  sDev = dev;
  sKbdVid = desc->idVendor;
  sKbdPid = desc->idProduct;
  sKbdSpeed = speedName(info.speed);
  sKbdMounted = true;

  Serial1.printf("[kbd] mounted: %04X:%04X %s speed, interface %u, IN ep 0x%02X, mps %u\n",
                 sKbdVid, sKbdPid, sKbdSpeed, sKbdIface, sInEpAddr, sInEpMps);
  Serial1.println("[kbd] modifier bits: LC LS LA LG RC RS RA RG — keys are raw HID usage IDs");
}

static void closeKeyboard(void) {
  usb_device_handle_t dev = sGoneDev != NULL ? sGoneDev : sDev;
  sGoneDev = NULL;
  if (dev == NULL) {
    return;
  }
  if (sInXfer != NULL) {
    usb_host_transfer_free(sInXfer);  // a gone device cannot complete transfers
    sInXfer = NULL;
  }
  usb_host_interface_release(sClient, dev, sKbdIface);
  usb_host_device_close(sClient, dev);
  sDev = NULL;
  sKbdMounted = false;
  Serial1.println("[kbd] removed");
}

static void clientEvent(const usb_host_client_event_msg_t *msg, void *) {
  switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      sNewDevAddr = msg->new_dev.address;
      sActions |= ACT_OPEN_DEV;
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      sGoneDev = msg->dev_gone.dev_hdl;
      sActions |= ACT_CLOSE_DEV;
      break;
    default:
      break;
  }
}

// Library daemon: enumeration, control transfer routing, event forwarding.
static void daemonTask(void *) {
  for (;;) {
    uint32_t eventFlags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &eventFlags);
    if (eventFlags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

static void clientTask(void *) {
  for (;;) {
    usb_host_client_handle_events(sClient, portMAX_DELAY);
    uint32_t actions = sActions;
    sActions = 0;
    if (actions & ACT_CLOSE_DEV) {
      closeKeyboard();
    }
    if (actions & ACT_OPEN_DEV) {
      openKeyboard(sNewDevAddr);
    }
  }
}

#if defined(CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK) && CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
// Core 3.3.11+ builds the prebuilt libs with this option, where a NULL filter
// silently rejects every device. It is off on arduino:esp32 2.0.18, so this
// whole block compiles away there.
static bool enumFilterAllowAll(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
  (void)dev_desc;
  *bConfigurationValue = 1;
  return true;
}
#endif

static bool startUsbHost() {
  usb_host_config_t hostConfig = {};
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;
#if defined(CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK) && CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
  hostConfig.enum_filter_cb = enumFilterAllowAll;
#endif

  esp_err_t err = usb_host_install(&hostConfig);
  if (err != ESP_OK) {
    Serial1.printf("[usbh] usb_host_install failed: %s\n", esp_err_to_name(err));
    return false;
  }

  usb_host_client_config_t clientConfig = {};
  clientConfig.is_synchronous = false;
  clientConfig.max_num_event_msg = 5;
  clientConfig.async.client_event_callback = clientEvent;
  clientConfig.async.callback_arg = NULL;

  err = usb_host_client_register(&clientConfig, &sClient);
  if (err != ESP_OK) {
    Serial1.printf("[usbh] usb_host_client_register failed: %s\n", esp_err_to_name(err));
    return false;
  }

  xTaskCreate(daemonTask, "usbh_daemon", 4096, NULL, 2, NULL);
  xTaskCreate(clientTask, "usbh_client", 4096, NULL, 3, NULL);
  return true;
}

// ---------------------------------------------------------------------------
// Console rendering of one raw HID boot report
// ---------------------------------------------------------------------------

static const char *const kModNames[8] = {"LC", "LS", "LA", "LG", "RC", "RS", "RA", "RG"};

static void printReport(const KbdReport &r) {
  char mods[32] = "";
  if (r.len >= 1 && r.bytes[0] != 0) {
    for (int bit = 0; bit < 8; bit++) {
      if (r.bytes[0] & (1u << bit)) {
        if (mods[0] != '\0') {
          strcat(mods, " ");
        }
        strcat(mods, kModNames[bit]);
      }
    }
  }
  // Boot keyboard report: [mods, reserved, key x6]. Anything longer (composite
  // report descriptors) is dumped as raw hex instead.
  if (r.len >= 8) {
    char keys[6 * 3];
    keys[0] = '\0';
    for (int i = 0; i < 6; i++) {
      char cell[4];
      snprintf(cell, sizeof(cell), (i == 5) ? "%02X" : "%02X ", r.bytes[2 + i]);
      strcat(keys, cell);
    }
    Serial1.printf("kbd  %-16s  keys %s   (%u reports)\n", mods[0] ? mods : "-", keys, (unsigned)sReportCount);
  } else {
    char hex[REPORT_MAX_BYTES * 3 + 1];
    hex[0] = '\0';
    for (int i = 0; i < r.len; i++) {
      char cell[4];
      snprintf(cell, sizeof(cell), (i == r.len - 1) ? "%02X" : "%02X ", r.bytes[i]);
      strcat(hex, cell);
    }
    Serial1.printf("kbd  %-16s  raw  %s   (%u reports)\n", mods[0] ? mods : "-", hex, (unsigned)sReportCount);
  }
}

// ---------------------------------------------------------------------------
// Wi-Fi + OTA
// ---------------------------------------------------------------------------

enum class Net : uint8_t { Connecting, Connected, Down };

static Net net = Net::Connecting;
static bool otaStarted = false;
static bool otaRunning = false;
static bool ledOn = false;
static uint32_t lastToggle = 0;
static uint32_t lastRetry = 0;
static uint32_t lastStatus = 0;
static uint8_t lastProgressTick = 0;

static void rgb(bool red, bool green, bool blue) {
#if RGB_SWAP_GB
  const bool tmp = green;
  green = blue;
  blue = tmp;
#endif
  digitalWrite(LED_RED, red ? LOW : HIGH);
  digitalWrite(LED_GREEN, green ? LOW : HIGH);
  digitalWrite(LED_BLUE, blue ? LOW : HIGH);
}

static void updateStatusLed() {
  if (otaRunning) {
    return;  // OTA owns the RGB LED while writing an image
  }
  switch (net) {
    case Net::Connecting: rgb(true, false, false); break;  // red
    case Net::Connected:  rgb(false, true, false); break;  // green
    case Net::Down:       rgb(false, false, false); break;  // off
  }
}

static void startOta() {
  if (otaStarted) {
    return;
  }

  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  // No password: anyone on this LAN may push a sketch. Uncomment to lock it:
  // ArduinoOTA.setPassword("change-me");

  ArduinoOTA.onStart([]() {
    otaRunning = true;
    lastProgressTick = 0;
    rgb(false, false, true);  // blue
    Serial1.println("\nOTA: update started, leaving the old sketch running");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    const uint8_t percent = (uint8_t)(progress / (total / 100));
    if (percent >= lastProgressTick + 10 || percent == 100) {
      lastProgressTick = percent;
      Serial1.printf("OTA: %3u%% (%u/%u bytes)\n", percent, progress, total);
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    otaRunning = false;
    Serial1.printf("OTA: error %u (%s)\n", (unsigned)error,
                   error == OTA_AUTH_ERROR      ? "wrong password"
                   : error == OTA_BEGIN_ERROR   ? "could not open the idle app partition"
                   : error == OTA_CONNECT_ERROR ? "uploader disconnected too early"
                   : error == OTA_RECEIVE_ERROR ? "incomplete transfer"
                                                : "could not finalise the image");
    updateStatusLed();
  });

  ArduinoOTA.onEnd([]() {
    Serial1.printf("OTA: image verified — rebooting into the new sketch (heap %u bytes free)\n",
                   ESP.getFreeHeap());
  });

  ArduinoOTA.begin();  // publishes _arduino._tcp over mDNS
  otaStarted = true;

  Serial1.printf("OTA: listening on port %u\n", OTA_PORT);
  Serial1.printf("OTA: push updates with\n"
                 "       make ota\n"
                 "   or  arduino-cli compile --profile nano_esp32 . && \\\n"
                 "       arduino-cli upload  --profile nano_esp32 -p %s.local --protocol network -F password= .\n",
                 OTA_HOSTNAME);
}

static void connectToWifi() {
  Serial1.printf("Wi-Fi: associating with \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < ASSOCIATE_MS) {
    delay(200);
    Serial1.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    net = Net::Connected;
    Serial1.println();
    Serial1.printf("Wi-Fi: connected — %s, RSSI %d dBm, channel %d\n",
                   WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
    Serial1.printf("Wi-Fi: MAC %s\n", WiFi.macAddress().c_str());
    startOta();
  } else {
    net = Net::Down;
    Serial1.println("\nWi-Fi: association timed out — will keep retrying in loop()");
    Serial1.println("Wi-Fi: double-tap RESET for the DFU bootloader if the network never comes back");
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial1.begin(CONSOLE_BAUD, SERIAL_8N1, CONSOLE_RX_PIN, CONSOLE_TX_PIN);
  delay(150);  // let the USB-UUART adapter settle before the first line

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  rgb(false, false, false);

  Serial1.printf("\nNano ESP32 USB host keyboard — build %s\n", SKETCH_VERSION);
  Serial1.printf("Chip %s rev %u, %u MHz, flash %u MB, PSRAM %u bytes\n",
                 ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz(),
                 ESP.getFlashChipSize() / 1024 / 1024, ESP.getPsramSize());
  Serial1.println("Stack: ESP-IDF USB Host Library (esp_usb_host), not TinyUSB");

  sReportQ = xQueueCreate(8, sizeof(KbdReport));

  if (!startUsbHost()) {
    Serial1.println("[usbh] host stack failed — check USBMode=hwcdc; keyboard will not work");
  } else {
    Serial1.println("Host ready. Plug in a USB keyboard (via power-injection cable).");
  }

  connectToWifi();
  updateStatusLed();
  lastRetry = millis();
}

void loop() {
  ArduinoOTA.handle();

  const uint32_t now = millis();
  const bool associated = (WiFi.status() == WL_CONNECTED);

  if (net == Net::Connected && !associated) {
    Serial1.println("\nWi-Fi: connection dropped");
    net = Net::Down;
    lastRetry = now;
    updateStatusLed();
  } else if (net != Net::Connected && associated) {
    net = Net::Connected;
    Serial1.printf("\nWi-Fi: back up — %s, RSSI %d dBm\n",
                   WiFi.localIP().toString().c_str(), WiFi.RSSI());
    startOta();
    updateStatusLed();
  } else if (net == Net::Down && now - lastRetry >= RETRY_MS) {
    lastRetry = now;
    Serial1.print("Wi-Fi: retrying");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    net = Net::Connecting;
    updateStatusLed();
  }

  // Drain HID reports and print the ones that changed (idle repeat reports are
  // identical and would flood the console).
  static KbdReport last = {};
  static bool lastValid = false;
  KbdReport r;
  while (xQueueReceive(sReportQ, &r, 0) == pdTRUE) {
    if (!lastValid || memcmp(&last, &r, sizeof(last)) != 0) {
      last = r;
      lastValid = true;
      printReport(r);
    }
  }
  if (!sKbdMounted && lastValid) {
    lastValid = false;  // a fresh mount should print its first report
  }

  const uint32_t period = (net == Net::Connected) ? BLINK_MS : FAULT_BLINK;
  if (now - lastToggle >= period) {
    lastToggle = now;
    ledOn = !ledOn;
    digitalWrite(LED_BUILTIN, ledOn ? HIGH : LOW);
  }

  if (now - lastStatus >= STATUS_MS) {
    lastStatus = now;
    char kbd[48];
    if (sKbdMounted) {
      snprintf(kbd, sizeof(kbd), "mounted %04X:%04X %s", sKbdVid, sKbdPid, sKbdSpeed);
    } else {
      snprintf(kbd, sizeof(kbd), "none");
    }
    Serial1.printf("[%6lu s] build %s | kbd=%s reports=%u | net=%s | ip=%s rssi=%d heap=%u ota=%s.local:%u\n",
                   (unsigned long)(now / 1000), SKETCH_VERSION, kbd, (unsigned)sReportCount,
                   net == Net::Connected    ? "connected"
                   : net == Net::Connecting ? "associating"
                                            : "down",
                   WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap(),
                   OTA_HOSTNAME, OTA_PORT);
  }
}
