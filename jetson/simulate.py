"""Run the production codec and receiver against synthetic BLE notifications."""

import io
import json
import logging
import tempfile
from pathlib import Path

from protocol import (ACCEPTED, ACK_FRAME, ACK_TYPE, DEMO_NUMBERS, FAILED, FALL_TIMEOUT,
                      IMU_FRAME, IMU_TYPE, MANUAL_SOS, QUEUED, SOS_FRAME, SOS_TYPE,
                      ProtocolError, pack_phone)
from receiver import EventStore, Receiver

LOG = logging.getLogger("hspi")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def sos(event, index, count=2, reason=FALL_TIMEOUT, phone=None):
    return SOS_FRAME.pack(SOS_TYPE, reason, event, index, count,
                          pack_phone(phone or DEMO_NUMBERS[index]))


def require_ack(frame, status, event):
    require(frame is not None and len(frame) == 20, "expected a 20-byte ACK")
    require(ACK_FRAME.unpack(frame) == (ACK_TYPE, status, event), "unexpected ACK")
    require(frame[10:] == bytes(10), "ACK reserved bytes must be zero")


def require_rejected(receiver, frame):
    try:
        receiver.receive(frame)
    except ProtocolError:
        return
    raise AssertionError("malformed or unexpected frame passed validation")


def run():
    fall_event = 0xFE12345600000001
    sos_event = 0xFE12345600000002
    interrupted_event = 0xFE12345600000003
    motion_log = io.StringIO()
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "simulation.db"
        store = EventStore(path, "simulation")
        try:
            receiver = Receiver(store, motion_log, demo_numbers_only=True)
            for n in range(52):
                if n == 13:  # Simulate a dropped IMU notification.
                    continue
                packet = IMU_FRAME.pack(IMU_TYPE, 1, (65510 + n) & 0xFFFF,
                                        (1000 * n) // 52, 42, -40, 998, 12, -28, n)
                require(receiver.receive(packet) is None, "IMU should never generate an ACK")
            require(receiver.samples == 51 and receiver.missed == 1, "sequence tracking failed")
            receiver.receive(packet)
            require(receiver.reordered == 1, "duplicate IMU should be discarded")
            rows = [json.loads(row) for row in motion_log.getvalue().splitlines()]
            require(len(rows) == 51 and rows[-1]["gyro_dps"] == [1.2, -2.8, 5.1],
                    "incorrect IMU decoding or units")
            require(rows[13]["seq_gap"] == 1 and rows[-1]["seq"] == 25, "sequence wrap failed")
            LOG.info("SIM IMU PASS: %d recorded, %d gap, %d duplicate, 16-bit sequence wrap",
                     receiver.samples, receiver.missed, receiver.reordered)

            # Known wire bytes are independent of the encoder used to generate the other samples.
            sample = bytes.fromhex("01 01 2a00 e8030000 0000 0000 e803 7b00 0000 0000")
            receiver.new_connection()
            receiver.receive(sample)
            last = json.loads(motion_log.getvalue().splitlines()[-1])
            require(last["seq"] == 42 and last["accel_mg"] == [0, 0, 1000] and
                    last["gyro_dps"] == [12.3, 0.0, 0.0], "wire fixture failed")

            require_rejected(receiver, b"\x99" * 20)
            require_rejected(receiver, sample[:-1])
            require_rejected(receiver, bytes([1, 2]) + sample[2:])
            require_rejected(receiver, SOS_FRAME.pack(2, 1, fall_event, 0, 1, b"\xaf" * 8))
            require_rejected(receiver, sos(fall_event, 2, count=2))
            require_rejected(receiver, sos(fall_event, 0, phone="+12025550199"))
            require(receiver.receive(sos(fall_event, 1)) is None,
                    "SOS must wait for all contacts even when index 1 arrives first")
            require(receiver.receive(sos(fall_event, 1)) is None,
                    "duplicate partial frame must not complete the SOS")
            require_rejected(receiver, sos(fall_event, 1, count=1, reason=MANUAL_SOS))
            require_rejected(receiver, sos(fall_event, 1, phone=DEMO_NUMBERS[0]))
            require(store.claim() is None, "an incomplete event must not be available for SMS")
            LOG.info("SIM SOS PASS: malformed and conflicting fragments rejected, partial event unacknowledged")
        finally:
            store.close()

        store = EventStore(path, "simulation")
        try:
            receiver = Receiver(store, demo_numbers_only=True)
            require_ack(receiver.receive(sos(fall_event, 0)), QUEUED, fall_event)
            require_ack(receiver.receive(sos(fall_event, 1)), QUEUED, fall_event)
            require(store.claim() == (fall_event, FALL_TIMEOUT, DEMO_NUMBERS[:2]),
                    "expected both recipients in one durable event")
            require(store.claim() is None, "single event must be claimed once")
            require_ack(store.finish(fall_event, accepted=True), ACCEPTED, fall_event)
            require_ack(receiver.receive(sos(fall_event, 0)), ACCEPTED, fall_event)
            require(store.claim() is None, "terminal replay must not produce another SMS job")

            require_ack(receiver.receive(sos(sos_event, 0, count=1, reason=MANUAL_SOS)),
                        QUEUED, sos_event)
            require(store.claim() == (sos_event, MANUAL_SOS, DEMO_NUMBERS[:1]), "manual reason lost")
            require_ack(store.finish(sos_event, accepted=False), FAILED, sos_event)
            require_ack(receiver.receive(sos(sos_event, 0, count=1, reason=MANUAL_SOS)),
                        FAILED, sos_event)

            require_ack(receiver.receive(sos(interrupted_event, 0, count=1)),
                        QUEUED, interrupted_event)
            require(store.claim()[0] == interrupted_event, "interrupted event missing")
        finally:
            store.close()

        store = EventStore(path, "simulation")
        try:
            store.recover_interrupted()
            receiver = Receiver(store, demo_numbers_only=True)
            require_ack(receiver.receive(sos(interrupted_event, 0, count=1)),
                        FAILED, interrupted_event)
            require(store.claim() is None, "uncertain SMS result must not be retried blindly")
        finally:
            store.close()
    LOG.info("SIM SOS PASS: crash/reopen, two recipients, manual SOS, dedup, ACK 1/2/3")
    LOG.info("SIM COMPLETE: no Bluetooth connection or real SMS was used")
