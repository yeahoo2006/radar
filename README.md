# ESP32 + LD2451 BLE prototype

ESP-IDF 5.5.5 firmware for an ESP32 development board. The LD2451 uses UART2 at 115200 baud (radar TX to ESP32 GPIO16/RX2; radar RX to GPIO17/TX2); radar OT1/GPIO1 goes to ESP32 GPIO14. The ESP32 USB serial log is 115200 baud. The board advertises as `LD2451-ESP32` over BLE.

The BLE service reports live targets and reads the radar's stored configuration. A paired BLE client can set these six radar parameters: maximum distance, direction, minimum speed, no-target delay, effective trigger count, and SNR threshold. The firmware verifies the write by querying the radar again. UART baud rate is not changed. See [PROTOCOL.md](PROTOCOL.md) for UUIDs, byte layouts, allowed ranges, and result codes.

## PC command-line client

On this Windows PC, the existing ESP-IDF Python 3.11 environment can run the client with the local `vendor` copy of Bleak:

```powershell
$python = 'C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe'
& $python D:\radar\ld2451_ble\pc_config.py scan
& $python D:\radar\ld2451_ble\pc_config.py read
& $python D:\radar\ld2451_ble\pc_config.py set --distance 40 --direction both --speed 2 --delay 2 --triggers 1 --snr 0
```

On another PC, install Python and run `python -m pip install -r requirements.txt` before using `python pc_config.py`.

`set` supplies all six values and requires BLE pairing. The Windows pairing prompt may appear. `--direction` accepts `away`, `approach`, or `both`; `--snr 0` selects the radar's default level 4. If multiple matching boards are nearby, pass the address shown by `scan` with `--address`. The client reports the radar ACK, the readback, and any failed command stage.

A BLE scanner app on a phone can also read Config and Live. To configure from a generic BLE app, pair with the ESP32, write the 10-byte request to Config Write using Write Request, then read or subscribe to Config Result and read Config again. The wire format is in [PROTOCOL.md](PROTOCOL.md).

## Build and recovery

Build from an ESP-IDF 5.5.5 shell with `idf.py -C D:\radar\ld2451_ble build`. Flash with `idf.py -C D:\radar\ld2451_ble -p COM5 flash`, substituting the actual USB port if different. The previous ESP32 AT firmware backup is `../ld2451_espidf_test/at_firmware_backup_4mb.bin`.

Verified on 2026-10-10: the write-enabled app built and flashed to COM5; the boot log in `startup_write_capture.txt` shows both configuration queries, BLE advertising, and recurring radar reports. A Windows BLE client discovered the board, read Config, then sent the current six values through an encrypted write. The radar replied, readback matched all requested bytes, and Config Result reported success (`state=2`, valid flags `0x03`). This test exercised the full write path without changing the detection settings.
