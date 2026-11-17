# Azure IoT Hub – setup (WP3 cloud tier)

Architecture: `docs/architecture.md` §5. The Pi (device `buddyband-gw`) forwards alarms, GPS,
teacher online/offline and one summary per minute; raw status stays on the Pi.

## 1. Subscription
Azure for Students via HSLU account: <https://azure.microsoft.com/free/students> → sign in at
<https://portal.azure.com>. No credit card needed.

## 2. Resource group + IoT Hub (portal)
1. **Create a resource → Resource group**: name `buddyband-rg`, region **Switzerland North**
   (or West Europe).
2. **Create a resource → IoT Hub**:
   - Subscription: Azure for Students, resource group `buddyband-rg`
   - IoT hub name: globally unique, e.g. `buddyband-hub-g4`
   - Region: same as the resource group
   - **Tier: Free (F1)** – 8 000 messages/day, one F1 hub per subscription
   - everything else default → Review + create (takes 1–2 min)

## 3. Device identity for the Pi
IoT Hub → **Devices → Add device**: Device ID `buddyband-gw`, authentication **Symmetric key**,
auto-generate keys → Save. Open the device → copy **Primary connection string**
(`HostName=...;DeviceId=buddyband-gw;SharedAccessKey=...`). Treat it like a password.

## 4. Give the connection string to the Pi (never into the repo)
```bash
ssh buddyband-gw
nano ~/buddyband-credentials.env      # add one line:  BB_IOTHUB_CONN=HostName=...;DeviceId=buddyband-gw;SharedAccessKey=...
exit
ssh -t buddyband-gw 'sudo bash ~/buddyband-gateway/install_services.sh'   # (re)starts buddyband-cloud
ssh buddyband-gw 'journalctl -u buddyband-cloud -f'                      # "connected to Azure IoT Hub"
```
Without the line the forwarder runs in buffering mode: messages wait in `cloud.outbox`
(max 20 000, oldest dropped first) and are sent as soon as the connection string exists.

## 5. See messages arrive
- Portal: IoT Hub → Overview → **Metrics** "Telemetry messages sent" / Device → Device-to-cloud messages
- VS Code extension *Azure IoT Hub* → right-click the device → **Start monitoring built-in event endpoint**

## 6. Alarm archive (done)
IoT Hub → **Message routing**: route `$body.kind = 'alarm'` → storage account `buddybandstore`,
container `alarms` (JSON, one file per minute); a second route keeps everything on the built-in
endpoint `events`.

## 7. Cloud teacher app (done, step D)
Same app as on the Pi (`app/`), in cloud mode, at the Static Web App URL. No login (demo setup).

```
Pi -> IoT Hub -> Function "ingest" (cloud/ingest) -> tables latest + alarms + track (buddybandstore)
Browser -> Static Web App (app/ + cloud/api) -> GET /api/state -> reads the tables
```
- The forwarder also sends STATUS, thinned to one per band every 30 s (+ stage changes),
  from its own small queue `cloud.status` -> the cloud app shows children, neighbours, stages.
- Create / redeploy everything: `az login` once, then `tools/azure_deploy.sh`
  (`BB_SKIP_FUNC=1` = web app only). Resources: consumer group `app`, Function App
  `buddyband-ingest-<sub>` (Consumption, Python 3.11), Static Web App `buddyband-app` (Free, API Python 3.10).
- The Pi's `app/config.json` is never deployed; without it the app picks cloud mode on HTTPS.
- GPS tracking: from stage "critical" a band sends its position every 5 s (firmware, unchanged).
  Every point goes to table `track`; `/api/state` returns the last 60 min, the app draws the path
  on the map ("Live"). When the band is lost, its last known position stays on the map in red.
  The Pi copy keeps the track in the browser (localStorage) across a reload.
- Card colours follow the teacher band LEDs: green = with the group, orange = moving away / critical,
  red = no contact.
- Limits: a child counts as lost after 75 s without status (local: 8 s); recall only works in the Pi copy.

## Quota check
Alarms + stage changes ~500/day, summary 1/min ≈ 480/8 h, GPS during incidents ~720 per band and hour of "critical" (every 5 s) →
**≈ 1 100 messages/day**, plus STATUS for the cloud app (3 bands × 2/min × 8 h ≈ 2 900)
→ **≈ 4 000 ≪ 8 000** (F1).
