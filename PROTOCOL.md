# LD2451 BLE protocol, version 1

## Roles and transport

- ESP32 is a BLE peripheral and GATT server. The phone is a BLE central/client.
- Advertising name: `LD2451-ESP32`.
- Service UUID: `3a56c3b0-0c7a-4fd9-8e9e-0d2451000000`.
- All multi-byte integers are unsigned little-endian. Version byte is `0x01`.
- BLE reads and notifications work without pairing. Configuration writes require an encrypted BLE connection. The ESP32 uses LE Secure Connections Just Works pairing and stores the bond.

| Characteristic | UUID | Properties | Purpose |
| --- | --- | --- | --- |
| Live | `3a56c3b0-0c7a-4fd9-8e9e-0d2451000001` | Read, Notify | Latest summary on Read; summary and target records on Notify |
| Config | `3a56c3b0-0c7a-4fd9-8e9e-0d2451000002` | Read | Current LD2451 detection and sensitivity parameters |
| Config Write | `3a56c3b0-0c7a-4fd9-8e9e-0d2451000003` | Write with response, encrypted | Queue all six detection/sensitivity settings |
| Config Result | `3a56c3b0-0c7a-4fd9-8e9e-0d2451000004` | Read, Notify | Asynchronous write outcome and radar readback |

Each notification is 10 or 12 bytes and fits the default BLE ATT MTU of 23 (20 value bytes). No application-level fragmentation is required.

## Live summary (`type=0x10`, 12 bytes)

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 1 | Version | `1` |
| 1 | 1 | Type | `0x10` |
| 2 | 2 | Sequence | Increments per valid LD2451 report; wraps at 65535 |
| 4 | 1 | Target count | Number of target records following, 0-5 |
| 5 | 1 | Alarm | LD2451 approaching alarm, 0 or 1 |
| 6 | 1 | OT1 | Electrical level of LD2451 GPIO1 sampled on ESP32 GPIO14, 0 or 1 |
| 7 | 1 | Reserved | Zero; ignore on reception |
| 8 | 4 | Uptime | ESP32 milliseconds since boot when report was processed; wraps at 2^32 |

A Read of Live returns the latest summary. Uptime zero before the first radar report. Each accepted radar report updates the readable summary. Notifications are limited to at most 5 snapshots per second; intermediate reports are replaced by newer data. An empty radar report produces a summary with target count zero.

## Live target (`type=0x11`, 10 bytes)

One target notification follows the summary for each reported target. All target records from one radar report have the same sequence as its summary.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 1 | Version | `1` |
| 1 | 1 | Type | `0x11` |
| 2 | 2 | Sequence | Matches the preceding summary |
| 4 | 1 | Index | Zero-based target index |
| 5 | 1 | Angle raw | Degrees = unsigned byte value minus 128 |
| 6 | 1 | Distance | Metres |
| 7 | 1 | Direction | `1` approaching, `0` receding |
| 8 | 1 | Speed | km/h |
| 9 | 1 | SNR | Raw 0-255 value from LD2451 |

The phone should start a new snapshot at each `0x10` message, accept only `0x11` records with its sequence, and display a complete target list after receiving `target count` records. If a newer summary arrives first, discard the partial list. BLE notifications have no application-level replay; the next snapshot supersedes a lost one.

## Config Read (10 bytes)

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 1 | Version | `1` |
| 1 | 1 | Valid flags | Bit 0: detection query succeeded; bit 1: sensitivity query succeeded |
| 2 | 1 | Maximum distance | Metres |
| 3 | 1 | Direction setting | `0` receding only, `1` approaching only, `2` both |
| 4 | 1 | Minimum speed | km/h |
| 5 | 1 | No-target delay | Seconds |
| 6 | 1 | Effective trigger count | Consecutive detections required |
| 7 | 1 | SNR threshold setting | `0` means firmware default level 4; explicit levels 3-8 |
| 8 | 2 | Extensions | Raw LD2451 extension bytes |

The ESP32 queries these values at boot using LD2451 commands `0x0012` and `0x0013`. Invalid groups are zeroed and identified by the valid flags. After a configuration write, the ESP32 queries both groups again and updates this characteristic with the readback.

## Config Write (10 bytes)

| Offset | Size | Field | Allowed values |
| --- | --- | --- | --- |
| 0 | 1 | Protocol version | `1` |
| 1 | 1 | Transaction ID | Chosen by client, 0-255; echoed in Result |
| 2 | 1 | Maximum distance | 10-100 m |
| 3 | 1 | Direction | `0` receding only, `1` approaching only, `2` both |
| 4 | 1 | Minimum speed | 0-120 km/h |
| 5 | 1 | No-target delay | 0-255 s |
| 6 | 1 | Effective trigger count | 1-10 |
| 7 | 1 | SNR threshold setting | `0` means firmware default 4; otherwise 3-8 |
| 8-9 | 2 | Reserved | Both zero |

Use a GATT **Write Request** (with response), not Write Without Response. A successful ATT write means the command was accepted into the ESP32 queue; it does not mean the radar accepted the settings. The radar's detection and sensitivity settings are two separate serial commands, so a failed transaction can leave a partial change. Always inspect Config Result and the Config readback. The BLE protocol never changes the LD2451 UART baud rate.

## Config Result (13 bytes)

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 1 | Protocol version | `1` |
| 1 | 1 | Transaction ID | Echo of the latest accepted Config Write |
| 2 | 1 | State | `0` idle, `1` applying, `2` verified success, `4` radar command/ACK error, `5` readback mismatch |
| 3 | 1 | Failed stage | LD2451 command byte: `FF` enable, `02` detection write, `03` sensitivity write, `12`/`13` readback, `FE` end; zero on success |
| 4 | 1 | Valid flags | Same meaning as Config byte 1 |
| 5-12 | 8 | Actual parameters | Same layout as Config bytes 2-9; only groups whose valid flags are set are trustworthy |

The ESP32 validates all six values before queuing the write. An invalid length returns ATT error `0x0D`; an invalid value returns `0x13`; a concurrent transaction returns `0x0E`. After acceptance, the ESP32 enables radar command mode, writes detection settings, writes sensitivity settings, reads both groups back, and ends command mode. The upper computer should subscribe to Result before writing and wait up to 20 seconds for state `2`, `4`, or `5` with the matching transaction ID. It may also poll Result. Treat state `2` and valid flags `0x03` as success.

## Connection behavior

1. Scan for `LD2451-ESP32`, connect, and discover the custom service.
2. Read Config to inspect the radar's boot-time parameters.
3. Read Live for the latest summary, then enable notifications on Live.
4. To change radar parameters, pair with the ESP32, subscribe to Config Result, and Write Request the 10-byte Config Write value. Check Result and read Config again.
5. Consider live data stale if no new summary arrives within 4 seconds. The LD2451 normally sends empty reports even without a target.

The prototype allows one connected client. Just Works pairing encrypts writes but does not authenticate the peer; a deployed product needs a stronger authorization method for configuration changes.
