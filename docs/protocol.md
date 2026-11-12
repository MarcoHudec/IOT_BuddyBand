# BuddyBand Protocol Specification

Version 1 · Status: draft for the deep session (14 Nov 2026) · last update 11 Nov 2026

This document is the shared interface between all work packages. Change it only
together (WP1 + WP2 + WP3) and bump `PROTOCOL_VERSION` in
`firmware/lib/protocol/protocol.h` when the packet layout changes.

## 1. Overview

```
Child bands ──BLE advertising (mesh flooding)──► Teacher band ──Wi-Fi / MQTT──► RabbitMQ (Raspberry Pi)
     ▲                                               │                              │        │
     └──────────── BLE (ACK / CMD flooding) ◄────────┘                        MQTT  │        │ AMQP
                                                                                    ▼        ▼
                                                                              Node-RED   Pika event logger
                                                                              dashboard  (CSV, evaluation)
```

| Link | Protocol | Course reference |
| --- | --- | --- |
| Band ↔ band | BLE advertising (non-connectable), managed flooding | L1 (BLE) |
| Teacher band → gateway | Wi-Fi + MQTT (PubSubClient) | L4, L6, `mqtt/` exercises |
| Gateway broker | RabbitMQ with MQTT plugin | L7, L8, `amqp/` exercises |
| Gateway → dashboard | MQTT → Node-RED dashboard | L6, L8 |
| Gateway → storage | AMQP (Pika consumer) → CSV | L7, L8 |

## 2. Design decisions

| ID | Decision | Rationale |
| --- | --- | --- |
| D1 | All band-to-band traffic is **broadcast** in BLE advertising packets | No pairing, no fixed order of children; any band in range can relay |
| D2 | **Node ID** 1 byte (teacher = 0, bands = 1…254), **group ID** 1 byte | Readable IDs; packets from other groups (other teams at the demo) are ignored |
| D3 | Five message types: STATUS, ALARM, GPS, ACK, CMD | Minimal set for the stage model plus a return channel |
| D4 | **Managed flooding**: TTL, duplicate cache, random relay jitter | Simple, robust, self-healing; routing trees are future work |
| D5 | ALARM and GPS are **acknowledged** end-to-end by the teacher band; STATUS is not | Alarms must arrive; status is periodic anyway |
| D6 | **Binary** packets over the air, **JSON** over MQTT | Advertising payload is limited to 31 bytes; JSON is easy for Node-RED and Python |
| D7 | Isolation (stage 3) is detected by the **teacher band** via STATUS timeout; the band also detects it locally | An isolated band cannot report itself |
| D8 | Serial output is **debug only** (lines starting with `#`) | Data path is fully wireless |

## 3. Over-the-air format (BLE)

Each packet is carried in the **manufacturer specific data** field of a
non-connectable BLE advertisement, with company ID `0xFFFF` (reserved for
testing). Budget: 31 bytes advertising data − 3 bytes flags − 4 bytes
manufacturer header = **24 bytes** for our packet.

All multi-byte fields are little-endian (native ESP32 byte order).

### 3.1 Header (8 bytes, every packet)

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | `version` | uint8 | Protocol version, currently 1 |
| 1 | `group` | uint8 | Group ID; packets of other groups are dropped |
| 2 | `type` | uint8 | Message type (see 3.2) |
| 3 | `src` | uint8 | Original sender |
| 4 | `tx` | uint8 | Node that transmitted **this copy** (relays set their own ID) |
| 5–6 | `seq` | uint16 | Sequence number, per `src`, increments per new message |
| 7 | `ttl` | uint8 | Remaining hops; starts at `TTL_MAX` = 3 |

`tx` matters for neighbour detection: the RSSI of a received packet belongs to
`tx`, not to `src`. Hop count is derived as `TTL_MAX − ttl`.

### 3.2 Message types and payloads

| Type | Value | Direction | Payload | Size |
| --- | --- | --- | --- | --- |
| STATUS | 1 | band → teacher, every 2 s | `stage` u8, `neighbours` u8, `best_rssi` i8, `battery` u8 (%) | 4 |
| ALARM | 2 | band → teacher, on stage change | `stage` u8, `prev_stage` u8, `reason` u8 | 3 |
| GPS | 3 | band → teacher, every 5 s from stage 2 | `lat` i32 (deg × 10⁷), `lon` i32 (deg × 10⁷), `fix_age` u8 (s) | 9 |
| ACK | 4 | teacher → band | `dst` u8, `acked_seq` u16 | 3 |
| CMD | 5 | teacher → band | `dst` u8, `cmd` u8 (1 = recall / beep, 2 = stop) | 2 |

Largest packet: header 8 + GPS 9 = 17 bytes, within the 24-byte budget.

Alarm `reason`: 1 = neighbour loss, 2 = tamper (reed switch), 3 = SOS button.

Stages: 0 = complete, 1 = drifting, 2 = critical, 3 = isolated.

## 4. Flooding rules (WP2)

1. Drop packets with wrong `version` or `group`.
2. Look up (`src`, `seq`) in the duplicate cache (last 32 entries). Seen → drop.
3. Otherwise add to cache and process locally (teacher: publish; band: ACK/CMD for own ID).
4. If `ttl > 1` and the packet is not addressed to this node: decrement `ttl`,
   set `tx` to own ID, wait a random 5–30 ms, then queue for advertising.
5. The teacher band does not relay band-to-teacher traffic; bands do not relay
   their own packets back.

**Advertising queue:** one BLE advertiser can only carry one packet at a time.
Each queued packet is advertised for 300 ms (≈ 3 advertising events at 100 ms
interval), then the next one. When the queue is empty the band advertises its
latest STATUS.

## 5. Reliability (WP2)

- ALARM and GPS: retransmit after 500 ms without matching ACK, at most 5 times.
- Count retransmissions per message → data for experiment X3.

## 6. Timing (start values, calibrate in week 2)

| Parameter | Value |
| --- | --- |
| STATUS interval | 2 s |
| Teacher marks band isolated | 8 s without STATUS |
| GPS interval from stage 2 | 5 s |
| ACK timeout / max retries | 500 ms / 5 |
| Advertising interval | 100 ms |
| BLE scan | continuous, passive, duplicates **not** filtered |
| Advertising slot per queued packet | 300 ms |

## 7. Teacher band → MQTT (WP3)

The teacher band connects via Wi-Fi to the gateway (Raspberry Pi hotspot) and
publishes JSON with PubSubClient.

| Topic | When | Example payload |
| --- | --- | --- |
| `buddyband/<group>/band/<id>/status` | every STATUS received | `{"seq":120,"hops":1,"stage":0,"neighbours":2,"rssi":-61,"battery":87}` |
| `buddyband/<group>/band/<id>/alarm` | ALARM received or isolation timeout | `{"seq":41,"hops":2,"stage":2,"prev":1,"reason":1}` |
| `buddyband/<group>/band/<id>/gps` | GPS received | `{"seq":44,"hops":2,"lat":47.0432,"lon":8.3097,"fix_age":3}` |
| `buddyband/<group>/summary` | every 2 s | `{"connected":3,"total":3}` |
| `buddyband/<group>/cmd/<id>` | **subscribed** by teacher band | `{"cmd":"recall"}` |

RabbitMQ's MQTT plugin maps topic levels `/` to routing-key dots `.` on the
`amq.topic` exchange, so AMQP consumers bind to `buddyband.#`.

## 8. Hardware and node IDs

Boards: ESP32 DevKit with **ESP32-WROOM-32D** (classic ESP32, chip ESP32-D0WD
rev 1.0, BLE 4.2 + Wi-Fi, micro-USB). Not ESP32-S3. PlatformIO board: `esp32dev`.

Every board carries a sticker with its node ID.

| Node ID | Role | Build environment | MAC address |
| --- | --- | --- | --- |
| 0 | Teacher band | `teacher` | tbd |
| 1 | Child band | `band1` | tbd |
| 2 | Child band | `band2` | tbd |
| 3 | Child band | `band3` | tbd |
| 11 | RSSI test sender | `rssi_a` | 24:0a:c4:1d:24:70 |
| 12 | RSSI test receiver | `rssi_b` | tbd |

The MAC address is printed by the upload log (`MAC: ...`). IDs 11/12 are test
IDs only; the boards get IDs 0–3 when the application firmware is flashed.

## 9. First RSSI observations (11 Nov 2026)

Setup: `rssi_test` firmware, TX power 9 dBm, advertising every 100 ms, new
sequence number every 1 s. Board 12 on a laptop as receiver, board 11 on a power
bank as sender. Indoors, apartment. Informal test, not yet the calibration series.

| Situation | RSSI (dBm) | Note |
| --- | --- | --- |
| Boards almost touching | about −8 | Saturation range, not useful for calibration |
| Side by side on a table (~10 cm) | −19 to −22 | Very stable |
| Board carried in hand while walking | −66 to −83 | Body attenuates the signal |
| Kitchen, ~5 m walking distance, walls in between (no line of sight) | −59 to −65, median ≈ −62 | Stable while not moving |

Findings that affect the design:

1. **About 10 receptions per second per neighbour.** Every advertisement is
   received, not only every new sequence number. Enough samples for smoothing.
2. **Single outliers.** Values between −78 and −102 dBm appear in otherwise
   stable series (3 of ~445 samples, < 1 %), even when nothing moves. Likely
   causes: BLE advertises on three channels with different multipath fading,
   and interference in the 2.4 GHz band (Wi-Fi, other Bluetooth devices).
   → The stage logic must never react to a single value. WP1 uses a **median
   over the last 10 samples** per neighbour.
3. **Body shadowing.** A board held at the body reads 10–20 dB weaker than the
   same board lying free nearby. Children wear the bands on their bodies, so
   stage 1 needs a time condition (e.g. below threshold for several seconds),
   not only a threshold.
4. **Walls matter more than distance indoors.** ~5 m through walls already
   gives about −62 dBm. Thresholds must be calibrated outdoors, where outings
   take place.

## 10. Open points

- [ ] Calibration series outdoors: 1/5/10/20/30 m, line of sight, 30 s each (WP1, deep session)
- [ ] Exact RSSI thresholds and time conditions for stages 1 and 2 (WP1, from calibration)
- [ ] Battery measurement possible on the dev boards? Otherwise send 255 = unknown
- [ ] Wi-Fi + BLE coexistence on the teacher band: measure scan loss (WP2/WP3)
- [ ] Fill in MAC addresses of all boards (table in section 8)
