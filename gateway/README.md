# Gateway (Raspberry Pi 4 B) – WP3

The Raspberry Pi runs **RabbitMQ** as the single message broker:

- **MQTT** (port 1883) for the teacher band and Node-RED
- **AMQP** (port 5672) for the Python event logger

This is the setup from the course exercises (`amqp/` in the course repo), applied
to BuddyBand.

```
Teacher band ──MQTT──► RabbitMQ (amq.topic) ──MQTT──► Node-RED dashboard
                                         └──AMQP──► consumer/event_logger.py ──► evaluation/events.csv
```

## 1. Raspberry Pi and SSH access

Our Pi: Raspberry Pi 4 **#47**, lab image (Debian 13), reachable as **`raspberrypi47.local`**
(mDNS), user `iotlabpi47`. **Do not change the Pi password** (course rule). Set up key login once
on each laptop (you type the Pi password a single time):

```bash
ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519_buddyband
cat >> ~/.ssh/config <<'EOF'
Host buddyband-gw
    HostName raspberrypi47.local
    User iotlabpi47
    IdentityFile ~/.ssh/id_ed25519_buddyband
EOF
ssh-copy-id -i ~/.ssh/id_ed25519_buddyband.pub buddyband-gw
```

## 2. RabbitMQ (one script)

`setup_pi.sh` installs RabbitMQ 4 with MQTT (1883), MQTT over WebSocket (15675) and the
management UI (15672), uses `rabbitmq/rabbitmq.conf`, disables anonymous MQTT and creates
**one user per client with a random password** (architecture.md D12):

| User | Used by | Rights |
| --- | --- | --- |
| `teacher-band` | teacher band firmware | publish `buddyband.*`, read only `buddyband.<g>.cmd.*` |
| `nodered` | Node-RED, `tools/watch_mqtt.sh` | management tag |
| `logger` | `consumer/event_logger.py` | |
| `forwarder` | cloud forwarder (WP3 cloud) | |
| `admin` | management UI | administrator |

```bash
scp -r gateway buddyband-gw:~/buddyband-gateway
ssh -t buddyband-gw 'sudo bash ~/buddyband-gateway/setup_pi.sh'      # asks for the Pi password once
```

Passwords end up in `~/buddyband-credentials.env` on the Pi (mode 600). Copy them to your laptop,
**never into the repo**:

```bash
mkdir -p ~/.config/buddyband && scp buddyband-gw:buddyband-credentials.env ~/.config/buddyband/credentials.env
tools/make_secrets.sh        # creates firmware/src/app/secrets.h (git-ignored), then set WIFI_SSID/WIFI_PASS
tools/watch_mqtt.sh          # live view of everything arriving at the broker
```

Management UI: `http://raspberrypi47.local:15672`, user `admin`, password `BB_ADMIN_PW`.

Verified 17 Nov 2026: bands → teacher (BLE) → Wi-Fi/MQTT → RabbitMQ on the Pi; status of all
3 bands + summary every 2 s; `buddyband/1/cmd/2 {"cmd":"recall"}` reaches band 2 via the teacher;
anonymous and foreign topics are refused.

## 3. Event logger (AMQP) – TODO: run as user `logger` with BB_LOGGER_PW

```bash
sudo apt install -y python3-pika
python3 consumer/event_logger.py --out ../evaluation/events.csv
```

## 4. Node-RED dashboard

```bash
bash <(curl -sL https://raw.githubusercontent.com/node-red/linux-installers/master/deb/update-nodejs-and-nodered)
sudo systemctl enable --now nodered
```

Install `node-red-dashboard` via *Manage palette*, then build the flow:
`mqtt in` (broker localhost:1883, admin/admin, topic `buddyband/+/band/+/#`) →
function (parse topic + JSON) → dashboard widgets. Export the flow to
`node-red/flows.json` and commit it.

## 5. Wi-Fi hotspot for outdoor tests

The teacher band and the teacher's phone connect to a hotspot opened by the Pi
(works without internet). Setup with NetworkManager:

```bash
sudo nmcli device wifi hotspot ifname wlan0 ssid BuddyBand password <choose-one>
```

## 6. Cloud (optional, week 3)

Forward alarm events (not status) from RabbitMQ to Azure IoT Hub, which also
speaks MQTT/AMQP. Decide in week 3 depending on progress.
