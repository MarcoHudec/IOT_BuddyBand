// BuddyBand application – entry point for child bands and the teacher band.
//
// Implemented
//   WP1: BLE advertising + scan, neighbour table with rolling RSSI median,
//        STATUS every 2 s, random boot seq (architecture.md §4 D4a),
//        stage state machine with dwell times + hysteresis (architecture.md §4.1)
//   WP2: managed flooding – duplicate cache, relay with TTL + random jitter,
//        advertising scheduler (one slot per queued packet), origin table,
//        ALARM with ACK / retry / latch, one alarm slot per reason (band),
//        ACK + per-child isolation detection by STATUS timeout, CMD recall (teacher)
//   WP3: teacher – Wi-Fi + MQTT uplink to the Pi (uplink.cpp, protocol.md §7)
//   WP4: band  – reed tamper alarm, double-tap SOS (MMA8452Q), GPS duty cycle
//                (on from stage 1, GPS packets from stage 2, D9), piezo feedback
//        teacher – LEDs show the group state, piezo per alarm, GPS print
//   Debug: serial commands (type 'help')
// Each remaining TODO names its work package.
//
// Serial output: lines starting with '#' are debug only (protocol.md D8).

#include <Arduino.h>
#include <esp_random.h>

#include "alarm.h"
#include "ble_link.h"
#include "config.h"
#include "hw.h"
#include "mesh.h"
#include "neighbours.h"
#include "protocol.h"
#include "stage.h"
#ifdef ROLE_TEACHER
#include "uplink.h"
#endif

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

// ---- mesh / stage state ----
static bb::NeighbourTable neighbours;
static bb::StageMachine stages;                 // thresholds: bb::StageConfig (provisional)
static bb::DupCache dupCache;
static bb::TxScheduler txSched(cfg::ADV_SLOT_MS);
static bb::OriginTable origins;

// One alarm slot per reason, so e.g. a stage change can never supersede an SOS.
static bb::AlarmSender alarms[3] = {
    {cfg::ALARM_RETRY_MS, cfg::ALARM_FAST_TRIES, cfg::ALARM_LATCH_MS},   // REASON_NEIGHBOUR_LOSS
    {cfg::ALARM_RETRY_MS, cfg::ALARM_FAST_TRIES, cfg::ALARM_LATCH_MS},   // REASON_TAMPER
    {cfg::ALARM_RETRY_MS, cfg::ALARM_FAST_TRIES, cfg::ALARM_LATCH_MS},   // REASON_SOS
};
[[maybe_unused]] static bb::AlarmSender& alarmFor(uint8_t reason) { return alarms[(reason >= 1 && reason <= 3) ? reason - 1 : 0]; }

static uint16_t seq;                            // random start, see setup()
static uint8_t  stage = bb::STAGE_COMPLETE;     // stays 0 on the teacher band
static uint32_t lastRxMs = 0;                   // last packet from own group (isolation timer)
static bool     ignoreTx[256] = {};             // debug: simulated out-of-range nodes

// ---- hardware ----
static hw::Buzzer buzzer;
[[maybe_unused]] static hw::Reed reed;
[[maybe_unused]] static hw::Accel accel;
[[maybe_unused]] static hw::Gps gps;
[[maybe_unused]] static int gpsForce = -1;      // -1 auto (D9), 0 forced off, 1 forced on
[[maybe_unused]] static uint32_t stageZeroSince = 0;

// ---- teacher state ----
static bool childLost[256] = {};                // child currently isolated (message timeout)
[[maybe_unused]] static struct { uint8_t stage, prev, reason; uint32_t at; bool valid; } lastAlarm[256] = {};
[[maybe_unused]] static uint32_t urgentUntil = 0;  // SOS / tamper keep the red LED on

static struct {
  uint32_t rx, own, dup, ignored, relayed, alarmsRx, acksTx, gpsTx, gpsRx;
} stats = {};

static uint16_t nextSeq() { return seq++; }

static const char* reasonName(uint8_t r) {
  switch (r) {
    case bb::REASON_NEIGHBOUR_LOSS: return "neighbour-loss";
    case bb::REASON_TAMPER:         return "TAMPER";
    case bb::REASON_SOS:            return "SOS";
    default:                        return "?";
  }
}

// ===========================================================================
// sending
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

// Band: (re)transmit pending alarms, each copy with a fresh seq (see alarm.h why).
[[maybe_unused]] static void serviceAlarms(uint32_t now) {
  for (auto& a : alarms) {
    if (!a.due(now)) continue;
    bb::Packet p;
    uint16_t s = nextSeq();
    bb::buildPacket(p, GROUP_ID, bb::MSG_ALARM, NODE_ID, s, a.payload());
    txSched.enqueue(p, now);
    a.sent(s, now);
    Serial.printf("# ALARM tx #%u seq=%u %s stage=%s prev=%s%s\n", a.tries(), s,
                  reasonName(a.payload().reason), bb::stageName(a.payload().stage),
                  bb::stageName(a.payload().prev_stage), a.latched() ? "  (latched)" : "");
  }
}

[[maybe_unused]] static void sendAck(uint8_t dst, uint16_t ackedSeq, uint32_t now) {
  bb::Packet p;
  bb::buildPacket(p, GROUP_ID, bb::MSG_ACK, NODE_ID, nextSeq(), bb::AckPayload{dst, ackedSeq});
  txSched.enqueue(p, now);
  stats.acksTx++;
}

[[maybe_unused]] static void sendCmd(uint8_t dst, uint8_t cmd, uint32_t now) {
  for (int i = 0; i < 2; i++) {  // two copies (unacknowledged), each a new message
    bb::Packet p;
    bb::buildPacket(p, GROUP_ID, bb::MSG_CMD, NODE_ID, nextSeq(), bb::CmdPayload{dst, cmd});
    txSched.enqueue(p, now + i * 400);
  }
}

// ===========================================================================
// child band: sensors
#ifndef ROLE_TEACHER
static void raiseAlarm(uint8_t reason, uint8_t prev, uint32_t now) {
  alarmFor(reason).raise(bb::AlarmPayload{stage, prev, reason}, now);
}

static void playStageTune(uint8_t s) {
  switch (s) {
    case bb::STAGE_COMPLETE: buzzer.play(hw::tunes::OK, hw::tunes::OK_N); break;
    case bb::STAGE_DRIFTING: buzzer.play(hw::tunes::DRIFT, hw::tunes::DRIFT_N); break;
    case bb::STAGE_CRITICAL: buzzer.play(hw::tunes::CRITICAL, hw::tunes::CRITICAL_N); break;
    default:                 buzzer.play(hw::tunes::ISOLATED, hw::tunes::ISOLATED_N); break;
  }
}

static void triggerSos(uint32_t now) {
  Serial.println("# SOS (double tap) -> ALARM");
  raiseAlarm(bb::REASON_SOS, stage, now);
  buzzer.play(hw::tunes::SOS, hw::tunes::SOS_N);
}

static void triggerTamper(uint32_t now) {
  Serial.println("# TAMPER (clasp opened) -> ALARM");
  raiseAlarm(bb::REASON_TAMPER, stage, now);
  buzzer.play(hw::tunes::TAMPER, hw::tunes::TAMPER_N);
}

static void serviceSensors(uint32_t now) {
  if (reed.poll(now, cfg::REED_OPEN_MS)) triggerTamper(now);

  static uint32_t lastTap = 0;
  if (accel.present() && now - lastTap >= 20) {
    lastTap = now;
    if (accel.pollDoubleTap()) triggerSos(now);
  }

  // GPS duty cycle (architecture.md D9): on from stage 1, off 60 s after back to complete
  gps.poll(now);
  if (stage != bb::STAGE_COMPLETE) stageZeroSince = now;
  bool want = gpsForce >= 0 ? gpsForce == 1
                            : (stage != bb::STAGE_COMPLETE || now - stageZeroSince < cfg::GPS_OFF_DELAY_MS);
  if (want != gps.powered()) {
    gps.power(want, now);
    Serial.printf("# GPS power %s\n", want ? "ON" : "OFF");
  }
  static bool ttffPrinted = false;
  if (!gps.powered()) ttffPrinted = false;
  if (gps.firstFixMs() && !ttffPrinted) {
    ttffPrinted = true;
    Serial.printf("# GPS first fix after %lu s (sats=%u)\n", gps.firstFixMs() / 1000, gps.fix().sats);
  }

  // GPS packets from stage "critical" (protocol.md §3.2), unacknowledged: periodic anyway
  static uint32_t lastGps = 0;
  if (stage >= bb::STAGE_CRITICAL && gps.fix().valid && now - lastGps >= cfg::GPS_INTERVAL_MS) {
    lastGps = now;
    uint32_t age = (now - gps.fix().at_ms) / 1000;
    bb::GpsPayload g{gps.fix().lat_e7, gps.fix().lon_e7, static_cast<uint8_t>(age > 255 ? 255 : age)};
    bb::Packet p;
    bb::buildPacket(p, GROUP_ID, bb::MSG_GPS, NODE_ID, nextSeq(), g);
    txSched.enqueue(p, now);
    stats.gpsTx++;
  }

  // remind the child while critical / isolated
  static uint32_t lastRemind = 0;
  if (stage >= bb::STAGE_CRITICAL && !buzzer.busy() && now - lastRemind >= cfg::BAND_REMIND_MS) {
    lastRemind = now;
    playStageTune(stage);
  }
}
#endif

// ===========================================================================
// teacher: group state on LEDs, piezo per alarm
#ifdef ROLE_TEACHER
static void teacherBeep(uint8_t src, const bb::AlarmPayload& a, uint32_t now) {
  auto& l = lastAlarm[src];
  bool same = l.valid && l.stage == a.stage && l.prev == a.prev_stage && l.reason == a.reason &&
              now - l.at < cfg::TEACHER_REALARM_MS;
  l = {a.stage, a.prev_stage, a.reason, now, true};
  if (same) return;  // retransmitted copy of the same alarm
  namespace t = hw::tunes;
  if (a.reason == bb::REASON_SOS)         { buzzer.play(t::SOS, t::SOS_N); urgentUntil = now + 30000; }
  else if (a.reason == bb::REASON_TAMPER) { buzzer.play(t::TAMPER, t::TAMPER_N); urgentUntil = now + 30000; }
  else if (a.stage == bb::STAGE_DRIFTING) buzzer.play(t::DRIFT, t::DRIFT_N);
  else if (a.stage == bb::STAGE_CRITICAL) buzzer.play(t::CRITICAL, t::CRITICAL_N);
  else if (a.stage == bb::STAGE_ISOLATED) buzzer.play(t::ISOLATED, t::ISOLATED_N);
  else                                    buzzer.play(t::OK, t::OK_N);
}

static const char* updateLeds(uint32_t now, uint8_t& connected, uint8_t& total) {
  int worst = -1;
  connected = total = 0;
  for (size_t i = 0; i < bb::OriginTable::capacity(); i++) {
    const bb::Origin& o = origins.at(i);
    if (!o.used) continue;
    total++;
    if (!childLost[o.src]) connected++;
    int s = childLost[o.src] ? static_cast<int>(bb::STAGE_ISOLATED) : static_cast<int>(o.stage);
    if (s > worst) worst = s;
  }
  hw::Led l = worst < 0                         ? hw::Led::OFF
            : (int32_t)(urgentUntil - now) > 0  ? hw::Led::RED
            : worst >= bb::STAGE_ISOLATED       ? hw::Led::RED
            : worst >= bb::STAGE_DRIFTING       ? hw::Led::YELLOW
                                                : hw::Led::GREEN;
  static const char* names[] = {"off", "green", "yellow", "red"};
  static hw::Led last = hw::Led::OFF;
  if (l != last) {
    Serial.printf("# TEACHER LED -> %s\n", names[(int)l]);
    last = l;
  }
  hw::ledsSet(l);
  return names[(int)l];
}

// a child is "lost" if no new message arrived for ISOLATION (protocol.md §6, D7)
static void checkChildren(uint32_t now) {
  for (size_t i = 0; i < bb::OriginTable::capacity(); i++) {
    const bb::Origin& o = origins.at(i);
    if (!o.used || childLost[o.src]) continue;
    if (now - o.last_seen > stages.config().isolation_ms) {
      childLost[o.src] = true;
      Serial.printf("# TEACHER ALARM band %u isolated: no message for %lu ms (last stage %s, hops %u)\n",
                    o.src, now - o.last_seen, bb::stageName(o.stage), o.hops);
      buzzer.play(hw::tunes::ISOLATED, hw::tunes::ISOLATED_N);
      uplink::publishTimeout(o.src, o.stage, o.hops, now - o.last_seen);
    }
  }
  uint8_t connected, total;
  const char* led = updateLeds(now, connected, total);
  static uint32_t lastSummary = 0;
  if (now - lastSummary >= cfg::STATUS_INTERVAL_MS) {
    lastSummary = now;
    uplink::publishSummary(connected, total, led);
  }
}

static void onMqttCmd(uint8_t dst, uint8_t cmd) {
  sendCmd(dst, cmd, millis());
  Serial.printf("# CMD %s -> band %u (from MQTT)\n", cmd == bb::CMD_RECALL ? "recall" : "stop", dst);
}
#endif

// ===========================================================================
static void printTables(uint32_t now) {
  Serial.printf("# t=%lus seq=%u rx=%lu dup=%lu own=%lu ign=%lu relayed=%lu txq=%u txdrop=%lu "
                "bledrop=%lu | active=%u best=%d stage=%u (%s)\n",
                now / 1000, seq, stats.rx, stats.dup, stats.own, stats.ignored, stats.relayed,
                (unsigned)txSched.pending(), txSched.drops(), bb::bleDropped(),
                neighbours.countActive(now, cfg::ACTIVE_WINDOW_MS),
                neighbours.bestMedian(now, cfg::ACTIVE_WINDOW_MS), stage, bb::stageName(stage));
#ifdef ROLE_TEACHER
  Serial.printf("#   teacher: alarms rx=%lu acks tx=%lu gps rx=%lu | uplink %s published=%lu failed=%lu\n",
                stats.alarmsRx, stats.acksTx, stats.gpsRx, uplink::stateText(), uplink::published(),
                uplink::failed());
#else
  for (uint8_t r = 1; r <= 3; r++) {
    const auto& a = alarmFor(r);
    if (!a.pending() && !a.totalTx()) continue;
    Serial.printf("#   alarm %-14s %s tries=%u tx=%lu acked=%lu last rtt=%lums/%u tries\n", reasonName(r),
                  a.pending() ? (a.latched() ? "LATCHED" : "pending") : "none", a.tries(), a.totalTx(),
                  a.acked(), a.lastRtt(), a.lastTries());
  }
  Serial.printf("#   sensors: reed=%s%s accel=%s gps=%s%s sentences=%lu fix=%s sats=%u gps tx=%lu\n",
                reed.closed() ? "closed" : "open", reed.armed() ? "" : "(not armed)",
                accel.present() ? "ok" : "MISSING", gps.powered() ? "on" : "off",
                gpsForce < 0 ? "" : "(forced)", gps.sentences(), gps.fix().valid ? "yes" : "no",
                gps.fix().sats, stats.gpsTx);
#endif
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

// ===========================================================================
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
    Serial.printf("# TEACHER ALARM band %u: %s  %s -> %s  hops=%u seq=%u  -> ACK sent\n", h.src,
                  reasonName(a.reason), bb::stageName(a.prev_stage), bb::stageName(a.stage),
                  bb::hops(h), h.seq);
    teacherBeep(h.src, a, rx.ms);
    uplink::publishAlarm(rx.pkt);
  } else if (h.type == bb::MSG_STATUS) {
    uplink::publishStatus(rx.pkt);
  } else if (h.type == bb::MSG_GPS) {
    const bb::GpsPayload& g = rx.pkt.payload<bb::GpsPayload>();
    stats.gpsRx++;
    Serial.printf("# TEACHER GPS band %u: %.6f, %.6f (fix age %us, hops %u)  https://maps.google.com/?q=%.6f,%.6f\n",
                  h.src, g.lat_e7 / 1e7, g.lon_e7 / 1e7, g.fix_age, bb::hops(h), g.lat_e7 / 1e7, g.lon_e7 / 1e7);
    uplink::publishGps(rx.pkt);
  }
  if (childLost[h.src]) {
    childLost[h.src] = false;
    Serial.printf("# TEACHER band %u reachable again (hops=%u)\n", h.src, bb::hops(h));
  }
#else
  if (h.type == bb::MSG_ACK && rx.pkt.payload<bb::AckPayload>().dst == NODE_ID) {
    uint16_t acked = rx.pkt.payload<bb::AckPayload>().acked_seq;
    for (uint8_t r = 1; r <= 3; r++) {
      auto& a = alarmFor(r);
      if (!a.onAck(acked, rx.ms)) continue;
      Serial.printf("# ALARM %s acked by teacher after %u tx, %lu ms (ACK hops=%u)\n", reasonName(r),
                    a.lastTries(), a.lastRtt(), bb::hops(h));
      if (r != bb::REASON_NEIGHBOUR_LOSS) buzzer.play(hw::tunes::ACKED, hw::tunes::ACKED_N);  // "help is coming"
    }
  } else if (h.type == bb::MSG_CMD && rx.pkt.payload<bb::CmdPayload>().dst == NODE_ID) {
    uint8_t cmd = rx.pkt.payload<bb::CmdPayload>().cmd;
    Serial.printf("# CMD %s from teacher (hops=%u)\n", cmd == bb::CMD_RECALL ? "RECALL" : "STOP", bb::hops(h));
    if (cmd == bb::CMD_RECALL) buzzer.play(hw::tunes::RECALL, hw::tunes::RECALL_N);
    else buzzer.stop();
  }
#endif
}

// ===========================================================================
// Debug commands in the serial monitor (type + Enter), see 'help'.
static void handleSerial(uint32_t now) {
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
    } else if (strcmp(line, "beep") == 0) {
      buzzer.play(hw::tunes::BOOT, hw::tunes::BOOT_N);
#ifdef ROLE_TEACHER
    } else if (sscanf(line, "recall %d", &id) == 1 && id > 0 && id < 255) {
      sendCmd(id, bb::CMD_RECALL, now);
      Serial.printf("# CMD recall -> band %d\n", id);
    } else if (sscanf(line, "stop %d", &id) == 1 && id > 0 && id < 255) {
      sendCmd(id, bb::CMD_STOP, now);
      Serial.printf("# CMD stop -> band %d\n", id);
#else
    } else if (strcmp(line, "sos") == 0) {
      triggerSos(now);
    } else if (strcmp(line, "tamper") == 0) {
      triggerTamper(now);
    } else if (strcmp(line, "gps on") == 0 || strcmp(line, "gps off") == 0 || strcmp(line, "gps auto") == 0) {
      gpsForce = line[5] == 'n' ? 1 : line[5] == 'f' ? 0 : -1;
      Serial.printf("# GPS mode: %s\n", gpsForce < 0 ? "auto (D9)" : gpsForce ? "forced on" : "forced off");
#endif
    } else {
      Serial.print("# commands: ignore <id> | unignore <id> | clear | beep");
#ifdef ROLE_TEACHER
      Serial.print(" | recall <id> | stop <id>");
#else
      Serial.print(" | sos | tamper | gps on|off|auto");
#endif
      Serial.print("   ignored:");
      for (int i = 0; i < 256; i++) if (ignoreTx[i]) Serial.printf(" %d", i);
      Serial.println();
    }
  }
}

// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(1500);
  seq = static_cast<uint16_t>(esp_random());  // avoid duplicate-cache hits after a reboot
  Serial.printf("# BuddyBand %s, node %d, group %d, protocol v%d, seq start %u\n",
                ROLE, NODE_ID, GROUP_ID, bb::PROTOCOL_VERSION, seq);

  buzzer.begin(pins::PIEZO);
#ifdef ROLE_TEACHER
  hw::ledsBegin();
#else
  reed.begin(pins::REED);
  bool acc = accel.begin(pins::SDA, pins::SCL, cfg::TAP_THRESHOLD);
  gps.begin(pins::GPS_RX, pins::GPS_TX, pins::GPS_EN);
  Serial.printf("# sensors: reed %s, accel %s, GPS on UART2 (power per D9)\n",
                reed.closed() ? "closed (armed)" : "open (arms when closed)", acc ? "ok" : "NOT FOUND");
#endif

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
  buzzer.play(hw::tunes::BOOT, hw::tunes::BOOT_N);

#ifdef ROLE_TEACHER
  uplink::begin(GROUP_ID, onMqttCmd);  // Wi-Fi + MQTT with LWT, subscribes buddyband/<g>/cmd/+
#endif
}

void loop() {
  bb::RxPacket rx;
  while (bb::bleReceive(rx)) handlePacket(rx);

  uint32_t now = millis();
  handleSerial(now);
  buzzer.update(now);

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
      raiseAlarm(bb::REASON_NEIGHBOUR_LOSS, stages.previous(), now);
      playStageTune(stage);
    }
  }
  serviceSensors(now);
  serviceAlarms(now);
#else
  uplink::loop(now);
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
