// BuddyBand application – entry point for child bands and the teacher band.
//
// Implemented
//   WP1: BLE advertising + scan, neighbour table with rolling RSSI median,
//        STATUS every 2 s, random boot seq (architecture.md §4 D4a),
//        stage state machine with dwell times + hysteresis (architecture.md §4.1)
//   WP2: managed flooding – duplicate cache, relay with TTL + random jitter,
//        advertising scheduler (one slot per queued packet), origin table,
//        ALARM on stage change with ACK / retry / latch (band),
//        ACK + per-child isolation detection by STATUS timeout (teacher)
//   Debug: serial commands to simulate an out-of-range neighbour (see handleSerial)
// Each remaining TODO names its work package.
//
// Serial output: lines starting with '#' are debug only (protocol.md D8).

#include <Arduino.h>
#include <esp_random.h>

#include "alarm.h"
#include "ble_link.h"
#include "config.h"
#include "mesh.h"
#include "neighbours.h"
#include "protocol.h"
#include "stage.h"

#ifndef NODE_ID
#error "NODE_ID must be set in platformio.ini"
#endif

#ifdef ROLE_TEACHER
static const char* ROLE = "teacher";
static constexpr bool IS_TEACHER = true;
#else
static const char* ROLE = "band";
static constexpr bool IS_TEACHER = false;
#endif

static bb::NeighbourTable neighbours;
static bb::StageMachine stages;                 // thresholds: bb::StageConfig (provisional)
static bb::DupCache dupCache;
static bb::TxScheduler txSched(cfg::ADV_SLOT_MS);
static bb::OriginTable origins;
static bb::AlarmSender alarms(cfg::ALARM_RETRY_MS, cfg::ALARM_FAST_TRIES, cfg::ALARM_LATCH_MS);
static bool childLost[256] = {};                // teacher: child currently isolated (STATUS timeout)

static uint16_t seq;                            // random start, see setup()
static uint8_t  stage = bb::STAGE_COMPLETE;     // stays 0 on the teacher band
static uint32_t lastRxMs = 0;                   // last packet from own group (isolation timer)
static bool     ignoreTx[256] = {};             // debug: simulated out-of-range nodes

static struct {
  uint32_t rx, own, dup, ignored, relayed, alarmsRx, acksTx;
} stats = {};

static uint16_t nextSeq() { return seq++; }

// Band: (re)transmit the pending alarm with a fresh seq (see alarm.h why).
[[maybe_unused]] static void serviceAlarm(uint32_t now) {
  if (!alarms.due(now)) return;
  bb::Packet p;
  uint16_t s = nextSeq();
  bb::buildPacket(p, GROUP_ID, bb::MSG_ALARM, NODE_ID, s, alarms.payload());
  txSched.enqueue(p, now);
  alarms.sent(s, now);
  Serial.printf("# ALARM tx #%u seq=%u stage=%s prev=%s reason=%u%s\n", alarms.tries(), s,
                bb::stageName(alarms.payload().stage), bb::stageName(alarms.payload().prev_stage),
                alarms.payload().reason, alarms.latched() ? "  (latched)" : "");
}

// Teacher: acknowledge one received ALARM copy.
static void sendAck(uint8_t dst, uint16_t ackedSeq, uint32_t now) {
  bb::Packet p;
  bb::buildPacket(p, GROUP_ID, bb::MSG_ACK, NODE_ID, nextSeq(), bb::AckPayload{dst, ackedSeq});
  txSched.enqueue(p, now);
  stats.acksTx++;
}

// ---------------------------------------------------------------------------
static void sendStatus(uint32_t now) {
  bb::StatusPayload s{
      stage,
      neighbours.countActive(now, cfg::ACTIVE_WINDOW_MS),
      neighbours.bestMedian(now, cfg::ACTIVE_WINDOW_MS),
      255,  // battery unknown on DevKit
  };
  bb::Packet p;
  bb::buildPacket(p, GROUP_ID, bb::MSG_STATUS, NODE_ID, nextSeq(), s);
  txSched.enqueue(p, now);    // own slot right away ...
  txSched.setBackground(p);   // ... and advertised whenever nothing else is due
}

static void printTables(uint32_t now) {
  Serial.printf("# t=%lus seq=%u rx=%lu dup=%lu own=%lu ign=%lu relayed=%lu txq=%u txdrop=%lu "
                "bledrop=%lu | active=%u best=%d stage=%u (%s)\n",
                now / 1000, seq, stats.rx, stats.dup, stats.own, stats.ignored, stats.relayed,
                (unsigned)txSched.pending(), txSched.drops(), bb::bleDropped(),
                neighbours.countActive(now, cfg::ACTIVE_WINDOW_MS),
                neighbours.bestMedian(now, cfg::ACTIVE_WINDOW_MS), stage, bb::stageName(stage));
  if (IS_TEACHER)
    Serial.printf("#   teacher: alarms rx=%lu acks tx=%lu\n", stats.alarmsRx, stats.acksTx);
  else
    Serial.printf("#   alarm: %s tries=%u tx=%lu acked=%lu superseded=%lu last rtt=%lums/%u tries\n",
                  alarms.pending() ? (alarms.latched() ? "LATCHED" : "pending") : "none",
                  alarms.tries(), alarms.totalTx(), alarms.acked(), alarms.superseded(),
                  alarms.lastRtt(), alarms.lastTries());
  for (size_t i = 0; i < bb::NeighbourTable::capacity(); i++) {
    const bb::Neighbour& n = neighbours.at(i);
    if (!n.used) continue;
    Serial.printf("#   nb  tx=%3u median=%4d last=%4d samples=%2u age=%5lums%s\n",
                  n.id, n.median(), n.last, n.count, now - n.last_seen,
                  n.active(now, cfg::ACTIVE_WINDOW_MS) ? "" : "  (stale)");
  }
  for (size_t i = 0; i < bb::OriginTable::capacity(); i++) {
    const bb::Origin& o = origins.at(i);
    if (!o.used) continue;
    Serial.printf("#   src=%3u seq=%5u hops=%u stage=%u nb=%u best=%4d age=%5lums msgs=%lu%s\n",
                  o.src, o.last_seq, o.hops, o.stage, o.neighbours, o.best_rssi,
                  now - o.last_seen, o.messages, childLost[o.src] ? "  LOST" : "");
  }
}

// ---------------------------------------------------------------------------
static void handlePacket(const bb::RxPacket& rx) {
  stats.rx++;
  const bb::Header& h = rx.pkt.header();

  // debug: behave as if this transmitter were out of range
  if (ignoreTx[h.tx]) { stats.ignored++; return; }

  // every copy (also duplicates, also own packets relayed back) is an RSSI sample of tx
  if (h.tx != NODE_ID) {
    neighbours.add(h.tx, rx.rssi, rx.ms);  // RSSI belongs to tx (protocol.md §3.1)
    lastRxMs = rx.ms;
  }
  if (h.src == NODE_ID) { stats.own++; return; }

  // flooding rules (protocol.md §4)
  if (!dupCache.checkAndAdd(h.src, h.seq)) { stats.dup++; return; }
  origins.update(rx.pkt, rx.ms);

  if (bb::shouldRelay(rx.pkt, NODE_ID, IS_TEACHER)) {
    uint32_t jitter = cfg::RELAY_JITTER_MIN_MS +
                      esp_random() % (cfg::RELAY_JITTER_MAX_MS - cfg::RELAY_JITTER_MIN_MS + 1);
    if (txSched.enqueue(bb::relayCopy(rx.pkt, NODE_ID), rx.ms + jitter)) stats.relayed++;
  }

#ifdef ROLE_TEACHER
  if (h.type == bb::MSG_ALARM) {
    const bb::AlarmPayload& a = rx.pkt.payload<bb::AlarmPayload>();
    stats.alarmsRx++;
    sendAck(h.src, h.seq, rx.ms);
    Serial.printf("# TEACHER ALARM band %u: %s -> %s reason=%u hops=%u seq=%u  -> ACK sent\n",
                  h.src, bb::stageName(a.prev_stage), bb::stageName(a.stage), a.reason,
                  bb::hops(h), h.seq);
    // TODO WP3: publish as JSON via MQTT (protocol.md §7); TODO WP4: piezo
  }
  if (childLost[h.src]) {
    childLost[h.src] = false;
    Serial.printf("# TEACHER band %u reachable again (hops=%u)\n", h.src, bb::hops(h));
  }
  // TODO WP3: publish STATUS/GPS as JSON via MQTT (protocol.md §7)
#else
  if (h.type == bb::MSG_ACK && rx.pkt.payload<bb::AckPayload>().dst == NODE_ID) {
    if (alarms.onAck(rx.pkt.payload<bb::AckPayload>().acked_seq, rx.ms))
      Serial.printf("# ALARM acked by teacher after %u tx, %lu ms (ACK hops=%u)\n",
                    alarms.lastTries(), alarms.lastRtt(), bb::hops(h));
  }
  // TODO WP3/WP4: CMD addressed to NODE_ID (recall -> piezo)
#endif
}

// Teacher: a child is "lost" if no new message arrived for ISOLATION (protocol.md §6, D7).
[[maybe_unused]] static void checkChildren(uint32_t now) {
  for (size_t i = 0; i < bb::OriginTable::capacity(); i++) {
    const bb::Origin& o = origins.at(i);
    if (!o.used || childLost[o.src]) continue;
    if (now - o.last_seen > stages.config().isolation_ms) {
      childLost[o.src] = true;
      Serial.printf("# TEACHER ALARM band %u isolated: no message for %lu ms (last stage %s, hops %u)\n",
                    o.src, now - o.last_seen, bb::stageName(o.stage), o.hops);
      // TODO WP3: publish alarm (reason isolation timeout); TODO WP4: piezo
    }
  }
}

// Debug commands in the serial monitor (type + Enter):
//   ignore <id>    drop all packets transmitted by <id> (simulates "out of range")
//   unignore <id>  undo        clear   undo all        help   list
static void handleSerial() {
  static char line[32];
  static size_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (len < sizeof line - 1) line[len++] = c;
      continue;
    }
    if (len == 0) continue;
    line[len] = 0;
    len = 0;
    int id;
    if (sscanf(line, "ignore %d", &id) == 1 && id >= 0 && id < 256) {
      ignoreTx[id] = true;
      Serial.printf("# ignoring tx=%d (simulated out of range)\n", id);
    } else if (sscanf(line, "unignore %d", &id) == 1 && id >= 0 && id < 256) {
      ignoreTx[id] = false;
      Serial.printf("# no longer ignoring tx=%d\n", id);
    } else if (strcmp(line, "clear") == 0) {
      memset(ignoreTx, 0, sizeof ignoreTx);
      Serial.println("# ignore list cleared");
    } else {
      Serial.print("# commands: ignore <id> | unignore <id> | clear   ignored:");
      for (int i = 0; i < 256; i++) if (ignoreTx[i]) Serial.printf(" %d", i);
      Serial.println();
    }
  }
}

// ---------------------------------------------------------------------------
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
  Serial.println("# type 'help' for debug commands");

  // TODO WP4: LEDs, piezo, GPS, accelerometer, reed switch
#ifdef ROLE_TEACHER
  // TODO WP3: Wi-Fi + MQTT (PubSubClient) with LWT, subscribe buddyband/<group>/cmd/#
#endif
}

void loop() {
  bb::RxPacket rx;
  while (bb::bleReceive(rx)) handlePacket(rx);
  handleSerial();

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
      alarms.raise(bb::AlarmPayload{stage, stages.previous(), bb::REASON_NEIGHBOUR_LOSS}, now);
      // TODO WP4: LED colour / piezo pattern per stage; GPS EN high from stage 1 (D9)
    }
  }
  serviceAlarm(now);
#else
  static uint32_t lastCheck = 0;
  if (now - lastCheck >= cfg::TEACHER_CHECK_MS) {
    lastCheck = now;
    checkChildren(now);
  }
#endif

  if (now - lastStatus >= cfg::STATUS_INTERVAL_MS) {
    lastStatus = now;
    neighbours.expire(now, cfg::NEIGHBOUR_TIMEOUT_MS);
    sendStatus(now);
  }

  if (const bb::Packet* p = txSched.tick(now)) bb::bleAdvertise(*p);

  if (now - lastDebug >= cfg::DEBUG_INTERVAL_MS) {
    lastDebug = now;
    printTables(now);
  }
  delay(2);  // yield; packets are buffered in the queue meanwhile
}
