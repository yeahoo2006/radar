"""Read and configure an ESP32 + LD2451 over BLE from a PC.

Requires: python -m pip install bleak
Examples:
  python pc_config.py scan
  python pc_config.py read
  python pc_config.py set --distance 40 --direction both --speed 2 \
      --delay 2 --triggers 1 --snr 0
"""

import argparse
import asyncio
import secrets
import sys
from pathlib import Path

_local_vendor = Path(__file__).with_name("vendor")
if _local_vendor.is_dir():
    sys.path.insert(0, str(_local_vendor))

SERVICE = "3a56c3b0-0c7a-4fd9-8e9e-0d2451000000"
CONFIG = "3a56c3b0-0c7a-4fd9-8e9e-0d2451000002"
CONFIG_WRITE = "3a56c3b0-0c7a-4fd9-8e9e-0d2451000003"
RESULT = "3a56c3b0-0c7a-4fd9-8e9e-0d2451000004"
DEVICE_NAME = "LD2451-ESP32"
DIRECTIONS = {"away": 0, "approach": 1, "both": 2}
DIRECTION_NAMES = {value: name for name, value in DIRECTIONS.items()}


def validate_values(distance, direction, speed, delay, triggers, snr):
    if not 10 <= distance <= 100:
        raise ValueError("distance must be 10..100 m")
    if not 0 <= speed <= 120:
        raise ValueError("speed must be 0..120 km/h")
    if not 0 <= delay <= 255:
        raise ValueError("delay must be 0..255 s")
    if not 1 <= triggers <= 10:
        raise ValueError("triggers must be 1..10")
    if snr != 0 and not 3 <= snr <= 8:
        raise ValueError("snr must be 0 (default 4) or 3..8")
    return bytes([distance, DIRECTIONS[direction], speed, delay,
                  triggers, snr, 0, 0])


def print_config(data):
    if len(data) != 10 or data[0] != 1:
        raise ValueError(f"unexpected Config value: {bytes(data).hex(' ')}")
    flags = data[1]
    print(f"Config raw: {bytes(data).hex(' ')}")
    if flags & 1:
        print(f"  distance={data[2]} m, direction={DIRECTION_NAMES.get(data[3], 'unknown')}, "
              f"min_speed={data[4]} km/h, no_target_delay={data[5]} s")
    else:
        print("  detection settings unavailable")
    if flags & 2:
        snr = "firmware default (level 4)" if data[7] == 0 else str(data[7])
        print(f"  trigger_count={data[6]}, SNR_threshold={snr}")
    else:
        print("  sensitivity settings unavailable")


def print_result(data):
    if len(data) != 13 or data[0] != 1:
        raise ValueError(f"unexpected Result value: {bytes(data).hex(' ')}")
    print(f"Result raw: {bytes(data).hex(' ')}")
    print(f"  transaction={data[1]}, status={data[2]}, "
          f"stage=0x{data[3]:02X}, valid_flags=0x{data[4]:02X}")


async def find_device(address, BleakScanner):
    devices = await BleakScanner.discover(timeout=8)
    if address:
        found = next((dev for dev in devices if dev.address.lower() == address.lower()), None)
        if not found:
            raise RuntimeError(f"BLE device {address} was not seen during scan")
        return found
    matches = [dev for dev in devices if dev.name == DEVICE_NAME]
    if not matches:
        raise RuntimeError(f"{DEVICE_NAME} not found; check that ESP32 is advertising")
    if len(matches) > 1:
        raise RuntimeError("multiple modules found; specify --address from scan output")
    return matches[0]


async def run(args):
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as exc:
        raise RuntimeError("Bleak is missing: run 'python -m pip install bleak'") from exc

    if args.action == "scan":
        devices = await BleakScanner.discover(timeout=8)
        for dev in devices:
            if dev.name == DEVICE_NAME:
                print(f"{dev.name}  {dev.address}")
        if not any(dev.name == DEVICE_NAME for dev in devices):
            print("No LD2451-ESP32 device found")
        return

    device = await find_device(args.address, BleakScanner)
    print(f"Connecting to {device.name} ({device.address})")
    async with BleakClient(device, pair=(args.action == "set"), timeout=30) as client:
        if SERVICE not in [str(s.uuid).lower() for s in client.services]:
            raise RuntimeError("connected device does not expose the LD2451 service")
        before = await client.read_gatt_char(CONFIG)
        print_config(before)
        if args.action == "read":
            return

        requested = validate_values(args.distance, args.direction, args.speed,
                                    args.delay, args.triggers, args.snr)
        txid = secrets.randbelow(256)
        packet = bytes([1, txid]) + requested
        print(f"Writing: {packet.hex(' ')}")
        await client.write_gatt_char(CONFIG_WRITE, packet, response=True)
        deadline = asyncio.get_running_loop().time() + 25
        while asyncio.get_running_loop().time() < deadline:
            await asyncio.sleep(0.3)
            result = await client.read_gatt_char(RESULT)
            if len(result) == 13 and result[0] == 1 and result[1] == txid and result[2] != 1:
                print_result(result)
                actual = await client.read_gatt_char(CONFIG)
                print_config(actual)
                if result[2] == 2 and result[4] == 3 and bytes(actual[2:10]) == requested:
                    print("SUCCESS: radar ACK and readback match requested values")
                    return
                raise RuntimeError("configuration did not verify; inspect Result and Config above")
        raise RuntimeError("timed out waiting for radar configuration result")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("scan", help="list nearby LD2451-ESP32 devices")
    read = sub.add_parser("read", help="read current radar settings")
    read.add_argument("--address", help="BLE address if more than one device is nearby")
    write = sub.add_parser("set", help="write all six radar settings and verify readback")
    write.add_argument("--address", help="BLE address if more than one device is nearby")
    write.add_argument("--distance", type=int, required=True, metavar="10..100")
    write.add_argument("--direction", choices=DIRECTIONS, required=True)
    write.add_argument("--speed", type=int, required=True, metavar="0..120")
    write.add_argument("--delay", type=int, required=True, metavar="0..255")
    write.add_argument("--triggers", type=int, required=True, metavar="1..10")
    write.add_argument("--snr", type=int, required=True, metavar="0|3..8")
    args = parser.parse_args()
    try:
        asyncio.run(run(args))
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc


if __name__ == "__main__":
    main()
