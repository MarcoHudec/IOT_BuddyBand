#!/usr/bin/env python3
"""BuddyBand event logger (AMQP consumer).

Reads every BuddyBand message from RabbitMQ and appends it to a CSV file.
The teacher band publishes via MQTT; RabbitMQ's MQTT plugin routes those
messages to the amq.topic exchange with routing keys like
"buddyband.1.band.2.alarm". This consumer binds to "buddyband.#".

Same pattern as the course exercises in amqp/exercise3 (Pika consumer).
The CSV is the raw data for the evaluation (experiments X1-X5).

Usage:  python3 event_logger.py --host 127.0.0.1 --out ../../evaluation/events.csv
"""

import argparse
import csv
import json
import time

import pika

EXCHANGE = "amq.topic"
QUEUE = "buddyband.events"
BINDING_KEY = "buddyband.#"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin")
    ap.add_argument("--out", default="events.csv")
    args = ap.parse_args()

    credentials = pika.PlainCredentials(args.user, args.password)
    connection = pika.BlockingConnection(
        pika.ConnectionParameters(args.host, 5672, "/", credentials))
    channel = connection.channel()
    channel.queue_declare(queue=QUEUE, durable=True)
    channel.queue_bind(exchange=EXCHANGE, queue=QUEUE, routing_key=BINDING_KEY)

    out = open(args.out, "a", newline="")
    writer = csv.writer(out)
    if out.tell() == 0:
        writer.writerow(["received_at", "routing_key", "payload"])

    def on_message(ch, method, properties, body):
        text = body.decode("utf-8", errors="replace")
        try:
            json.loads(text)  # only check that it is valid JSON
        except json.JSONDecodeError:
            print(" [!] not JSON:", method.routing_key, text)
        writer.writerow([f"{time.time():.3f}", method.routing_key, text])
        out.flush()
        print(" [x]", method.routing_key, text)

    channel.basic_consume(QUEUE, on_message, auto_ack=True)
    print(f" [*] Logging {BINDING_KEY} to {args.out}. To exit press CTRL+C")
    try:
        channel.start_consuming()
    except KeyboardInterrupt:
        pass
    finally:
        out.close()
        connection.close()


if __name__ == "__main__":
    main()
