| Supported Targets | ESP32-S3 |
| ----------------- | -------- |

# CaptureKVMBridge [![Implemented with Codex](https://img.shields.io/badge/Implemented%20with-Codex-6A5ACD?logo=openai&logoColor=white)](https://github.com/openai/codex)

CaptureKVMBridge turns an ESP32-S3 with native USB OTG into a USB bridge for the
[CaptureKVM](https://github.com/PaulFreund/CaptureKVM) desktop application. The firmware exposes the
chip's full-speed USB OTG port as a composite device (keyboard, mouse, and 48 kHz mono microphone)
so you can interact with a remote machine. Control frames can arrive over Wi-Fi or the board's second
USB serial port when it is backed by a USB-UART bridge. The legacy P4 display path is still compiled
only when targeting `esp32p4`.

## Firmware Highlights

- Full-speed USB OTG: enumerates as a HID keyboard, HID mouse (relative + absolute), and USB audio
  microphone so the target PC sees standard input peripherals.
- Wi-Fi TLV ingress: optional SoftAP + UDP listener for CaptureKVM command frames.
- USB-UART TLV ingress: uses the second USB serial port on common two-port ESP32-S3 boards, mapped
  to UART0 RX on GPIO44 by default and clocked at 3 Mbps.
- Remote wake support: when the host PC suspends with wake enabled, incoming keyboard or mouse
  activity triggers a USB remote wakeup so the target machine powers back on seamlessly.
- Optional P4-only display UI: the Waveshare LCD dashboard remains available for legacy `esp32p4`
  builds, but generic ESP32-S3 builds run headless.
- Configurable USB descriptors, microphone buffering, and TLV payload limits via Kconfig options.

## Control Protocol

The TLV framing, field definitions, and host-side tooling live in the
[CaptureKVM desktop repository](https://github.com/PaulFreund/CaptureKVM). Install the application,
connect the ESP32-S3 USB OTG port to the target PC you want to drive, and send TLV frames from the
controlling PC over the second USB serial port or the configured Wi-Fi link. The desktop app handles
all packet formatting and streaming.

## Building the Firmware

```bash
idf.py set-target esp32s3
idf.py reconfigure
idf.py build flash monitor
```

Use an ESP32-S3 board with 16 MB flash. Connect the native USB OTG port (GPIO19/GPIO20) to the
destination system that should receive the emulated peripherals, and connect the second USB serial
port to the controlling PC running CaptureKVM. The default S3 configuration expects that serial port
to be backed by a USB-UART bridge wired to UART0 RX on GPIO44. USB Serial/JTAG TLV ingress remains
disabled on ESP32-S3 because it shares the native USB pins with USB OTG.

## License

This project is licensed under the terms of the MIT License. See the `LICENSE` file for details.
