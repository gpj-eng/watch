"""Receive the Huangshan watch on Jetson Nano using the BlueZ system D-Bus."""

import argparse
import concurrent.futures
import contextlib
import logging
import os
import re
import sqlite3
import sys
import time
from pathlib import Path

from protocol import ACK_FRAME, ProtocolError, RX_UUID, SERVICE_UUID, TX_UUID, time_sync
from receiver import EventStore, Receiver

LOG = logging.getLogger("hspi")
BLUEZ = "org.bluez"
DEVICE = BLUEZ + ".Device1"
SERVICE = BLUEZ + ".GattService1"
CHAR = BLUEZ + ".GattCharacteristic1"
PROPS = "org.freedesktop.DBus.Properties"
OBJECTS = "org.freedesktop.DBus.ObjectManager"


class BleGateway:
    def __init__(self, config, receiver, sender):
        import dbus
        from dbus.mainloop.glib import DBusGMainLoop
        from gi.repository import GLib

        DBusGMainLoop(set_as_default=True)
        self.dbus = dbus
        self.glib = GLib
        self.bus = dbus.SystemBus()
        self.manager = self.interface("/", OBJECTS)
        self.config = config
        self.receiver = receiver
        self.sender = sender
        self.executor = concurrent.futures.ThreadPoolExecutor(1) if sender else None
        self.sending = None
        self.acks = {}
        self.ack_in_flight = None
        self.retry_ack_at = 0

    def interface(self, path, name):
        return self.dbus.Interface(self.bus.get_object(BLUEZ, path, introspect=False), name)

    def device(self):
        expected_address = self.config.watch.upper()
        expected_adapter = "/org/bluez/" + self.config.adapter
        for path, interfaces in self.manager.GetManagedObjects(timeout=3).items():
            props = interfaces.get(DEVICE)
            if props and props.get("Address") == expected_address and props.get("Adapter") == expected_adapter:
                return path, props
        raise RuntimeError("watch not known on this adapter; run bluetoothctl scan/pair first")

    def characteristics(self, device_path):
        objects = self.manager.GetManagedObjects(timeout=3)
        service_paths = {path for path, interfaces in objects.items()
                         if path.startswith(device_path + "/") and
                         interfaces.get(SERVICE, {}).get("UUID") == SERVICE_UUID}
        paths = {}
        for path, interfaces in objects.items():
            char = interfaces.get(CHAR, {})
            if char.get("Service") not in service_paths:
                continue
            uuid = char.get("UUID")
            flags = char.get("Flags", [])
            if (uuid == TX_UUID and "notify" in flags) or (uuid == RX_UUID and "write" in flags):
                paths[uuid] = path
        if TX_UUID not in paths or RX_UUID not in paths:
            raise RuntimeError("watch NUS notify/write characteristics not found")
        return paths[TX_UUID], paths[RX_UUID]

    def queue_ack(self, payload):
        _, _, event_id = ACK_FRAME.unpack(payload)
        self.acks[event_id] = payload

    def write_ack(self, rx):
        if self.ack_in_flight is not None or not self.acks or time.monotonic() < self.retry_ack_at:
            return
        event_id = next(iter(self.acks))
        payload = self.acks.pop(event_id)
        _, status, _ = ACK_FRAME.unpack(payload)
        pending = (event_id, payload)
        self.ack_in_flight = pending

        def written():
            if self.ack_in_flight is pending:
                self.ack_in_flight = None
                LOG.info("ACK %016x sent: status=%d", event_id, status)

        def failed(error):
            if self.ack_in_flight is pending:
                self.ack_in_flight = None
                self.acks.setdefault(event_id, payload)
                self.retry_ack_at = time.monotonic() + 1
                LOG.warning("ACK %016x write failed: %s", event_id, error.get_dbus_name())

        # An empty options dict also supports the BlueZ 5.48 WriteValue API.
        try:
            rx.WriteValue(self.dbus.ByteArray(payload), self.dbus.Dictionary({}, signature="sv"),
                          reply_handler=written, error_handler=failed, timeout=3)
        except self.dbus.DBusException as error:
            failed(error)

    def service_sms(self):
        if self.sender is None:
            return
        if self.sending is not None:
            event_id, future = self.sending
            if not future.done():
                return
            try:
                accepted = future.result()
            except Exception as exc:
                LOG.error("SOS %016x: SMS result uncertain (%s)", event_id, type(exc).__name__)
                accepted = False
            self.queue_ack(self.receiver.store.finish(event_id, accepted))
            self.sending = None
        job = self.receiver.store.claim()
        if job is not None:
            self.sending = job[0], self.executor.submit(self.sender.send, *job)

    def session(self):
        deadline = time.monotonic() + 12
        device_path, props = self.device()
        if not self.config.bench_unpaired and not props.get("Paired", False):
            raise RuntimeError("watch is not paired; pair it using bluetoothctl")
        device = self.interface(device_path, DEVICE)
        if not props.get("Connected", False):
            device.Connect(timeout=12)
        try:
            current = self.interface(device_path, PROPS)
            while not current.Get(DEVICE, "ServicesResolved", timeout=3):
                if time.monotonic() > deadline:
                    raise RuntimeError("GATT service discovery timed out")
                time.sleep(0.2)
            self.subscribe(device_path, current)
        finally:
            try:
                device.Disconnect(timeout=3)
            except self.dbus.DBusException:
                pass

    def subscribe(self, device_path, device_props):
        tx_path, rx_path = self.characteristics(device_path)
        tx, rx = self.interface(tx_path, CHAR), self.interface(rx_path, CHAR)
        loop = self.glib.MainLoop()
        self.receiver.new_connection()
        last_poll = time.monotonic()

        def notification(interface, changed, invalidated):
            if interface != CHAR or "Value" not in changed:
                return
            try:
                reply = self.receiver.receive(bytes(changed["Value"]))
            except ProtocolError as exc:
                LOG.warning("Ignored malformed or unexpected BLE frame: %s", exc)
                return
            except (sqlite3.Error, OSError) as exc:
                LOG.error("Storage failed; no acknowledgement was sent (%s)", type(exc).__name__)
                return
            if reply is not None:
                self.queue_ack(reply)
                self.write_ack(rx)

        def connection_change(interface, changed, invalidated):
            if interface == DEVICE and "Connected" in changed and not changed["Connected"]:
                loop.quit()

        def tick():
            nonlocal last_poll
            try:
                now = time.monotonic()
                if now - last_poll >= 1:
                    last_poll = now
                    if not device_props.Get(DEVICE, "Connected", timeout=3):
                        loop.quit()
                        return True
                    if not self.config.bench_unpaired and not device_props.Get(DEVICE, "Paired", timeout=3):
                        loop.quit()
                        return True
                self.service_sms()
                self.write_ack(rx)
            except (self.dbus.DBusException, sqlite3.Error) as exc:
                LOG.warning("BLE session interrupted (%s)", type(exc).__name__)
                loop.quit()
            return True

        callback = self.interface(tx_path, PROPS).connect_to_signal("PropertiesChanged", notification)
        device_callback = device_props.connect_to_signal("PropertiesChanged", connection_change)
        timer = self.glib.timeout_add(250, tick)
        try:
            tx.StartNotify(timeout=5)
            LOG.info("Subscribed to HSPI-SOS on %s (%s)", self.config.watch, self.config.adapter)
            if not self.config.bench_unpaired:
                try:
                    rx.WriteValue(self.dbus.ByteArray(time_sync(int(time.time()))),
                                  self.dbus.Dictionary({}, signature="sv"), timeout=3)
                    LOG.info("TIME_SYNC sent to %s", self.config.watch)
                except (self.dbus.DBusException, ValueError) as exc:
                    LOG.warning("TIME_SYNC unavailable: %s", exc)
            loop.run()
        finally:
            if self.ack_in_flight is not None:
                event_id, payload = self.ack_in_flight
                self.acks.setdefault(event_id, payload)
                self.ack_in_flight = None
            self.glib.source_remove(timer)
            callback.remove()
            device_callback.remove()
            try:
                tx.StopNotify(timeout=3)
            except self.dbus.DBusException:
                pass

    def run(self):
        try:
            while True:
                try:
                    self.session()
                except (self.dbus.DBusException, RuntimeError) as exc:
                    LOG.warning("Connection not ready: %s", exc)
                time.sleep(3)
        finally:
            if self.executor:
                self.executor.shutdown(wait=True)
                if self.sending is not None:
                    event_id, future = self.sending
                    error = future.exception()
                    self.receiver.store.finish(event_id, error is None and future.result())


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode")
    modes.add_parser("simulate", help="Exercise packet decode, storage and ACKs with demo data")
    ble = modes.add_parser("ble", help="Connect to a specific watch on the Jetson system bus")
    ble.add_argument("--watch", required=True, help="Bluetooth address from bluetoothctl")
    ble.add_argument("--adapter", default="hci0")
    ble.add_argument("--db", type=Path, help="Override the local SOS database path")
    ble.add_argument("--imu-log", type=Path, help="Append all accepted IMU samples as JSON Lines")
    ble.add_argument("--bench-unpaired", action="store_true",
                     help="Receive from a prototype without pairing, only with demo numbers")
    ble.add_argument("--live-sms", action="store_true", help="Enable live Tencent SMS")
    ble.add_argument("--confirm-encrypted-gatt", action="store_true",
                     help="Confirm watch firmware and pairing enforce link encryption")
    options = parser.parse_args()
    if options.mode is None:
        parser.error("select simulate or ble")
    if options.mode == "ble":
        if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", options.watch):
            parser.error("--watch requires a valid Bluetooth address")
        if not re.fullmatch(r"hci[0-9]+", options.adapter):
            parser.error("--adapter requires a BlueZ adapter name, e.g. hci0")
        if options.live_sms and (options.bench_unpaired or not options.confirm_encrypted_gatt):
            parser.error("--live-sms requires pairing and confirmed --confirm-encrypted-gatt")
    return options


def run_ble(config):
    import fcntl

    sender = None
    if config.live_sms:
        from sms import TencentSms
        sender = TencentSms()
    mode = "live" if sender else ("bench" if config.bench_unpaired else "dry")
    address = config.watch.upper().replace(":", "")
    database = config.db or Path.home() / ".local" / "state" / "hspi" / (address + "-" + mode + ".db")
    database.parent.mkdir(parents=True, exist_ok=True)
    with open(str(database) + ".lock", "a") as lock, contextlib.ExitStack() as stack:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("this SOS database is already used by another gateway") from None
        store = EventStore(database, address + "/" + mode)
        stack.callback(store.close)
        imu_log = None
        if config.imu_log:
            imu_log = stack.enter_context(config.imu_log.open("a", encoding="utf-8", buffering=1))
        if sender:
            store.recover_interrupted()
        LOG.info("SOS database %s, SMS mode %s", database, mode)
        receiver = Receiver(store, imu_log, config.bench_unpaired)
        BleGateway(config, receiver, sender).run()


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    config = parse_args()
    if config.mode == "simulate":
        from simulate import run
        run()
        return 0
    if sys.platform != "linux":
        LOG.error("BLE mode needs Linux BlueZ; use simulate on this host.")
        return 2
    os.umask(0o077)
    try:
        run_ble(config)
    except KeyboardInterrupt:
        return 0
    except (ValueError, ImportError, OSError, sqlite3.Error) as exc:
        LOG.error("%s", exc)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
