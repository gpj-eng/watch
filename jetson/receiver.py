"""Persist complete SOS messages and forward IMU samples."""

import json
import logging
import sqlite3
import time

from protocol import ACCEPTED, DEMO_NUMBERS, FAILED, QUEUED, Imu, ProtocolError, ack, decode

LOG = logging.getLogger("hspi")

SCHEMA = """
CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS events (
    id TEXT PRIMARY KEY,
    reason INTEGER NOT NULL,
    count INTEGER NOT NULL,
    received_at REAL NOT NULL,
    state TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS recipients (
    event_id TEXT NOT NULL REFERENCES events(id),
    recipient_index INTEGER NOT NULL,
    phone TEXT NOT NULL,
    PRIMARY KEY (event_id, recipient_index),
    UNIQUE (event_id, phone)
);
"""


class EventStore:
    def __init__(self, database, scope):
        self.db = sqlite3.connect(str(database))
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.executescript(SCHEMA)
        stored = self.db.execute("SELECT value FROM metadata WHERE key='scope'").fetchone()
        if stored and stored[0] != scope:
            self.db.close()
            raise ValueError("database belongs to another watch or operating mode")
        with self.db:
            self.db.execute("INSERT OR IGNORE INTO metadata VALUES ('scope', ?)", (scope,))

    def close(self):
        self.db.close()

    def receive(self, packet):
        event = "{:016x}".format(packet.event_id)
        completed = False
        with self.db:
            self.db.execute(
                "INSERT OR IGNORE INTO events VALUES (?, ?, ?, ?, 'assembling')",
                (event, packet.reason, packet.count, time.time()),
            )
            reason, count, state = self.db.execute(
                "SELECT reason, count, state FROM events WHERE id=?", (event,)
            ).fetchone()
            if (reason, count) != (packet.reason, packet.count):
                raise ProtocolError("conflicting metadata for an SOS event")
            existing = self.db.execute(
                "SELECT phone FROM recipients WHERE event_id=? AND recipient_index=?",
                (event, packet.index),
            ).fetchone()
            if existing is None:
                try:
                    self.db.execute(
                        "INSERT INTO recipients VALUES (?, ?, ?)", (event, packet.index, packet.phone)
                    )
                except sqlite3.IntegrityError:
                    raise ProtocolError("repeated destination in a single SOS event") from None
            elif existing[0] != packet.phone:
                raise ProtocolError("conflicting recipient for an SOS event")
            if state == "assembling":
                received = self.db.execute(
                    "SELECT COUNT(*) FROM recipients WHERE event_id=?", (event,)
                ).fetchone()[0]
                if received == count:
                    self.db.execute("UPDATE events SET state='queued' WHERE id=?", (event,))
                    state = "queued"
                    completed = True
        # The SQLite context commits before any ACK is handed to the caller.
        if completed:
            LOG.info("SOS %s committed: reason=%d, recipients=%d", event, reason, count)
        return {"queued": QUEUED, "sending": QUEUED, "accepted": ACCEPTED, "failed": FAILED}.get(state)

    def recover_interrupted(self):
        with self.db:
            cursor = self.db.execute("UPDATE events SET state='failed' WHERE state='sending'")
        if cursor.rowcount:
            LOG.warning("Found %d interrupted SMS requests; check provider history before a new send.",
                        cursor.rowcount)

    def claim(self):
        with self.db:
            self.db.execute("BEGIN IMMEDIATE")
            row = self.db.execute(
                "SELECT id, reason FROM events WHERE state='queued' ORDER BY received_at LIMIT 1"
            ).fetchone()
            if row is None:
                return None
            event, reason = row
            self.db.execute("UPDATE events SET state='sending' WHERE id=?", (event,))
            phones = self.db.execute(
                "SELECT phone FROM recipients WHERE event_id=? ORDER BY recipient_index", (event,)
            )
            return int(event, 16), reason, tuple(row[0] for row in phones)

    def finish(self, event_id, accepted):
        status = "accepted" if accepted else "failed"
        with self.db:
            self.db.execute(
                "UPDATE events SET state=? WHERE id=? AND state='sending'",
                (status, "{:016x}".format(event_id)),
            )
        return ack(event_id, ACCEPTED if accepted else FAILED)


class Receiver:
    def __init__(self, store, imu_log=None, demo_numbers_only=False):
        self.store = store
        self.imu_log = imu_log
        self.demo_numbers_only = demo_numbers_only
        self.last_seq = None
        self.samples = 0
        self.missed = 0
        self.reordered = 0
        self.last_imu_report = None

    def new_connection(self):
        self.last_seq = None

    def receive(self, frame):
        packet = decode(frame)
        if isinstance(packet, Imu):
            self.receive_imu(packet)
            return None
        if self.demo_numbers_only and packet.phone not in DEMO_NUMBERS:
            raise ProtocolError("unpaired bench mode accepts demo numbers only")
        status = self.store.receive(packet)
        return ack(packet.event_id, status) if status is not None else None

    def receive_imu(self, sample):
        delta = 1 if self.last_seq is None else (sample.seq - self.last_seq) & 0xFFFF
        if delta == 0 or delta > 0x7FFF:
            self.reordered += 1
            return
        self.last_seq = sample.seq
        self.samples += 1
        self.missed += delta - 1
        now = time.monotonic()
        payload = {
            "type": "imu", "received_at": time.time(), "seq": sample.seq,
            "received_monotonic_s": now,
            "uptime_ms": sample.uptime_ms, "accel_mg": [sample.ax, sample.ay, sample.az],
            "gyro_dps": [axis / 10 for axis in (sample.gx, sample.gy, sample.gz)],
            "seq_gap": delta - 1,
        }
        encoded = json.dumps(payload, separators=(",", ":"))
        if self.imu_log is not None:
            self.imu_log.write(encoded + "\n")
        if self.last_imu_report is None or now - self.last_imu_report >= 1:
            LOG.info("IMU %s", encoded)
            self.last_imu_report = now
