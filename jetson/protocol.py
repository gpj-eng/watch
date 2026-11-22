"""HSPI-SOS 20-byte BLE packet codec, independent of BlueZ."""

import re
import struct
from collections import namedtuple

SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

IMU_TYPE = 0x01
SOS_TYPE = 0x02
ACK_TYPE = 0x81
TIME_SYNC_TYPE = 0x82
WEATHER_SYNC_TYPE = 0x84
WATCH_OFFSET_MINUTES = 480
WATCH_TIME_MIN = 1735689600
WATCH_TIME_LIMIT = 4102444800
QUEUED = 1
ACCEPTED = 2
FAILED = 3
FALL_TIMEOUT = 1
MANUAL_SOS = 2
MAX_RECIPIENTS = 3
FRAME_SIZE = 20
DEMO_NUMBERS = ("+12025550100", "+12025550101", "+12025550102")

IMU_FRAME = struct.Struct("<BBHI6h")
SOS_FRAME = struct.Struct("<BBQBB8s")
ACK_FRAME = struct.Struct("<BBQ10x")
TIME_SYNC_FRAME = struct.Struct("<BBIh12x")
WEATHER_SYNC_FRAME = struct.Struct("<BBBBBh3B3b3bI")
Imu = namedtuple("Imu", "seq uptime_ms ax ay az gx gy gz")
Sos = namedtuple("Sos", "reason event_id index count phone")


class ProtocolError(ValueError):
    pass


def valid_phone(phone):
    return isinstance(phone, str) and re.fullmatch(r"\+[1-9][0-9]{0,14}", phone) is not None


def unpack_phone(encoded):
    digits = []
    padding = False
    for byte in encoded:
        for digit in (byte >> 4, byte & 0x0F):
            if digit == 0x0F:
                padding = True
            elif padding or digit > 9:
                raise ProtocolError("invalid SOS phone encoding")
            else:
                digits.append(str(digit))
    phone = "+" + "".join(digits)
    if not valid_phone(phone):
        raise ProtocolError("invalid E.164 number")
    return phone


def pack_phone(phone):
    if not valid_phone(phone):
        raise ProtocolError("invalid E.164 number")
    return bytes.fromhex(phone[1:].ljust(16, "f"))


def decode(frame):
    if len(frame) != FRAME_SIZE:
        raise ProtocolError("invalid frame length")
    if frame[0] == IMU_TYPE:
        _, version, *sample = IMU_FRAME.unpack(frame)
        if version != 1:
            raise ProtocolError("unsupported IMU format version")
        return Imu(*sample)
    if frame[0] == SOS_TYPE:
        _, reason, event_id, index, count, phone = SOS_FRAME.unpack(frame)
        if reason not in (FALL_TIMEOUT, MANUAL_SOS) or not event_id:
            raise ProtocolError("invalid SOS reason or event ID")
        if not 0 <= index < count <= MAX_RECIPIENTS:
            raise ProtocolError("invalid SOS recipient index/count")
        return Sos(reason, event_id, index, count, unpack_phone(phone))
    raise ProtocolError("unsupported notification type")


def ack(event_id, status):
    return ACK_FRAME.pack(ACK_TYPE, status, event_id)


def time_sync(utc_epoch):
    local_epoch = utc_epoch + WATCH_OFFSET_MINUTES * 60
    if not WATCH_TIME_MIN <= local_epoch < WATCH_TIME_LIMIT:
        raise ValueError("Jetson clock outside the supported watch date range")
    return TIME_SYNC_FRAME.pack(TIME_SYNC_TYPE, 1, utc_epoch, WATCH_OFFSET_MINUTES)


def weather_sync(code, humidity, wind_kmh, temp_tenths, days, highs, lows, utc_epoch):
    if not WATCH_TIME_MIN <= utc_epoch < WATCH_TIME_LIMIT:
        raise ValueError("Jetson clock outside the supported watch date range")
    return WEATHER_SYNC_FRAME.pack(WEATHER_SYNC_TYPE, 1, code, humidity, wind_kmh,
                                   temp_tenths, *days, *highs, *lows, utc_epoch)
