# BuddyBand

A connected wristband that detects early when a child drifts away from the group
on kindergarten and school outings. Bands form a BLE mesh; GPS is switched on
only when a child is about to lose the group.

HSLU I.MA_IOTSUWO.H26 · Group 4: Marco Hudec, Lino Bucher, Reto Wirz, Milorad Bozic

## Architecture

| Tier | Component | Technology |
| --- | --- | --- |
| Mesh edge | 3 child bands + 1 teacher band (ESP32-WROOM-32D DevKit) | BLE advertising, managed flooding |
| Gateway | Raspberry Pi 4 B | RabbitMQ (MQTT + AMQP), Node-RED dashboard, Python event logger |
| Cloud (optional) | Azure IoT Hub | alarm events only |

Details: [`docs/protocol.md`](docs/protocol.md).

## Repository layout

```
firmware/            PlatformIO project for all ESP32 boards
  lib/protocol/      shared packet format (protocol.h)
  src/app/           band + teacher application
  src/rssi_test/     RSSI measurement for milestone M1
  test/              host test for protocol.h
gateway/             Raspberry Pi: RabbitMQ config, event logger, Node-RED flow
docs/                protocol, decisions, test protocols
evaluation/          raw measurement data (CSV) + analysis scripts
report/              final report, poster, figures
```

## Getting started (firmware)

1. Install VS Code and the **PlatformIO IDE** extension.
2. Open the folder `firmware/` (not the repo root) in VS Code.
3. Pick an environment in the PlatformIO toolbar: `band1`, `band2`, `band3`,
   `teacher`, `rssi_a`, `rssi_b`.
4. Upload, then open the serial monitor (115200 baud).
   If no serial port shows up on macOS, install the USB-serial driver of the
   chip on the board (CP210x or CH340/CH9102).

Each environment sets `NODE_ID` (teacher = 0) and the role. Libraries
(NimBLE-Arduino, PubSubClient) are installed automatically.

Board labels: write the node ID on every board with a sticker.

## Host test for the protocol

```bash
cd firmware/test/test_protocol
g++ -std=c++17 -I../../lib/protocol test_protocol.cpp && ./a.out
```

## Work packages

| WP | Topic |
| --- | --- |
| WP1 | Neighbour detection and stage logic |
| WP2 | Mesh relay and teacher band |
| WP3 | Gateway, dashboard, cloud |
| WP4 | Sensors, feedback, evaluation |
