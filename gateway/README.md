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

## 1. Raspberry Pi OS

Raspberry Pi OS (64-bit) Lite or Desktop. Enable SSH. Give the Pi a fixed name,
e.g. `buddyband-gw`.

## 2. RabbitMQ with MQTT plugin

```bash
sudo apt update
sudo apt install -y rabbitmq-server
sudo rabbitmq-plugins enable rabbitmq_mqtt rabbitmq_management

# user from the course exercises
sudo rabbitmqctl add_user admin admin
sudo rabbitmqctl set_user_tags admin administrator
sudo rabbitmqctl set_permissions -p / admin ".*" ".*" ".*"

sudo cp rabbitmq/rabbitmq.config /etc/rabbitmq/rabbitmq.config
sudo systemctl restart rabbitmq-server
```

Check: management UI at `http://buddyband-gw.local:15672` (admin/admin).

Quick MQTT test (needs `mosquitto-clients` only as command-line tools):

```bash
sudo apt install -y mosquitto-clients
mosquitto_sub -h localhost -u admin -P admin -t 'buddyband/#' -v &
mosquitto_pub -h localhost -u admin -P admin -t buddyband/1/band/2/status -m '{"stage":0}'
```

## 3. Event logger (AMQP)

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
