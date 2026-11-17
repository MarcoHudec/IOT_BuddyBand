// Data sources for the teacher app. Both deliver the same events to app.js:
//   onEvent({ kind: "status"|"alarm"|"gps"|"summary"|"teacher_online", band, data, at })
//   onConn(state, detail)    state = "online" | "offline" | "connecting"
// and offer send(band, cmd) for the recall command.

import { MqttWs } from "./mqtt-ws.js?v=dev";

// ---------------------------------------------------------------- local (Pi)
// MQTT over WebSocket straight to RabbitMQ on the Pi (works without internet).
export class LocalSource {
  constructor(cfg) { this.cfg = cfg; this.label = "lokal (Pi)"; this.canSend = true; }

  start(onEvent, onConn) {
    const host = this.cfg.mqtt_host || location.hostname;
    const url = this.cfg.mqtt_url || `ws://${host}:15675/ws`;
    this.client = new MqttWs({ url, username: this.cfg.mqtt_user, password: this.cfg.mqtt_password });
    this.client.on("status", (s) => onConn(s === "connected" ? "online" : "offline", s));
    this.client.on("message", (topic, payload) => {
      // buddyband/<g>/band/<id>/<kind> | buddyband/<g>/summary | buddyband/<g>/teacher/online
      const p = topic.split("/");
      const at = Date.now();
      if (p.length === 5 && p[2] === "band") {
        onEvent({ kind: p[4], band: Number(p[3]), data: safeJson(payload), at });
      } else if (p[2] === "summary") {
        onEvent({ kind: "summary", data: safeJson(payload), at });
      } else if (p[2] === "teacher" && p[3] === "online") {
        onEvent({ kind: "teacher_online", data: { online: payload.trim() === "true" }, at });
      }
    });
    onConn("connecting", url);
    this.client.subscribe(`buddyband/${this.cfg.group}/#`);
    // RabbitMQ delivers retained messages only for exact topic filters, not for wildcards:
    this.client.subscribe(`buddyband/${this.cfg.group}/teacher/online`);
    for (const id of this.cfg.bands || [1, 2, 3]) this.client.subscribe(`buddyband/${this.cfg.group}/band/${id}/status`);
    this.client.connect();
  }

  send(band, cmd) {
    return this.client.publish(`buddyband/${this.cfg.group}/cmd/${band}`, JSON.stringify({ cmd }));
  }
}

// ---------------------------------------------------------------- cloud (Azure)
// Polls the Static Web App API (/api/state), which reads Azure Table Storage.
// Filled by the "ingest" Azure Function from IoT Hub (step D).
export class CloudSource {
  constructor(cfg) {
    this.cfg = cfg;
    this.label = "Cloud (Azure)";
    this.canSend = false;           // recall via cloud -> IoT Hub C2D: later
    this.seen = new Set();
    this.base = cfg.api_base || "/api";
  }

  start(onEvent, onConn) {
    let first = true;
    const tick = async () => {
      try {
        const r = await fetch(`${this.base}/state?group=${this.cfg.group}`, { cache: "no-store" });
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        const s = await r.json();
        onConn("online", "Azure");
        for (const ev of s.events || []) {          // latest per band/kind + recent alarms
          const key = `${ev.kind}-${ev.band}-${ev.data?.seq}-${ev.at}`;
          if (this.seen.has(key)) continue;
          this.seen.add(key);
          onEvent(first ? { ...ev, history: true } : ev);   // no alarm sound for what happened before opening
        }
        first = false;
      } catch (e) {
        onConn("offline", String(e.message || e));
      }
    };
    onConn("connecting", "Azure");
    tick();
    setInterval(tick, (this.cfg.poll_s || 3) * 1000);
  }

  send() { return false; }
}

// ---------------------------------------------------------------- demo (no hardware)
// index.html?demo – simulated trip: children walk around the teacher, the last child
// wanders off and back every 60 s (drifting -> critical -> isolated), one SOS per 90 s.
// Same RSSI thresholds as the firmware (stage.h): -70 / -85 dBm, out of range below -92.
export class DemoSource {
  constructor(cfg) { this.cfg = cfg; this.label = "Demo"; this.canSend = true; }

  start(onEvent, onConn) {
    const ids = this.cfg.bands || [1, 2, 3];
    const t0 = Date.now(), seq = new Map(), stage = new Map();
    const lat0 = 47.3769, lon0 = 8.5417;
    const pos = (id, t) => {                         // metres around the teacher (0, 0)
      const a = id * 2.1 + t / 15000;
      const wander = id === ids[ids.length - 1];
      const r = wander ? 4 + 58 * (0.5 - 0.5 * Math.cos((2 * Math.PI * t) / 60000)) : 2 + 1.5 * id;
      return [r * Math.cos(a), r * Math.sin(a)];
    };
    const rssiAt = (d) => Math.round(-40 - 30 * Math.log10(Math.max(d, 0.5)) + (Math.random() * 4 - 2));
    const emit = (id, kind, data) => {
      const s = (seq.get(id) || 0) + 1; seq.set(id, s);
      onEvent({ kind, band: id, data: { seq: s, ...data }, at: Date.now() });
    };

    const tick = () => {
      const t = Date.now() - t0;
      const P = new Map([[0, [0, 0]], ...ids.map((id) => [id, pos(id, t)])]);
      for (const id of ids) {
        const [x, y] = P.get(id);
        const nb = [];
        for (const [o, [ox, oy]] of P) {
          if (o === id) continue;
          const r = rssiAt(Math.hypot(ox - x, oy - y));
          if (r > -92) nb.push({ id: o, rssi: r });
        }
        nb.sort((a, b) => b.rssi - a.rssi);
        const best = nb[0]?.rssi ?? -128;
        const st = !nb.length ? 3 : best >= -70 ? 0 : best >= -85 ? 1 : 2;
        const prev = stage.get(id) ?? 0;
        const hops = nb.some((n) => n.id === 0) ? 0 : 1;
        stage.set(id, st);
        if (st === 3) {                              // isolated: nothing reaches the teacher any more
          if (prev !== 3) setTimeout(() => emit(id, "alarm", { hops: 0, stage: 3, prev, reason: 0 }), 8000);
          continue;
        }
        if (st !== prev) emit(id, "alarm", { hops, stage: st, prev, reason: 1 });
        emit(id, "status", { hops, stage: st, neighbours: nb.length, rssi: best, battery: 255, nb: nb.slice(0, 4) });
        if (Math.round(t / 2000) % 5 === 0)
          emit(id, "gps", { lat: lat0 + y / 111111, lon: lon0 + x / (111111 * Math.cos((lat0 * Math.PI) / 180)), fix_age: 1 });
      }
      if (Math.round(t / 2000) % 45 === 12) emit(ids[0], "alarm", { hops: 0, stage: 0, prev: 0, reason: 3 });
    };
    onConn("online", "Simulation");
    onEvent({ kind: "teacher_online", data: { online: true }, at: Date.now() });
    tick();
    setInterval(tick, 2000);
  }

  send() { return true; }
}

function safeJson(s) {
  try { return JSON.parse(s); } catch { return { raw: s }; }
}
