"""GET /api/state?group=1 for the cloud teacher app (CloudSource in app/sources.js).

Returns {"events": [{kind, band, data, at}, ...]}: the newest row per (kind, band) from
table "latest" plus the alarms of the last 24 h (newest 100) from table "alarms".
No login (demo setup): anyone with the URL can read the state.
App setting: TABLES_CONN (connection string of the BuddyBand storage account).
"""
import json
import os
import time

import azure.functions as func
from azure.data.tables import TableServiceClient

MAX_MS = 9_999_999_999_999
_svc = None


def svc():
    global _svc
    if _svc is None:
        _svc = TableServiceClient.from_connection_string(os.environ["TABLES_CONN"])
    return _svc


def event(row):
    band = row.get("band", -1)
    return {"kind": row["kind"], "band": None if band == -1 else band,
            "data": json.loads(row.get("data") or "{}"), "at": int(row["at"])}


def main(req: func.HttpRequest) -> func.HttpResponse:
    group = req.params.get("group", "1")
    if not group.isdigit():
        return func.HttpResponse("bad group", status_code=400)
    since = int(time.time() * 1000) - 24 * 3600 * 1000
    flt = f"PartitionKey eq '{group}'"
    latest = [event(r) for r in svc().get_table_client("latest").query_entities(flt) if r["kind"] != "alarm"]
    alarms_t = svc().get_table_client("alarms")
    alarms = []
    for r in alarms_t.query_entities(f"{flt} and RowKey lt '{MAX_MS - since:013d}~'", results_per_page=100):
        alarms.append(event(r))
        if len(alarms) >= 100:
            break
    alarms.reverse()                 # oldest first, so the feed ends up newest on top
    body = json.dumps({"events": latest + alarms, "server_time": int(time.time() * 1000)})
    return func.HttpResponse(body, mimetype="application/json", headers={"Cache-Control": "no-store"})
