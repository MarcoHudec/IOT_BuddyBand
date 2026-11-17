# Evaluation data

- `rssi/` – RSSI measurements (milestone M1). File name: `<environment>_<distance>m.csv`, e.g. `indoor_10m.csv`.
  Columns: `ms,self,tx,seq,rssi` (output of `firmware/src/rssi_test`).
- `events.csv` – all messages logged by `gateway/consumer/event_logger.py`.

Note for every measurement in `notes.md`: date, place, environment, board IDs, TX power, who measured.

## RSSI calibration (M1 / experiment X1)

1. Flash `rssi_a` (sender, ID 11) on one board → power bank. Flash `rssi_b` (receiver) on a second board → laptop.
   **All other boards off** (or the script ignores and reports them).
2. Same height for both boards (~1 m, e.g. on a backpack/post); measure distance with a tape or step count.
3. Per point (1, 5, 10, 20, 30 m), 30 s each:
   ```bash
   cd evaluation/rssi
   ~/.platformio/penv/bin/python log_rssi.py --env outdoor_free --distance 5
   ```
4. Repeat the whole series with the sender **worn on the body** (`--env outdoor_body`) – that is the realistic case.
5. Write one line per run in `notes.md`. Thresholds `T_near` / `T_far` (architecture.md §4.1) come from these medians.

## Live view while walking

```bash
cd evaluation/rssi
~/.platformio/penv/bin/python live_rssi.py                         # A=-50 dBm @1 m, n=2.5
~/.platformio/penv/bin/python live_rssi.py --a -48 --n 2.2 --log walk_outdoor_1.csv
```

Prints every 0.5 s: packets/s, last RSSI, rolling median of 10, estimated distance
`d = 10^((A − median) / (10·n))`. Set `A` to your measured 1 m median and fit `n` from the
calibration series. Distance is only an estimate (±30–50 %); the stage logic uses RSSI thresholds.
