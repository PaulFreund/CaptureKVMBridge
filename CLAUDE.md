# CaptureKVMBridge Board Configuration Notes

The following settings are specific to the target hardware and must be preserved:

## Board-specific sdkconfig settings

| Setting | Value | Reason |
|---|---|---|
| `CONFIG_APP_BOARD_RGB_LED_GPIO` | `38` | RGB LED is on GPIO38 (not the default 48) |
| `CONFIG_APP_UART_BAUDRATE` | `2000000` | 2 Mbaud — max supported by CH340 USB-serial chip |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `16MB` | Flash chip is 16 MB |

These are set in both `sdkconfig.defaults` and `sdkconfig`.

## Flashing

Use `C:\espressif\do_flash.bat` to build and flash. It clears MSYSTEM environment variables that conflict with ESP-IDF when running from Git Bash, then calls `idf.py fullclean && idf.py -p COM5 -b 2000000 build flash`.

COM5 must be free (close CaptureKVM first).
