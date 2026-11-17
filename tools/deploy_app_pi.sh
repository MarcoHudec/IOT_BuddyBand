#!/usr/bin/env bash
# Copies the teacher app to the Pi (~/buddyband-app) and writes its config.json there
# from ~/buddyband-credentials.env (BB_APP_PW). No sudo needed; the service
# buddyband-app (install_services.sh, once) serves it on http://raspberrypi47.local:8080
#   tools/deploy_app_pi.sh
set -euo pipefail
cd "$(dirname "$0")/.."
PI="${BB_SSH:-buddyband-gw}"
ssh "$PI" 'mkdir -p ~/buddyband-app'
tar -C app --exclude=config.json -cf - . | ssh "$PI" 'tar -C ~/buddyband-app -xf -'
# cache busting: ?v=dev -> ?v=<commit>-<time>, so phones fetch the new css/js after every deploy
VER="$(git rev-parse --short HEAD 2>/dev/null || echo x)-$(date +%s)"
ssh "$PI" "sed -i 's/?v=dev/?v=$VER/g' ~/buddyband-app/index.html ~/buddyband-app/app.js ~/buddyband-app/sources.js"
scp -q gateway/app_server.py "$PI":buddyband-gateway/app_server.py
ssh "$PI" 'python3 - <<EOF
import json, pathlib
home = pathlib.Path.home()
env = dict(l.split("=", 1) for l in (home / "buddyband-credentials.env").read_text().splitlines()
           if "=" in l and not l.startswith("#"))
cfg = json.loads((home / "buddyband-app/config.example.json").read_text())
cfg["mqtt_password"] = env["BB_APP_PW"].strip()
cfg.pop("mqtt_host", None)            # browser connects to the host it loaded the page from
(home / "buddyband-app/config.json").write_text(json.dumps(cfg, indent=2))
print("deployed", sorted(p.name for p in (home / "buddyband-app").iterdir()))
EOF'
