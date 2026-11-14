// BuddyBand application – entry point for child bands and the teacher band.
//
// Implemented (WP1): BLE advertising + scan, neighbour table with rolling RSSI
// median, STATUS every 2 s, random boot sequence number (architecture.md §4 D4a),
// stage state machine with dwell times + hysteresis (architecture.md §4.1).
// Each remaining TODO names its work package.
//
// Serial output: lines starting with '#' are debug only (protocol.md D8).

#include <Arduino.h>
#include <esp_random.h>

#include "ble_link.h"
#include "config.h"
#include "neighbours.h"
#include "protocol.h"
#include "stage.h"

#ifndef NODE_ID
#error "NODE_ID must be set in platformio.ini"
#endif

#ifdef ROLE_TEACHER
static const char* ROLE = "teacher";
#else
static const char* ROLE = "band";
#endif

static bb::NeighbourTable neighbours;
static bb::StageMachine stages;                 // thresholds: bb::StageConfig (provisional)
static uint16_t seq;                            // random start, see setup()
static uint8_t  stage = bb::STAGE_COMPLETE;     // stays 0 on the teacher band
static uint32_t lastRxMs = 0;                   // last packet from own group (isolation timer)
static uint32_t rxTotal = 0;
static uint32_t rxOwn = 0;                      // own packets heard back via relays (ignored)

static void sendStatus(uint32_t now) {
  bb::StatusPayload s{
      stage,
      neighbours.countActive(now, cfg::ACTIVE_WINDOW_MS),
      neighbours.bestMedian(now, cfg::ACTIVE_WINDOW_MS),
      255,  // battery unknown on DevKit
  };
  bb::Packet p;
  bb::buildPacket(p, GROUP_ID, bb::MSG_STATUS, NODE_ID, seq++, s);
  bb::bleAdvertise(p);  // TODO WP2: advertising queue (relays, ACK, ALARM) instead of direct replace
}

static void printTable(uint32_t now) {
  Serial.printf("# t=%lus seq=%u rx=%lu own=%lu dropped=%lu | active=%u best=%d stage=%u (%s)\n",
                now / 1000, seq, rxTotal, rxOwn, bb::bleDropped(),
                neighbours.countActive(now, cfg::ACTIVE_WINDOW_MS),
                neighbours.bestMedian(now, cfg::ACTIVE_WINDOW_MS), stage, bb::stageName(stage));
  for (size_t i = 0; i < bb::NeighbourTable::capacity(); i++) {
    const bb::Neighbour& n = neighbours.at(i);
    if (!n.used) continue;
    Serial.printf("#   tx=%3u median=%4d last=%4d samples=%2u age=%5lums total=%lu%s\n",
                  n.id, n.median(), n.last, n.count, now - n.last_seen, n.total,
                  n.active(now, cfg::ACTIVE_WINDOW_MS) ? "" : "  (stale)");
  }
}

static void handlePacket(const bb::RxPacket& rx) {
  rxTotal++;
  const bb::Header& h = rx.pkt.header();
  if (h.src == NODE_ID || h.tx == NODE_ID) { rxOwn++; return; }

  neighbours.add(h.tx, rx.rssi, rx.ms);  // RSSI belongs to tx (protocol.md §3.1)
  lastRxMs = rx.ms;

  // TODO WP2: duplicate cache on (src, seq), relay with TTL + jitter (protocol.md §4)
  // TODO WP2: band – handle ACK/CMD addressed to NODE_ID
#ifdef ROLE_TEACHER
  // TODO WP3: publish STATUS/ALARM/GPS as JSON via MQTT (protocol.md §7), send ACK
#endif
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  seq = static_cast<uint16_t>(esp_random());  // avoid duplicate-cache hits after a reboot
  Serial.printf("# BuddyBand %s, node %d, group %d, protocol v%d, seq start %u\n",
                ROLE, NODE_ID, GROUP_ID, bb::PROTOCOL_VERSION, seq);

  bb::bleBegin(cfg::TX_POWER_DBM);
  lastRxMs = millis();  // isolation timer starts at boot
  sendStatus(millis());
#ifndef ROLE_TEACHER
  const bb::StageConfig& sc = stages.config();
  Serial.printf("# stage config: T_near=%d T_far=%d hyst=%d dB dwell up/down=%lu/%lu ms isolation=%lu ms\n",
                sc.t_near_dbm, sc.t_far_dbm, sc.hyst_db, sc.dwell_up_ms, sc.dwell_down_ms,
                sc.isolation_ms);
#endif

  // TODO WP4: LEDs, piezo, GPS, accelerometer, reed switch
#ifdef ROLE_TEACHER
  // TODO WP3: Wi-Fi + MQTT (PubSubClient) with LWT, subscribe buddyband/<group>/cmd/#
  // TODO WP2: isolation detection by STATUS timeout (protocol.md §6)
#endif
}

void loop() {
  bb::RxPacket rx;
  while (bb::bleReceive(rx)) handlePacket(rx);

  uint32_t now = millis();
  static uint32_t lastStatus = 0, lastDebug = 0;
  [[maybe_unused]] static uint32_t lastStageTick = 0;

#ifndef ROLE_TEACHER
  if (now - lastStageTick >= cfg::STAGE_TICK_MS) {
    lastStageTick = now;
    if (stages.update(now, neighbours, lastRxMs)) {
      stage = stages.stage();
      Serial.printf("# STAGE %s -> %s  (best=%d active=%u)\n", bb::stageName(stages.previous()),
                    bb::stageName(stage), neighbours.bestMedian(now, cfg::ACTIVE_WINDOW_MS),
                    neighbours.countActive(now, cfg::ACTIVE_WINDOW_MS));
      sendStatus(now);  // report the new stage right away
      lastStatus = now;
      // TODO WP2: send ALARM (stage, prev_stage, REASON_NEIGHBOUR_LOSS) with ACK/retry + latch
      // TODO WP4: LED colour / piezo pattern per stage; GPS EN high from stage 1 (D9)
    }
  }
#endif

  if (now - lastStatus >= cfg::STATUS_INTERVAL_MS) {
    lastStatus = now;
    neighbours.expire(now, cfg::NEIGHBOUR_TIMEOUT_MS);
    sendStatus(now);
  }
  if (now - lastDebug >= cfg::DEBUG_INTERVAL_MS) {
    lastDebug = now;
    printTable(now);
  }
  delay(2);  // yield; packets are buffered in the queue meanwhile
}
