#!/usr/bin/env python3
"""BuddyBand event logger (AMQP consumer, WP3).

Writes every BuddyBand message that reaches RabbitMQ to a CSV file per day.
The teacher band publishes via MQTT; RabbitMQ's MQTT plugin routes those
messages to the amq.topic exchange with routing keys like
"buddyband.1.band.2.alarm" (MQTT "/" -> AMQP "."). This consumer binds a
durable queue to "buddyband.#" (same pattern as course exercise amqp/exercise3).

Reliability (lecture 2 / 7): the queue is durable, messages are acknowledged
manually only after the CSV line is written and flushed -> at-least-once.
If the logger is down, RabbitMQ keeps the messages in the queue.

CSV columns: received_at (UTC, ISO), epoch_ms, routing_key, group, kind, band, payload
  kind = status | alarm | gps | summary | teacher_online | cmd | other

Usage (on the Pi; password from ~/buddyband-credentials.env, user "logger"):
  python3 event_logger.py                           # -> ~/buddyband-data/events-YYYY-MM-DD.csv
  python3 event_logger.py --out-dir /tmp/x --verbose
"""

import argparse
import csv
import datetime as dt
import json
import os
import signal
import sys
import time
from collections import Counter
from pathlib import Path

import pika

EXCHANGE = "amq.topic"
QUEUE = "buddyband.events"
BINDING_KEY = "buddyband.#"


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
    """buddyband.<g>.band.<id>.<kind> | buddyband.<g>.summary | ...teacher.online | ...cmd.<id>"""
    p = routing_key.split(".")
    group = p[1] if len(p) > 1 else ""
    if len(p) == 5 and p[2] == "band":
        return group, p[4], p[3]
    if len(p) == 3 and p[2] == "summary":
        return group, "summary", ""
    if len(p) == 4 and p[2] == "teacher" and p[3] == "online":
        return group, "teacher_online", ""
    if len(p) == 4 and p[2] == "cmd":
        return group, "cmd", p[3]
    return group, "other", ""


class DailyCsv:
    HEADER = ["received_at", "epoch_ms", "routing_key", "group", "kind", "band", "payload"]

    def __init__(self, out_dir: Path):
        self.out_dir = out_dir
        self.out_dir.mkdir(parents=True, exist_ok=True)
        self.day = None
        self.f = None
        self.w = None

    def write(self, row):
        today = dt.datetime.now(dt.timezone.utc).date().isoformat()
        if today != self.day:
            self.close()
            path = self.out_dir / f"events-{today}.csv"
            new = not path.exists() or path.stat().st_size == 0
            self.f = open(path, "a", newline="")
            self.w = csv.writer(self.f)
            if new:
                self.w.writerow(self.HEADER)
            self.day = today
            print(f"[logger] writing {path}", flush=True)
        self.w.writerow(row)
        self.f.flush()

    def close(self):
        if self.f:
            self.f.close()
            self.f = None


def run(args, password: str) -> None:
    out = DailyCsv(Path(args.out_dir).expanduser())
    counts = Counter()
    last_report = time.monotonic()

    def on_message(ch, method, properties, body):
        nonlocal last_report
        now = dt.datetime.now(dt.timezone.utc)
        text = body.decode("utf-8", errors="replace")
        try:
            json.loads(text)
        except json.JSONDecodeError:
            if text not in ("true", "false"):  # teacher/online is a plain string
                print(f"[logger] not JSON: {method.routing_key} {text}", flush=True)
        group, kind, band = classify(method.routing_key)
        out.write([now.isoformat(timespec="milliseconds"), int(now.timestamp() * 1000),
                   method.routing_key, group, kind, band, text])
        ch.basic_ack(delivery_tag=method.delivery_tag)  # only after the line is on disk
        counts[kind] += 1
        if args.verbose or kind in ("alarm", "teacher_online", "cmd"):
            print(f"[logger] {method.routing_key} {text}", flush=True)
        if time.monotonic() - last_report >= args.report_s:
            last_report = time.monotonic()
            print(f"[logger] last {args.report_s}s: {dict(counts)}", flush=True)
            counts.clear()

    backoff = 1
    while True:
        try:
            params = pika.ConnectionParameters(
                args.host, args.port, "/", pika.PlainCredentials(args.user, password),
                heartbeat=30, client_properties={"connection_name": "buddyband-event-logger"})
            conn = pika.BlockingConnection(params)
            ch = conn.channel()
            ch.queue_declare(queue=QUEUE, durable=True)
            ch.queue_bind(exchange=EXCHANGE, queue=QUEUE, routing_key=BINDING_KEY)
            ch.basic_qos(prefetch_count=100)
            ch.basic_consume(QUEUE, on_message, auto_ack=False)
            print(f"[logger] connected to {args.host}:{args.port} as {args.user}, "
                  f"queue {QUEUE} <- {BINDING_KEY}", flush=True)
            backoff = 1
            ch.start_consuming()
        except (pika.exceptions.AMQPConnectionError, pika.exceptions.ChannelClosedByBroker,
                pika.exceptions.StreamLostError) as e:
            print(f"[logger] connection lost ({e.__class__.__name__}), retry in {backoff}s", flush=True)
            time.sleep(backoff)
            backoff = min(backoff * 2, 30)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5672)
    ap.add_argument("--user", default="logger")
    ap.add_argument("--cred", default="~/buddyband-credentials.env",
                    help="env file with BB_LOGGER_PW (written by setup_pi.sh)")
    ap.add_argument("--out-dir", default="~/buddyband-data")
    ap.add_argument("--report-s", type=int, default=60, help="print a message count every N seconds")
    ap.add_argument("--verbose", action="store_true", help="print every message")
    args = ap.parse_args()

    password = os.environ.get("BB_LOGGER_PW") or load_env(Path(args.cred).expanduser()).get("BB_LOGGER_PW")
    if not password:
        sys.exit(f"no BB_LOGGER_PW in environment or {args.cred}")

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))  # systemd stop
    try:
        run(args, password)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
