#!/usr/bin/env python3
"""Live RSSI view with rolling median and a rough distance estimate.

Reads the CSV lines (ms,self,tx,seq,rssi) from the rssi_test receiver board and
prints one line every --interval seconds:

    t=  12.5s  n= 9/s  last= -71  median(10)= -66  ~  6.3 m  |##########

Distance uses the log-distance path-loss model
    d = 10 ** ((A - median) / (10 * n))
A = median RSSI at 1 m, n = path-loss exponent (~2 free outdoors, 2.5-3.5 body-worn, up to 4 indoors).
This is an ESTIMATE (realistically +-30..50 %), meant for demos and sanity checks -
the BuddyBand stage logic uses RSSI thresholds, not metres.

Usage:
    ~/.platformio/penv/bin/python live_rssi.py                      # defaults A=-50, n=2.5
    ~/.platformio/penv/bin/python live_rssi.py --a -48 --n 2.2 --log walk_indoor.csv
Stop with Ctrl+C.
"""

import argparse
import statistics
import time
from collections import deque
from pathlib import Path

import serial

HERE = Path(__file__).resolve().parent


def distance(rssi_median: float, a: float, n: float) -> float:
    return 10 ** ((a - rssi_median) / (10 * n))


def bar(rssi_median: float) -> str:
    # -100 dBm -> empty, -30 dBm -> 35 chars
    width = max(0, min(35, int((rssi_median + 100) / 2)))
    return "#" * width


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--tx", type=int, default=11, help="sender node ID (rssi_a = 11); -1 = any")
    ap.add_argument("--a", type=float, default=-50.0, help="median RSSI at 1 m (dBm)")
    ap.add_argument("--n", type=float, default=2.5, help="path-loss exponent")
    ap.add_argument("--window", type=int, default=10, help="samples in rolling median")
    ap.add_argument("--interval", type=float, default=0.5, help="seconds between output lines")
    ap.add_argument("--log", help="optional CSV file (in evaluation/rssi/) for t,last,median,distance")
    args = ap.parse_args()

    win = deque(maxlen=args.window)
    arrivals = deque()  # timestamps of the last second, for samples/s
    others = 0
    last = None
    log = None
    if args.log:
        path = HERE / args.log
        if path.exists():
            raise SystemExit(f"{path.name} already exists")
        log = open(path, "w")
        log.write("t_s,last_dbm,median_dbm,distance_m\n")

    print(f"Live RSSI from tx={args.tx} on {args.port}  (A={args.a:g} dBm @1 m, n={args.n:g}, "
          f"median of {args.window})  - Ctrl+C to stop")
    t0 = time.time()
    next_print = t0 + args.interval
    pending = b""
    try:
        with serial.Serial(args.port, 115200, timeout=0.05) as s:
            s.reset_input_buffer()
            while True:
                pending += s.read(4096)
                *lines, pending = pending.split(b"\n")
                now = time.time()
                for raw in lines:
                    parts = raw.decode("utf-8", "replace").strip().split(",")
                    if len(parts) != 5 or not parts[0].isdigit():
                        continue
                    if args.tx >= 0 and int(parts[2]) != args.tx:
                        others += 1
                        continue
                    last = int(parts[4])
                    win.append(last)
                    arrivals.append(now)

                while arrivals and arrivals[0] < now - 1.0:
                    arrivals.popleft()

                if now >= next_print:
                    next_print += args.interval
                    t = now - t0
                    if not win or not arrivals:
                        print(f"t={t:6.1f}s  no packets from tx={args.tx} in the last second")
                        continue
                    med = statistics.median(win)
                    d = distance(med, args.a, args.n)
                    print(f"t={t:6.1f}s  n={len(arrivals):2d}/s  last={last:4d}  "
                          f"median({len(win)})={med:6.1f}  ~{d:5.1f} m  |{bar(med)}")
                    if log:
                        log.write(f"{t:.2f},{last},{med:.1f},{d:.2f}\n")
                        log.flush()
    except KeyboardInterrupt:
        pass
    finally:
        if log:
            log.close()
        if others:
            print(f"\nignored {others} packets from other senders - switch those boards off")


if __name__ == "__main__":
    main()
