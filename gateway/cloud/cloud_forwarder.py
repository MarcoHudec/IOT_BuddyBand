#!/usr/bin/env python3
"""BuddyBand cloud forwarder: RabbitMQ -> Azure IoT Hub (WP3, architecture.md §5).

Edge-first split: only what is useful far away / after the outing goes to the cloud:
  alarms (incl. teacher timeout), GPS (bands send it only from stage "critical"),
  teacher online/offline, and the group summary at most once per minute.
Raw STATUS (every 2 s per band) stays on the Pi -> ~1100 msgs/day, IoT Hub F1 allows 8000.

Store-and-forward (lecture 7 queueing, lecture 2 delivery semantics):
  durable queue "cloud.outbox" bound to the topics above; a message is acked only
  after IoT Hub confirmed it (send_message returns) -> at-least-once. Without
  internet or without an IoT Hub connection string the messages simply wait in
  the queue (capped at 20 000, oldest dropped first). Each message carries a
  message_id "<group>-<band>-<kind>-<seq>" so the cloud side can drop duplicates.

Config (env file, default ~/buddyband-credentials.env):
  BB_FORWARDER_PW   RabbitMQ password of user "forwarder" (setup_pi.sh)
  BB_IOTHUB_CONN    device connection string of IoT Hub device "buddyband-gw"

Usage:
  .venv/bin/python cloud_forwarder.py              # forward to IoT Hub
  .venv/bin/python cloud_forwarder.py --dry-run    # print what would be sent, no Azure needed
"""

import argparse
import datetime as dt
import json
import os
import signal
import socket
import sys
import time
from collections import Counter
from pathlib import Path

import pika

EXCHANGE = "amq.topic"
QUEUE = "cloud.outbox"
BINDINGS = [
    "buddyband.*.band.*.alarm",
    "buddyband.*.band.*.gps",
    "buddyband.*.summary",
    "buddyband.*.teacher.online",
]
QUEUE_ARGS = {"x-max-length": 20000, "x-overflow": "drop-head"}
SUMMARY_EVERY_S = 60


def load_env(path: Path) -> dict:
    env = {}
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip()
    return env


def classify(routing_key: str):
    p = routing_key.split(".")
    group = p[1] if len(p) > 1 else ""
    if len(p) == 5 and p[2] == "band":
        return group, p[4], p[3]
    if len(p) == 3 and p[2] == "summary":
        return group, "summary", ""
    if len(p) == 4 and p[2] == "teacher" and p[3] == "online":
        return group, "teacher_online", ""
    return group, "other", ""


def to_cloud_doc(routing_key: str, text: str, gateway: str):
    """Flat JSON document for IoT Hub (routing queries use $body.kind)."""
    group, kind, band = classify(routing_key)
    if kind == "teacher_online":
        data = {"online": text.strip() == "true"}
    else:
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            data = {"raw": text}
    now = dt.datetime.now(dt.timezone.utc)
    doc = dict(data)
    doc.update({
        "kind": kind,
        "group": int(group) if group.isdigit() else group,
        "band": int(band) if band.isdigit() else None,
        "gw_received_at": now.isoformat(timespec="milliseconds"),
        "gw": gateway,
    })
    seq = data.get("seq") if isinstance(data, dict) else None
    ident = seq if seq else int(now.timestamp() * 1000)  # seq 0 = teacher timeout -> use time
    msg_id = f"{group}-{band or 'g'}-{kind}-{ident}"
    return kind, msg_id, doc


class Sender:
    """Thin wrapper so --dry-run works without Azure."""

    def __init__(self, conn_str, dry_run):
        self.dry_run = dry_run
        self.client = None
        if not dry_run:
            from azure.iot.device import IoTHubDeviceClient  # imported only when needed
            self.client = IoTHubDeviceClient.create_from_connection_string(conn_str)
            self.client.connect()
            print("[cloud] connected to Azure IoT Hub", flush=True)

    def send(self, kind, msg_id, doc):
        body = json.dumps(doc, separators=(",", ":"))
        if self.dry_run:
            print(f"[cloud] DRY {msg_id} {body}", flush=True)
            return
        from azure.iot.device import Message
        m = Message(body, message_id=msg_id, content_encoding="utf-8", content_type="application/json")
        m.custom_properties["kind"] = kind
        self.client.send_message(m)  # blocks until IoT Hub acknowledged (MQTT PUBACK), raises on error

    def close(self):
        if self.client:
            self.client.shutdown()


def declare(ch):
    ch.queue_declare(queue=QUEUE, durable=True, arguments=QUEUE_ARGS)
    for b in BINDINGS:
        ch.queue_bind(exchange=EXCHANGE, queue=QUEUE, routing_key=b)


def run(args, env):
    gateway = socket.gethostname()
    counts = Counter()
    last_summary = 0.0
    last_report = time.monotonic()
    backoff = 1
    sender = None

    while True:
        conn = None
        try:
            params = pika.ConnectionParameters(
                args.host, args.port, "/", pika.PlainCredentials(args.user, env["BB_FORWARDER_PW"]),
                heartbeat=600,  # IoT Hub send may block while offline
                client_properties={"connection_name": "buddyband-cloud-forwarder"})
            conn = pika.BlockingConnection(params)
            ch = conn.channel()
            declare(ch)  # buffering starts as soon as the queue exists

            conn_str = env.get("BB_IOTHUB_CONN", "")
            if not args.dry_run and not conn_str:
                backlog = ch.queue_declare(queue=QUEUE, passive=True).method.message_count
                print(f"[cloud] no BB_IOTHUB_CONN yet - buffering only, backlog {backlog}", flush=True)
                conn.close()
                time.sleep(60)
                env.update(load_env(Path(args.cred).expanduser()))  # pick up a newly added connection string
                continue

            if sender is None:
                sender = Sender(conn_str, args.dry_run)

            ch.basic_qos(prefetch_count=1)  # one at a time: keeps order, ack after cloud confirm
            backlog = ch.queue_declare(queue=QUEUE, passive=True).method.message_count
            print(f"[cloud] consuming {QUEUE} (backlog {backlog}) {'DRY RUN' if args.dry_run else ''}", flush=True)
            backoff = 1

            for method, _props, body in ch.consume(QUEUE, inactivity_timeout=5):
                if method is None:  # idle tick
                    pass
                else:
                    kind, msg_id, doc = to_cloud_doc(method.routing_key, body.decode("utf-8", "replace"), gateway)
                    if kind == "summary" and time.monotonic() - last_summary < SUMMARY_EVERY_S:
                        counts["summary_skipped"] += 1
                    else:
                        sender.send(kind, msg_id, doc)  # raises -> no ack -> redelivered later
                        if kind == "summary":
                            last_summary = time.monotonic()
                        counts[kind] += 1
                    ch.basic_ack(method.delivery_tag)
                if time.monotonic() - last_report >= args.report_s:
                    last_report = time.monotonic()
                    print(f"[cloud] last {args.report_s}s: {dict(counts) or 'nothing'}", flush=True)
                    counts.clear()

        except (pika.exceptions.AMQPError, OSError, Exception) as e:  # noqa: BLE001 – keep the service alive
            print(f"[cloud] {e.__class__.__name__}: {e} - retry in {backoff}s", flush=True)
            if sender and not args.dry_run:
                try:
                    sender.close()
                except Exception:  # noqa: BLE001
                    pass
                sender = None
            time.sleep(backoff)
            backoff = min(backoff * 2, 60)
        finally:
            if conn and conn.is_open:
                try:
                    conn.close()
                except Exception:  # noqa: BLE001
                    pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5672)
    ap.add_argument("--user", default="forwarder")
    ap.add_argument("--cred", default="~/buddyband-credentials.env")
    ap.add_argument("--dry-run", action="store_true", help="print instead of sending to Azure")
    ap.add_argument("--report-s", type=int, default=60)
    args = ap.parse_args()

    env = load_env(Path(args.cred).expanduser())
    env.update({k: v for k, v in os.environ.items() if k.startswith("BB_")})
    if "BB_FORWARDER_PW" not in env:
        sys.exit(f"no BB_FORWARDER_PW in {args.cred}")
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        run(args, env)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
