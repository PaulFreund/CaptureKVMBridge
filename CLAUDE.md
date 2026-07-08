# CaptureKVMBridge Board Configuration Notes

Target board: **Guition JC-ESP32P4-M3-DEV**

## Board notes

- USB-to-UART: **CH340C** on GPIO37 (TX) / GPIO38 (RX). Max baud rate: **2,000,000 bps**.
- **No controllable LED** on this board. The visible LEDs (power, battery charge, Ethernet) are driven by external ICs, not ESP32 GPIOs.
- Flash size: 16 MB.

## Board-specific sdkconfig settings

| Setting | Value | Reason |
|---|---|---|
| `CONFIG_APP_UART_BAUDRATE` | `2000000` | CH340C hardware maximum |
| `CONFIG_APP_UART_RX_GPIO` | `38` | UART0 RXD → CH340C TXD |
| `CONFIG_APP_UART_TX_GPIO` | `37` | UART0 TXD → CH340C RXD |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `16MB` | Flash chip is 16 MB |
| `CONFIG_FREERTOS_HZ` | `1000` | 1 ms tick resolution for low-latency HID polling |

These are set in both `sdkconfig.defaults` and `sdkconfig`.

## Flashing

Use `C:\espressif\do_flash.bat` to build and flash. It clears MSYSTEM environment variables that conflict with ESP-IDF when running from Git Bash, then calls `idf.py fullclean && idf.py -p COM5 -b 2000000 build flash`.

COM5 must be free (close CaptureKVM first).
