# Evaluation data

- `rssi/` – RSSI measurements (milestone M1). File name: `<environment>_<distance>m.csv`, e.g. `indoor_10m.csv`.
  Columns: `ms,self,tx,seq,rssi` (output of `firmware/src/rssi_test`).
- `events.csv` – all messages logged by `gateway/consumer/event_logger.py`.

Note for every measurement in `notes.md`: date, place, environment, board IDs, TX power, who measured.
