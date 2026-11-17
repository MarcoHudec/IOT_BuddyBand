"""GET /api/state?group=1 for the cloud teacher app (CloudSource in app/sources.js).

Returns {"events": [{kind, band, data, at}, ...]}: the newest row per (kind, band) from
table "latest", the alarms of the last 24 h (newest 100) from table "alarms" and the
GPS track of the last 60 min (newest 400 points) from table "track".
No login (demo setup): anyone with the URL can read the state.
App setting: TABLES_CONN (connection string of the BuddyBand storage account).
"""
import json
import os
import time

import azure.functions as func
from azure.core.exceptions import ResourceNotFoundError
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


def newest(table, flt, since, limit):
    """Rows newer than since (RowKey = inverted ms, so newest first), returned oldest first."""
    out = []
    try:
        for r in svc().get_table_client(table).query_entities(
                f"{flt} and RowKey lt '{MAX_MS - since:013d}~'", results_per_page=limit):
            out.append(event(r))
            if len(out) >= limit:
                break
    except ResourceNotFoundError:    # table not created yet
        return []
    out.reverse()
    return out


def main(req: func.HttpRequest) -> func.HttpResponse:
    group = req.params.get("group", "1")
    if not group.isdigit():
        return func.HttpResponse("bad group", status_code=400)
    flt = f"PartitionKey eq '{group}'"
    latest = [event(r) for r in svc().get_table_client("latest").query_entities(flt) if r["kind"] != "alarm"]
    now = int(time.time() * 1000)
    alarms = newest("alarms", flt, now - 24 * 3600 * 1000, 100)
    track = newest("track", flt, now - 3600 * 1000, 400) if any(e["kind"] == "gps" for e in latest) else []
    body = json.dumps({"events": latest + track + alarms, "server_time": now})
    return func.HttpResponse(body, mimetype="application/json", headers={"Cache-Control": "no-store"})
