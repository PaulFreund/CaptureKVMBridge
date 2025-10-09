| Supported Targets | ESP32-P4 |
| ----------------- | -------- |

# CaptureKVMBridge [![Implemented with Codex](https://img.shields.io/badge/Implemented%20with-Codex-6A5ACD?logo=openai&logoColor=white)](https://github.com/openai/codex)

CaptureKVMBridge turns the ESP32-P4-WIFI6-Touch-LCD-4B panel into a USB bridge for the
[CaptureKVM](https://github.com/PaulFreund/CaptureKVM) desktop application. Any other ESP32-P4 board should work as well. Please be aware that I needed to solder two 0Ohm resistors to the board and create a custom cable, so keep that in mind when choosing a board. The firmware exposes the
board's high-speed USB OTG port as a composite device (keyboard, mouse, and 48 kHz mono microphone)
so you can interact with a remote machine, while the full-speed USB port accepts command frames from
the PC side without requiring any custom drivers on Windows.

## Firmware Highlights

- High-speed USB: enumerates as a HID keyboard, HID mouse (relative + absolute), and USB audio
  microphone so the target PC sees standard input peripherals.
- Full-speed USB: presents a CDC ACM channel that receives TLV command frames from CaptureKVM;
  keyboard, mouse, and microphone data are forwarded with a latency-optimised scheduling pipeline.
- Remote wake support: when the host PC suspends with wake enabled, incoming keyboard or mouse
  activity triggers a USB remote wakeup so the target machine powers back on seamlessly.
- Touch display UI: LVGL dashboard with a dark theme that shows USB state and per-feature activity
  indicators at a glance.
- Configurable USB descriptors, microphone buffering, and TLV payload limits via Kconfig options.

## Control Protocol

The TLV framing, field definitions, and host-side tooling live in the
[CaptureKVM desktop repository](https://github.com/PaulFreund/CaptureKVM). Install the application,
connect the board's full-speed USB port to the controlling PC, and the high-speed port to the target
PC you want to drive. The desktop app handles all packet formatting and streaming.

## Building the Firmware

```bash
idf.py set-target esp32p4
idf.py reconfigure
idf.py build flash monitor
```

Make sure both USB ports are connected: full-speed to the controlling computer running CaptureKVM and
high-speed to the destination system that should receive the emulated peripherals.

## License

This project is licensed under the terms of the MIT License. See the `LICENSE` file for details.
