#!/usr/bin/env python3
"""Record one RSSI measurement point from the rssi_test receiver board.

Reads the CSV lines (ms,self,tx,seq,rssi) printed by firmware/src/rssi_test,
saves them to evaluation/rssi/<env>_<distance>m.csv and prints a summary.

Usage (receiver board on the laptop, sender on a power bank):
    ~/.platformio/penv/bin/python log_rssi.py --env outdoor_body --distance 5
    ~/.platformio/penv/bin/python log_rssi.py --env outdoor_free --distance 10 --seconds 30 --port /dev/ttyUSB0

Needs only pyserial (already inside PlatformIO's Python).
"""

import argparse
import statistics
import time
from pathlib import Path

import serial

HERE = Path(__file__).resolve().parent


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", required=True, help="e.g. outdoor_free, outdoor_body, indoor")
    ap.add_argument("--distance", required=True, type=float, help="metres")
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--tx", type=int, default=11,
                    help="node ID of the sender to keep (rssi_a = 11); -1 keeps all")
    args = ap.parse_args()

    dist = f"{args.distance:g}"
    out = HERE / f"{args.env}_{dist}m.csv"
    if out.exists():
        raise SystemExit(f"{out.name} already exists - choose another --env or delete it first")

    rows = []
    others = {}  # tx -> count of packets from other senders (ignored)
    with serial.Serial(args.port, 115200, timeout=0.2) as s:
        s.reset_input_buffer()
        print(f"Recording {args.seconds:g} s at {dist} m ({args.env}) ...")
        end = time.time() + args.seconds
        pending = b""
        while time.time() < end:
            pending += s.read(4096)
            *lines, pending = pending.split(b"\n")
            for raw in lines:
                line = raw.decode("utf-8", "replace").strip()
                parts = line.split(",")
                if len(parts) == 5 and parts[0].isdigit():
                    if args.tx >= 0 and int(parts[2]) != args.tx:
                        others[parts[2]] = others.get(parts[2], 0) + 1
                        continue
                    rows.append(line)
                    print(f"\r  samples: {len(rows):4d}  last RSSI: {parts[4]:>4} dBm", end="")
    print()
    if others:
        print(f"  ignored packets from other senders (tx: count): {others} "
              "- switch those boards off for clean data")

    if not rows:
        raise SystemExit(f"No samples from tx={args.tx} - is the sender powered and the receiver on --port?")

    out.write_text("ms,self,tx,seq,rssi\n" + "\n".join(rows) + "\n")
    rssi = [int(r.split(",")[4]) for r in rows]
    q = statistics.quantiles(rssi, n=4) if len(rssi) >= 4 else [min(rssi), 0, max(rssi)]
    print(f"Saved {len(rssi)} samples to {out.relative_to(HERE.parent.parent)}")
    print(f"  median {statistics.median(rssi):.0f} dBm | IQR {q[0]:.0f} .. {q[2]:.0f} | "
          f"min {min(rssi)} | max {max(rssi)} | {len(rssi) / args.seconds:.1f} samples/s")


if __name__ == "__main__":
    main()
