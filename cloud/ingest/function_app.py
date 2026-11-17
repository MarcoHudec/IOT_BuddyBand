"""BuddyBand cloud ingest (architecture.md §5.4, step D).

IoT Hub built-in endpoint (Event Hub compatible, consumer group "app")
  -> Azure Table Storage in the same storage account as the alarm archive:
     latest  one row per (group, kind, band): newest status / gps / summary / teacher_online / alarm
     alarms  every alarm, RowKey sorted newest first (inverted timestamp)
     track   every GPS point (bands send one every 5 s from stage "critical"), same RowKey scheme
The teacher app reads both through the Static Web App API (cloud/api/state).

App settings: IOTHUB_EVENTS (built-in endpoint connection string), IOTHUB_EH_NAME,
IOTHUB_CONSUMER_GROUP (= app), TABLES_CONN (optional, defaults to AzureWebJobsStorage).
"""
import datetime as dt
import json
import logging
import os
from typing import List

import azure.functions as func
from azure.data.tables import TableServiceClient, UpdateMode

app = func.FunctionApp()
KINDS = {"status", "alarm", "gps", "summary", "teacher_online"}
MAX_MS = 9_999_999_999_999
_tables = None


def tables():
    global _tables
    if _tables is None:
        svc = TableServiceClient.from_connection_string(
            os.environ.get("TABLES_CONN") or os.environ["AzureWebJobsStorage"])
        _tables = tuple(svc.create_table_if_not_exists(t) for t in ("latest", "alarms", "track"))
    return _tables


def received_ms(doc: dict) -> int:
    try:
        return int(dt.datetime.fromisoformat(doc["gw_received_at"]).timestamp() * 1000)
    except (KeyError, ValueError, TypeError):
        return int(dt.datetime.now(dt.timezone.utc).timestamp() * 1000)


def to_entities(doc: dict):
    """-> (latest_row, history_row or None) for one forwarded document, or None to skip.
    history_row goes to table "alarms" (kind alarm) or "track" (kind gps)."""
    kind = doc.get("kind")
    if kind not in KINDS:
        return None
    group = str(doc.get("group", ""))
    band = doc.get("band")
    at = received_ms(doc)
    data = {k: v for k, v in doc.items() if k not in ("kind", "group", "band", "gw", "gw_received_at")}
    row = {"kind": kind, "band": -1 if band is None else int(band), "at": float(at),   # double: ms do not fit Int32
           "data": json.dumps(data, separators=(",", ":"))}
    latest = {"PartitionKey": group, "RowKey": f"{kind}-{'g' if band is None else band}", **row}
    hist = None
    if kind in ("alarm", "gps"):
        hist = {"PartitionKey": group, "RowKey": f"{MAX_MS - at:013d}-{band}-{data.get('seq', 0)}", **row}
    return latest, hist


@app.event_hub_message_trigger(arg_name="events", event_hub_name="%IOTHUB_EH_NAME%",
                               connection="IOTHUB_EVENTS", consumer_group="%IOTHUB_CONSUMER_GROUP%",
                               cardinality=func.Cardinality.MANY)
def ingest(events: List[func.EventHubEvent]):   # typing.List: the worker rejects list[...]
    latest_t, alarms_t, track_t = tables()
    newest = {}                                    # one write per latest row and batch
    n_alarms = n_gps = 0
    for ev in events:
        try:
            doc = json.loads(ev.get_body().decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            logging.warning("skipping non-JSON message")
            continue
        out = to_entities(doc)
        if not out:
            continue
        latest, hist = out
        key = (latest["PartitionKey"], latest["RowKey"])
        if key not in newest or newest[key]["at"] <= latest["at"]:
            newest[key] = latest
        if hist and hist["kind"] == "alarm":
            alarms_t.upsert_entity(hist, mode=UpdateMode.REPLACE)   # idempotent on redelivery
            n_alarms += 1
        elif hist:
            track_t.upsert_entity(hist, mode=UpdateMode.REPLACE)
            n_gps += 1
    for row in newest.values():
        latest_t.upsert_entity(row, mode=UpdateMode.REPLACE)
    logging.info("ingest: %d events -> %d latest rows, %d alarms, %d gps", len(events), len(newest), n_alarms, n_gps)
