"""Sync a paired watch on each PC connection using the gateway's existing frame."""

import argparse
import asyncio
import time

from jetson.protocol import RX_UUID, time_sync


async def sync(address, once):
    from bleak import BleakClient
    from bleak.exc import BleakError

    while True:
        disconnected = asyncio.Event()
        try:
            async with BleakClient(
                address, pair=True, timeout=60,
                disconnected_callback=lambda _: disconnected.set(),
            ) as watch:
                await watch.write_gatt_char(RX_UUID, time_sync(int(time.time())), response=True)
                print("TIME_SYNC sent", flush=True)
                if once:
                    return
                await disconnected.wait()
        except (BleakError, OSError, TimeoutError) as exc:
            if once:
                raise
            print(f"Waiting for watch: {exc}", flush=True)
        await asyncio.sleep(3)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address", help="Bluetooth address of the watch")
    parser.add_argument("--once", action="store_true", help="Disconnect after one time update")
    args = parser.parse_args()
    try:
        asyncio.run(sync(args.address, args.once))
    except KeyboardInterrupt:
        pass
