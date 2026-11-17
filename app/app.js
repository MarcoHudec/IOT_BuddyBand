// BuddyBand teacher app – one code base for the cloud copy (Azure Static Web Apps)
// and the local copy served by the Raspberry Pi (architecture.md §5.4).
// config.json decides the data source: {"mode":"local"|"cloud"|"demo", ...}; ?demo forces the demo.
// One band = one child. The teacher band has id 0.

import { CloudSource, DemoSource, LocalSource } from "./sources.js?v=dev";

const STAGES = [
  { name: "beisammen",     cls: "s0", say: "ist bei der Gruppe" },
  { name: "entfernt sich", cls: "s1", say: "entfernt sich von der Gruppe" },
  { name: "kritisch",      cls: "s2", say: "ist fast außer Reichweite – bitte zurückrufen" },
  { name: "isoliert",      cls: "s3", say: "hat keinen Kontakt zur Gruppe – bitte suchen!" },
];
// No status for this long -> lost. Local: 8 s (= firmware isolation timeout). Cloud: STATUS arrives
// only every 30 s per band (cloud_forwarder.py), so allow 75 s there.
let LOST_MS = 8000;
const TEACHER = 0;
// GPS: bands send a position every 5 s from stage "critical" (firmware config.h GPS_INTERVAL_MS).
const TRACK_MS = 60 * 60 * 1000;     // keep the track of the last hour
const LIVE_MS = 20000;               // newer than this = live position
const COLORS = ["#334155", "#3b82f6", "#8b5cf6", "#ec4899", "#14b8a6", "#f97316", "#0ea5e9", "#a855f7", "#84cc16"];

const state = {
  bands: new Map(),        // id -> { id, status, statusAt, gps, gpsAt, track: [{lat, lon, at}], lastAlarm, timeout }
  alarms: [],              // newest first
  teacherOnline: null,
  conn: "connecting",
  connDetail: "",
};
let cfg, source;

// ------------------------------------------------------------------ helpers
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const isTeacher = (id) => id === TEACHER;
function name(id) {
  if (isTeacher(id)) return "Lehrer-Band";
  return (cfg.names && cfg.names[id]) || `Kind ${id}`;
}
function initial(id) {
  if (isTeacher(id)) return "L";
  const n = name(id);
  return /^(Kind|Band) \d+$/.test(n) ? String(id) : n.trim()[0].toUpperCase();
}
const color = (id) => COLORS[id % COLORS.length];
const avatar = (id, sm = "") => `<span class="avatar ${sm}" style="--a:${color(id)}">${esc(initial(id))}</span>`;

// RSSI -> plain words. -70 / -85 dBm are the firmware thresholds T_near / T_far (stage.h).
function signal(rssi) {
  if (rssi == null || rssi <= -128) return { q: 0, word: "kein Signal", dist: "nicht in Reichweite" };
  if (rssi >= -60) return { q: 4, word: "sehr stark", dist: "sehr nah" };
  if (rssi >= -70) return { q: 3, word: "stark", dist: "nah" };
  if (rssi >= -85) return { q: 2, word: "mittel", dist: "etwas entfernt" };
  return { q: 1, word: "schwach", dist: "weit weg" };
}
const bars = (q) => `<span class="bars q${q}"><i></i><i></i><i></i><i></i></span>`;

const ago = (t, now) => {
  if (!t) return "–";
  const s = Math.max(0, Math.round((now - t) / 1000));
  return s < 60 ? `vor ${s} s` : `vor ${Math.round(s / 60)} min`;
};
const time = (t) => new Date(t).toLocaleTimeString("de-CH", { hour: "2-digit", minute: "2-digit", second: "2-digit" });

// ------------------------------------------------------------------ events
function band(id) {
  if (!state.bands.has(id)) state.bands.set(id, { id });
  return state.bands.get(id);
}

function onEvent(ev) {
  if (ev.kind === "status") {
    const b = band(ev.band);
    b.status = ev.data; b.statusAt = ev.at; b.timeout = false;
  } else if (ev.kind === "gps") {
    addGps(band(ev.band), ev.data, ev.at);
    saveTracks();
  } else if (ev.kind === "alarm") {
    const b = band(ev.band);
    const a = { ...ev.data, band: ev.band, at: ev.at };
    // the band re-sends an alarm until it is acknowledged: show each (stage, prev, reason) once per 10 s
    const dup = state.alarms.find((x) => x.band === a.band && x.stage === a.stage && x.prev === a.prev &&
                                        x.reason === a.reason && a.at - x.at < 10000);
    if (!dup) {
      state.alarms.unshift(a);
      state.alarms.length = Math.min(state.alarms.length, 200);
      if (!ev.history && (a.stage >= 2 || a.reason >= 2 || a.reason === 0)) alertSound(a.reason === 3 ? 3 : 1);
    }
    b.lastAlarm = a;
    if (a.reason === 0 && a.at >= (b.statusAt || 0)) b.timeout = true;   // an older timeout must not hide newer status
  } else if (ev.kind === "summary") {
    // the summary is published by the teacher band, so it is online (cloud only gets "online" on changes)
    if (state.teacherOnline == null && Date.now() - ev.at < 180000) state.teacherOnline = true;
  } else if (ev.kind === "teacher_online") {
    state.teacherOnline = ev.data.online;
  }
  render();
}

function addGps(b, d, at) {
  if (!Number.isFinite(d?.lat) || !Number.isFinite(d?.lon) || (d.lat === 0 && d.lon === 0)) return;
  const t = at - (d.fix_age || 0) * 1000;            // when the fix was taken
  b.track ||= [];
  if (b.track.some((p) => p.at === t)) return;
  b.track.push({ lat: d.lat, lon: d.lon, at: t });
  b.track.sort((x, y) => x.at - y.at);
  while (b.track.length && (b.track.length > 720 || t - b.track[0].at > TRACK_MS)) b.track.shift();
  const last = b.track[b.track.length - 1];
  b.gps = last; b.gpsAt = last.at;
}

// local copy: keep the tracks over a page reload (MQTT only delivers new GPS messages)
const trackKey = () => `bb-tracks-${cfg.group}`;
function saveTracks() {
  if (cfg.mode !== "local") return;
  clearTimeout(saveTracks.t);
  saveTracks.t = setTimeout(() => {
    try {
      const o = {};
      for (const b of state.bands.values()) if (b.track?.length) o[b.id] = b.track;
      localStorage.setItem(trackKey(), JSON.stringify(o));
    } catch { /* storage blocked */ }
  }, 1000);
}
function loadTracks() {
  if (cfg.mode !== "local") return;
  try {
    const o = JSON.parse(localStorage.getItem(trackKey()) || "{}");
    for (const [id, pts] of Object.entries(o))
      for (const p of pts) if (Date.now() - p.at < TRACK_MS) addGps(band(Number(id)), { lat: p.lat, lon: p.lon }, p.at);
  } catch { /* nothing stored */ }
}

function onConn(s, detail) {
  state.conn = s; state.connDetail = detail;
  render();
}

// ------------------------------------------------------------------ derived
const isLost = (b, now) => !!b.status && (b.timeout || now - b.statusAt > LOST_MS);
function effectiveStage(b, now) {
  if (!b.status) return null;
  return isLost(b, now) ? 3 : b.status.stage;
}
const children = () => [...state.bands.values()].filter((b) => !isTeacher(b.id)).sort((a, b) => a.id - b.id);

function groupState(now) {
  const kids = children().filter((b) => b.status);
  const by = [[], [], [], []];
  for (const b of kids) by[effectiveStage(b, now)].push(b);
  const sos = state.alarms.find((a) => (a.reason === 2 || a.reason === 3) && now - a.at < 30000);
  const list = (arr) => arr.map((b) => name(b.id)).join(", ");
  let g;
  if (!kids.length) g = { cls: "none", text: "Warte auf Daten …", sub: "Noch kein Band gemeldet." };
  else if (sos) g = { cls: "s3", text: sos.reason === 3 ? `SOS von ${name(sos.band)}!` : `Band von ${name(sos.band)} geöffnet!`,
                      sub: "Bitte sofort nachsehen." };
  else if (by[3].length) g = { cls: "s3", text: `${list(by[3])} ${by[3].length > 1 ? "haben" : "hat"} keinen Kontakt`, sub: "Bitte suchen und zurückrufen." };
  else if (by[2].length) g = { cls: "s2", text: `${list(by[2])} fast außer Reichweite`, sub: "Jetzt zurückrufen." };
  else if (by[1].length) g = { cls: "s1", text: `${list(by[1])} entfernt sich`, sub: "Im Auge behalten." };
  else g = { cls: "s0", text: kids.length === 1 ? "Das Kind ist bei der Gruppe" : `Alle ${kids.length} Kinder beisammen`,
             sub: "Alles in Ordnung." };
  g.counts = by.map((a) => a.length);
  return g;
}

// ------------------------------------------------------------------ render
function render() {
  const now = Date.now();
  const g = groupState(now);
  document.getElementById("group").className = `banner ${g.cls}`;
  document.getElementById("group-text").textContent = g.text;
  document.getElementById("group-sub").textContent = g.sub;
  setHtml(document.getElementById("counts"), g.counts.some(Boolean)
    ? g.counts.map((n, i) => n ? `<span class="count ${STAGES[i].cls}"><b>${n}</b> ${STAGES[i].name}</span>` : "").join("") : "");

  const conn = document.getElementById("conn");
  conn.className = `pill ${state.conn}`;
  conn.textContent = `${source.label}: ${state.conn === "online" ? "verbunden" : state.conn === "connecting" ? "verbinde…" : "getrennt"}`;
  conn.title = state.connDetail;
  const t = document.getElementById("teacher");
  t.className = `pill ${state.teacherOnline === false ? "offline" : state.teacherOnline ? "online" : "connecting"}`;
  t.textContent = `Lehrer-Band: ${state.teacherOnline === false ? "offline" : state.teacherOnline ? "online" : "?"}`;
  document.getElementById("offline-hint").hidden = !(cfg.mode === "cloud" && state.conn === "offline");

  renderCards(now);
  renderNet(now);
  renderFeed();
  updateMap(now);
}

function setHtml(el, html) { if (el.innerHTML !== html) el.innerHTML = html; }

// Each card element is created once; only its body is updated, so the recall button
// is never replaced while the teacher taps it.
function renderCards(now) {
  const grid = document.getElementById("bands");
  const kids = children();
  grid.querySelector(".empty-msg")?.remove();
  if (!kids.length && !grid.children.length) grid.innerHTML = `<p class="empty-msg">Noch keine Kinder-Bänder empfangen.</p>`;
  for (const b of kids) {
    let el = grid.querySelector(`[data-band="${b.id}"]`);
    if (!el) {
      el = document.createElement("article");
      el.dataset.band = b.id;
      el.innerHTML = `<div class="body"></div>${source.canSend ? `<button data-recall="${b.id}">🔔 ${esc(name(b.id))} zurückrufen</button>` : ""}`;
      const next = [...grid.children].find((c) => Number(c.dataset.band) > b.id);   // keep numeric order
      grid.insertBefore(el, next || null);
    }
    const { cls, html } = card(b, now);
    el.className = `card ${cls}`;
    setHtml(el.querySelector(".body"), html);
  }
}

function card(b, now) {
  const st = effectiveStage(b, now);
  const s = b.status || {};
  const lost = isLost(b, now);
  const stage = st == null ? { name: "–", cls: "none", say: "noch keine Daten" } : STAGES[st];
  const sig = signal(lost ? null : s.rssi);
  const say = lost ? `keine Meldung ${ago(b.statusAt, now)} – außer Reichweite oder ausgeschaltet` : stage.say;

  let nbHtml;
  if (lost) nbHtml = `<small>unbekannt</small>`;
  else if (Array.isArray(s.nb)) {
    const chips = s.nb.map((n) => {
      const q = signal(n.rssi);
      return `<span class="chip" title="${esc(name(n.id))}: ${q.word} (${n.rssi} dBm)">${avatar(n.id, "sm")}${esc(name(n.id))}${bars(q.q)}</span>`;
    });
    const more = (s.neighbours ?? 0) - s.nb.length;
    if (more > 0) chips.push(`<span class="chip">+${more} weitere</span>`);
    nbHtml = chips.length ? `<div class="chips">${chips.join("")}</div>` : `<small>niemand in der Nähe</small>`;
  } else {
    const n = s.neighbours ?? 0;
    nbHtml = n ? `${n} ${n === 1 ? "Band" : "Bänder"} in Reichweite` : `<small>niemand in der Nähe</small>`;
  }

  // hops comes from whichever copy reached the teacher first (a relayed copy can win the race),
  // so a band that hears the teacher band directly counts as direct
  const direct = !s.hops || (Array.isArray(s.nb) && s.nb.some((n) => isTeacher(n.id)));
  const via = !direct ?`über ${s.hops} ${s.hops > 1 ? "andere Kinder" : "anderes Kind"} weitergeleitet` : "direkt beim Lehrer-Band";
  const loc = whereabouts(b, now);
  let gps = "";
  if (loc.show) {
    const g = b.gps;
    const words = !g ? "GPS sucht noch Satelliten …"
      : loc.live ? `<b>Live</b> · ${ago(b.gpsAt, now)}`
      : `<b>Letzter bekannter Standort</b> · ${ago(b.gpsAt, now)}`;
    const links = g ? `<span class="loc-links"><button type="button" class="link" data-show="${b.id}">Auf Karte</button>
        <a href="https://maps.google.com/?q=${g.lat},${g.lon}" target="_blank" rel="noopener">Google Maps</a></span>` : "";
    gps = `<div class="gps ${loc.live ? "live" : g ? "last" : "wait"}">📍 <span>${words}</span>${links}</div>`;
  }
  const html = `
    <header>${avatar(b.id)}<div><h3>${esc(name(b.id))}</h3>${name(b.id) === `Band ${b.id}` ? "" : `<small>Band ${b.id}</small>`}</div><span class="badge">${stage.name}</span></header>
    <p class="say">${esc(name(b.id))} ${say}</p>
    <dl class="rows">
      <dt>Signal</dt><dd title="stärkstes Signal zu einem Nachbarn${s.rssi > -128 ? ` (${s.rssi} dBm)` : ""}">${bars(sig.q)}${sig.dist} <small>· ${sig.word}</small></dd>
      <dt>Nachbarn</dt><dd>${nbHtml}</dd>
      <dt>Verbindung</dt><dd>${lost ? "<b>keine</b>" : via}</dd>
      <dt>Zuletzt</dt><dd>${ago(b.statusAt, now)}</dd>
    </dl>
    ${gps}`;
  return { cls: stage.cls, html };
}

// Where is the child? Shown from stage "critical", while lost, and for 2 min after a position.
function whereabouts(b, now) {
  const st = effectiveStage(b, now);
  const fresh = b.gpsAt && now - b.gpsAt < LIVE_MS;
  return { show: st >= 2 || (b.gpsAt && now - b.gpsAt < 120000), live: fresh && !isLost(b, now) };
}

// "Wer ist bei wem?": teacher in the middle, children around it, a line for every
// pair that hears each other (from the nb lists in STATUS, firmware v2).
function renderNet(now) {
  const W = 420, H = 300, cx = W / 2, cy = H / 2 - 6, R = 108;
  const kids = children();
  const ids = new Set(kids.map((b) => b.id));
  for (const b of kids) if (!isLost(b, now)) for (const n of b.status?.nb || []) if (!isTeacher(n.id)) ids.add(n.id);
  const list = [...ids].sort((a, b) => a - b);
  const pos = new Map([[TEACHER, [cx, cy]]]);
  list.forEach((id, i) => {
    const a = -Math.PI / 2 + (2 * Math.PI * i) / Math.max(list.length, 1);
    pos.set(id, [cx + R * 1.35 * Math.cos(a), cy + R * Math.sin(a)]);
  });

  const edges = new Map();               // "a-b" -> best rssi of both directions
  let anyNb = false;
  for (const b of kids) {
    if (isLost(b, now) || !Array.isArray(b.status?.nb)) continue;
    anyNb = true;
    for (const n of b.status.nb) {
      if (!pos.has(n.id)) continue;
      const k = [Math.min(b.id, n.id), Math.max(b.id, n.id)].join("-");
      edges.set(k, Math.max(edges.get(k) ?? -128, n.rssi));
    }
  }

  let svg = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Nachbarschaft der Kinder">`;
  for (const [k, rssi] of edges) {
    const [a, b] = k.split("-").map(Number);
    const [x1, y1] = pos.get(a), [x2, y2] = pos.get(b);
    const q = signal(rssi);
    svg += `<line class="edge q${q.q}" x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"><title>${esc(name(a))} ↔ ${esc(name(b))}: ${q.dist} (${rssi} dBm)</title></line>`;
  }
  const node = (id, r, stageCls, lost) => {
    const [x, y] = pos.get(id);
    const ring = stageCls ? `var(--${stageCls})` : "var(--line)";
    return `<g class="node${lost ? " lost" : ""}">
      <circle class="ring" cx="${x}" cy="${y}" r="${r + 5}" style="stroke:${ring}"/>
      <circle cx="${x}" cy="${y}" r="${r}" fill="${color(id)}"/>
      <text class="ini" x="${x}" y="${y}">${esc(initial(id))}</text>
      <text class="lbl" x="${x}" y="${y + r + 17}">${esc(name(id))}</text></g>`;
  };
  svg += node(TEACHER, 24, state.teacherOnline === false ? "s3" : state.teacherOnline ? "s0" : null, state.teacherOnline === false);
  for (const id of list) {
    const b = state.bands.get(id);
    const st = b ? effectiveStage(b, now) : null;
    svg += node(id, 18, st == null ? null : STAGES[st].cls, b ? isLost(b, now) : true);
  }
  if (!list.length) svg += `<text class="empty" x="${cx}" y="${cy + 60}">Noch keine Kinder empfangen</text>`;
  else if (!anyNb) svg += `<text class="empty" x="${cx}" y="${H - 6}">Nachbar-Liste fehlt – Bänder mit Firmware v2 flashen</text>`;
  svg += "</svg>";
  setHtml(document.getElementById("net"), svg);
}

function renderFeed() {
  const icon = { 0: "📡", 1: "📏", 2: "🔓", 3: "🆘" };
  const html = state.alarms.slice(0, 50).map((a) => {
    let what, detail = "";
    if (a.reason === 1) {
      const to = STAGES[a.stage], from = STAGES[a.prev];
      what = a.stage < a.prev ? `${esc(name(a.band))} ist wieder näher` : `${esc(name(a.band))} ${to?.name ?? "?"}`;
      detail = `${from?.name ?? "?"} → ${to?.name ?? "?"}`;
    } else if (a.reason === 2) what = `Band von ${esc(name(a.band))} wurde geöffnet`;
    else if (a.reason === 3) what = `${esc(name(a.band))} hat SOS gedrückt`;
    else if (a.reason === 0) { what = `Kein Kontakt mehr zu ${esc(name(a.band))}`; detail = "vom Lehrer-Band festgestellt"; }
    else what = `${esc(name(a.band))}: Grund ${a.reason}`;
    if (a.hops) detail += `${detail ? " · " : ""}über ${a.hops} ${a.hops > 1 ? "Kinder" : "Kind"} weitergeleitet`;
    const cls = `${STAGES[a.stage]?.cls || ""} ${a.reason >= 2 || a.reason === 0 ? "urgent" : ""}`;
    return `<li class="${cls}"><span class="ic">${icon[a.reason] ?? "•"}</span>
      <span class="what"><b>${what}</b>${detail ? `<small>${detail}</small>` : ""}</span><span class="t">${time(a.at)}</span></li>`;
  }).join("") || `<li class="calm">Keine Meldungen – alles ruhig. 🌤️</li>`;
  setHtml(document.getElementById("alarms"), html);
}

function recall(id) {
  if (source.send(id, "recall")) toast(`Rückruf an ${name(id)} gesendet`);
  else toast("Rückruf nicht möglich (keine Verbindung)");
}

function toast(msg) {
  const el = document.getElementById("toast");
  el.textContent = msg; el.hidden = false;
  clearTimeout(toast.t); toast.t = setTimeout(() => (el.hidden = true), 2500);
}

// ------------------------------------------------------------------ map (only with internet)
// Live track while a child is critical; when the connection is gone the last known
// position stays on the map (red, with its age).
let map, layers = new Map(), fittedIds = "";
function updateMap(now = Date.now()) {
  const wrap = document.getElementById("map-wrap");
  const shown = children().filter((b) => b.gps && whereabouts(b, now).show);
  if (!shown.length || !window.L) { wrap.hidden = true; return; }
  const firstShow = wrap.hidden;
  wrap.hidden = false;
  if (!map) {
    map = L.map("map");
    L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", { attribution: "© OpenStreetMap", maxZoom: 19 }).addTo(map);
  }
  if (firstShow) map.invalidateSize();
  for (const [id, l] of layers) if (!shown.some((b) => b.id === id)) { l.line.remove(); l.marker.remove(); layers.delete(id); }
  for (const b of shown) {
    const lost = isLost(b, now), live = whereabouts(b, now).live;
    const cls = STAGES[effectiveStage(b, now)].cls;
    const pts = b.track.map((p) => [p.lat, p.lon]);
    let l = layers.get(b.id);
    if (!l) {
      l = { line: L.polyline(pts, { weight: 4, opacity: 0.8 }).addTo(map),
            marker: L.marker(pts[pts.length - 1], { zIndexOffset: 1000 }).addTo(map)
              .bindTooltip("", { permanent: true, direction: "right", offset: [14, 0] }) };
      layers.set(b.id, l);
    }
    l.line.setLatLngs(pts);
    l.line.setStyle({ color: color(b.id), dashArray: lost ? "6 8" : null });
    l.marker.setLatLng(pts[pts.length - 1]);
    const key = `${cls}${live ? "-live" : ""}`;
    if (l.key !== key) {
      l.key = key;
      l.marker.setIcon(L.divIcon({ className: `pin ${cls}${live ? " live" : ""}`, iconSize: [30, 30], iconAnchor: [15, 15],
        html: `<span class="avatar sm" style="--a:${color(b.id)}">${esc(initial(b.id))}</span>` }));
    }
    l.marker.setTooltipContent(`${esc(name(b.id))} · ${live ? "live" : lost ? `zuletzt hier, ${ago(b.gpsAt, now)}` : ago(b.gpsAt, now)}`);
  }
  const ids = shown.map((b) => b.id).join(",");
  if (ids !== fittedIds) {                            // re-fit when a child appears or disappears
    fittedIds = ids;
    map.fitBounds(L.latLngBounds(shown.flatMap((b) => b.track.map((p) => [p.lat, p.lon]))).pad(0.3), { maxZoom: 18 });
  }
}

function showOnMap(id) {
  const b = state.bands.get(id);
  if (!b?.gps || !map) return;
  document.getElementById("map-wrap").scrollIntoView({ behavior: "smooth", block: "center" });
  map.setView([b.gps.lat, b.gps.lon], Math.max(map.getZoom(), 17));
}

// ------------------------------------------------------------------ sound
let audio;
function alertSound(times) {
  try {
    audio ||= new AudioContext();
    for (let i = 0; i < times * 2; i++) {
      const o = audio.createOscillator(), g = audio.createGain();
      o.frequency.value = i % 2 ? 1800 : 2400;
      o.connect(g); g.connect(audio.destination);
      const t0 = audio.currentTime + i * 0.25;
      g.gain.setValueAtTime(0.2, t0); g.gain.setValueAtTime(0, t0 + 0.18);
      o.start(t0); o.stop(t0 + 0.2);
    }
    navigator.vibrate?.([200, 100, 200]);
  } catch { /* audio needs a user gesture first */ }
}
document.addEventListener("click", () => { audio ||= new AudioContext(); }, { once: true });

// ------------------------------------------------------------------ start
async function main() {
  cfg = await fetch("config.json", { cache: "no-store" }).then((r) => r.json())
    .catch(() => ({ mode: location.protocol === "https:" ? "cloud" : "local", group: 1 }));  // cloud copy has no config.json
  cfg.group ??= 1;
  LOST_MS = (cfg.lost_s ?? (cfg.mode === "cloud" ? 75 : 8)) * 1000;
  if (new URLSearchParams(location.search).has("demo")) cfg.mode = "demo";
  source = cfg.mode === "cloud" ? new CloudSource(cfg) : cfg.mode === "demo" ? new DemoSource(cfg) : new LocalSource(cfg);
  document.getElementById("trip").textContent = `${cfg.trip || "Ausflug"} · Gruppe ${cfg.group}`;
  document.getElementById("local-link").href = cfg.local_url || "http://raspberrypi47.local:8080";
  document.getElementById("bands").addEventListener("click", (e) => {   // delegated: survives updates
    const btn = e.target.closest("button[data-recall]");
    if (btn) recall(Number(btn.dataset.recall));
    const show = e.target.closest("button[data-show]");
    if (show) showOnMap(Number(show.dataset.show));
  });
  loadTracks();
  render();
  source.start(onEvent, onConn);
  setInterval(render, 1000);   // ages, lost detection
}
main();
