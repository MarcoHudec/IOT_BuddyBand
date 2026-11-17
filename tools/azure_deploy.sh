#!/usr/bin/env bash
# Cloud teacher app (architecture.md §5.4, step D): creates what is missing and deploys.
# Safe to run again – existing resources are kept, code is redeployed.
#   az login                      # once, in the browser
#   tools/azure_deploy.sh         # create + deploy everything
#   BB_SKIP_FUNC=1 tools/azure_deploy.sh   # only the web app (app/ + cloud/api)
#
# Resources (all in resource group buddyband-rg, one storage account for everything):
#   IoT Hub (exists)            consumer group "app" on the built-in endpoint
#   Storage account (exists)    blob container "alarms" (archive, IoT Hub route) + tables latest / alarms
#   Function App (Consumption)  cloud/ingest: IoT Hub -> tables
#   Static Web App (Free)       app/ + cloud/api (/api/state), no login (demo)
# No secret is printed: connection strings go straight from az into app settings.
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="$HOME/.local/bin:$PATH"

RG="${BB_RG:-buddyband-rg}"
az account show -o none 2>/dev/null || { echo "Please run 'az login' first"; exit 1; }
LOC="$(az group show -n "$RG" --query location -o tsv)"
HUB="${BB_HUB:-$(az iot hub list -g "$RG" --query '[0].name' -o tsv)}"
ST="${BB_STORAGE:-$(az storage account list -g "$RG" --query '[0].name' -o tsv)}"
SUFFIX="$(az account show --query id -o tsv | cut -c1-6)"
FUNC="${BB_FUNC:-buddyband-ingest-$SUFFIX}"
SWA="${BB_SWA:-buddyband-app}"
SWA_LOC="${BB_SWA_LOC:-westeurope}"     # Static Web Apps exist only in a few regions
echo "resource group $RG ($LOC) | IoT Hub $HUB | storage $ST | function $FUNC | web app $SWA"
[[ -n $HUB && -n $ST ]] || { echo "IoT Hub or storage account not found in $RG"; exit 1; }

step() { echo; echo "=== $*"; }

step "1/7 resource providers"
for ns in Microsoft.Web Microsoft.Storage; do az provider register --namespace "$ns" --wait -o none; done

step "2/7 IoT Hub consumer group 'app'"
az iot hub consumer-group show --hub-name "$HUB" -g "$RG" -n app -o none 2>/dev/null \
  || az iot hub consumer-group create --hub-name "$HUB" -g "$RG" -n app -o none
echo ok

step "3/7 tables 'latest' and 'alarms' in $ST"
ST_CONN="$(az storage account show-connection-string -g "$RG" -n "$ST" -o tsv)"
for t in latest alarms; do az storage table create -n "$t" --connection-string "$ST_CONN" -o none; done
echo ok

step "4/7 Function App $FUNC (ingest)"
if [[ ${BB_SKIP_FUNC:-0} != 1 ]]; then
if ! az functionapp show -g "$RG" -n "$FUNC" -o none 2>/dev/null; then
  az functionapp create -g "$RG" -n "$FUNC" --storage-account "$ST" --consumption-plan-location "$LOC" \
    --runtime python --runtime-version 3.11 --functions-version 4 --os-type Linux -o none
fi
EH_ENDPOINT="$(az iot hub show -n "$HUB" -g "$RG" --query properties.eventHubEndpoints.events.endpoint -o tsv)"
EH_PATH="$(az iot hub show -n "$HUB" -g "$RG" --query properties.eventHubEndpoints.events.path -o tsv)"
EH_KEY="$(az iot hub policy show --hub-name "$HUB" -g "$RG" -n service --query primaryKey -o tsv)"
az functionapp config appsettings set -g "$RG" -n "$FUNC" -o none --settings \
  "IOTHUB_EVENTS=Endpoint=$EH_ENDPOINT;SharedAccessKeyName=service;SharedAccessKey=$EH_KEY;EntityPath=$EH_PATH" \
  "IOTHUB_EH_NAME=$EH_PATH" "IOTHUB_CONSUMER_GROUP=app" "TABLES_CONN=$ST_CONN"
unset EH_KEY
echo ok
fi

BUILD="$(mktemp -d)"; trap 'rm -rf "$BUILD"' EXIT
step "5/7 deploy ingest code (remote build, ~2 min)"
if [[ ${BB_SKIP_FUNC:-0} != 1 ]]; then
(cd cloud/ingest && zip -qr "$BUILD/ingest.zip" function_app.py host.json requirements.txt)
az functionapp deployment source config-zip -g "$RG" -n "$FUNC" --src "$BUILD/ingest.zip" --build-remote true -o none
# Consumption plan: wake the host and re-register the trigger, otherwise it may sleep until the next event
curl -s -o /dev/null --max-time 90 "https://$FUNC.azurewebsites.net/" || true
az rest --method post -o none --url "https://management.azure.com$(az functionapp show -g "$RG" -n "$FUNC" --query id -o tsv)/syncfunctiontriggers?api-version=2022-03-01"
echo ok
fi

step "6/7 Static Web App $SWA"
if ! az staticwebapp show -n "$SWA" -g "$RG" -o none 2>/dev/null; then
  az staticwebapp create -n "$SWA" -g "$RG" -l "$SWA_LOC" --sku Free -o none
fi
az staticwebapp appsettings set -n "$SWA" -g "$RG" --setting-names "TABLES_CONN=$ST_CONN" -o none
unset ST_CONN
echo ok

step "7/7 deploy app/ and cloud/api"
mkdir -p "$BUILD/app" "$BUILD/api"
# never ship the Pi's config.json (MQTT password); the cloud copy runs without one (-> cloud mode)
tar -C app --exclude=config.json -cf - . | tar -C "$BUILD/app" -xf -
VER="$(git rev-parse --short HEAD 2>/dev/null || echo x)-$(date +%s)"
sed -i "s/?v=dev/?v=$VER/g" "$BUILD/app/index.html" "$BUILD/app/app.js" "$BUILD/app/sources.js"
tar -C cloud/api --exclude=__pycache__ -cf - . | tar -C "$BUILD/api" -xf -
# ship the Python packages with the API (SWA managed functions, Python 3.10, Linux x64)
grep -v '^azure-functions' cloud/api/requirements.txt > "$BUILD/api-req.txt"   # the worker brings azure-functions
uv pip install -q --target "$BUILD/api/.python_packages/lib/site-packages" -r "$BUILD/api-req.txt" \
  --python-version 3.10 --python-platform x86_64-manylinux2014 --only-binary :all:
SWA_CLI_DEPLOYMENT_TOKEN="$(az staticwebapp secrets list -n "$SWA" -g "$RG" --query properties.apiKey -o tsv)" \
  npx -y @azure/static-web-apps-cli@2.0.2 deploy "$BUILD/app" --api-location "$BUILD/api" \
  --api-language python --api-version 3.10 --env production 2>&1 | grep -viE "token|npm warn"
HOST="$(az staticwebapp show -n "$SWA" -g "$RG" --query defaultHostname -o tsv)"
echo
echo "=== done: https://$HOST"
