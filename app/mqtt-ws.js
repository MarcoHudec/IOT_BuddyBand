// Minimal MQTT 3.1.1 client over WebSocket (QoS 0) – no dependencies, works offline.
// Enough for the BuddyBand teacher app: CONNECT, SUBSCRIBE, PUBLISH, PINGREQ, incoming PUBLISH.
// RabbitMQ web_mqtt: ws://<pi>:15675/ws, subprotocol "mqtt".

const enc = new TextEncoder();
const dec = new TextDecoder();

function str(s) {                       // MQTT UTF-8 string: 2-byte length + bytes
  const b = enc.encode(s);
  return [b.length >> 8, b.length & 0xff, ...b];
}

function packet(type, body) {           // fixed header + remaining length (varint)
  const out = [type];
  let n = body.length;
  do {
    let byte = n % 128;
    n = Math.floor(n / 128);
    if (n > 0) byte |= 0x80;
    out.push(byte);
  } while (n > 0);
  return new Uint8Array([...out, ...body]);
}

export class MqttWs {
  constructor({ url, username, password, clientId, keepalive = 30 }) {
    Object.assign(this, { url, username, password, keepalive });
    this.clientId = clientId || `buddyband-app-${Math.random().toString(16).slice(2, 10)}`;
    this.handlers = { message: [], status: [] };
    this.subs = [];
    this.buf = new Uint8Array(0);
    this.packetId = 1;
    this.connected = false;
    this.retry = 1000;
  }

  on(ev, fn) { this.handlers[ev].push(fn); return this; }
  emit(ev, ...a) { this.handlers[ev].forEach((f) => f(...a)); }

  connect() {
    this.ws = new WebSocket(this.url, ["mqtt"]);
    this.ws.binaryType = "arraybuffer";
    this.ws.onopen = () => {
      const flags = 0x02 | (this.username ? 0x80 : 0) | (this.password ? 0x40 : 0);
      const body = [...str("MQTT"), 4, flags, this.keepalive >> 8, this.keepalive & 0xff, ...str(this.clientId)];
      if (this.username) body.push(...str(this.username));
      if (this.password) body.push(...str(this.password));
      this.ws.send(packet(0x10, body));
    };
    this.ws.onmessage = (e) => this.receive(new Uint8Array(e.data));
    this.ws.onclose = () => {
      const was = this.connected;
      this.connected = false;
      clearInterval(this.pinger);
      this.emit("status", "disconnected");
      setTimeout(() => this.connect(), this.retry);
      this.retry = was ? 1000 : Math.min(this.retry * 2, 15000);
    };
    this.ws.onerror = () => {};
  }

  subscribe(filter) {
    this.subs.push(filter);
    if (this.connected) this.sendSubscribe(filter);
  }

  sendSubscribe(filter) {
    const id = this.packetId++ & 0xffff || 1;
    this.ws.send(packet(0x82, [id >> 8, id & 0xff, ...str(filter), 0]));
  }

  publish(topic, payload) {
    if (!this.connected) return false;
    this.ws.send(packet(0x30, [...str(topic), ...enc.encode(payload)]));
    return true;
  }

  receive(chunk) {
    const b = new Uint8Array(this.buf.length + chunk.length);
    b.set(this.buf); b.set(chunk, this.buf.length);
    this.buf = b;
    for (;;) {                                   // parse as many complete packets as present
      if (this.buf.length < 2) return;
      let mult = 1, len = 0, i = 1, byte;
      do {
        if (i >= this.buf.length) return;
        byte = this.buf[i++];
        len += (byte & 0x7f) * mult;
        mult *= 128;
      } while (byte & 0x80);
      if (this.buf.length < i + len) return;
      const type = this.buf[0] >> 4, flags = this.buf[0] & 0x0f;
      const body = this.buf.subarray(i, i + len);
      this.buf = this.buf.slice(i + len);
      this.handle(type, flags, body);
    }
  }

  handle(type, flags, body) {
    if (type === 2) {                                         // CONNACK
      if (body[1] !== 0) { this.emit("status", `refused (${body[1]})`); this.ws.close(); return; }
      this.connected = true;
      this.retry = 1000;
      this.subs.forEach((f) => this.sendSubscribe(f));
      this.pinger = setInterval(() => this.ws.send(new Uint8Array([0xc0, 0])), (this.keepalive * 1000) / 2);
      this.emit("status", "connected");
    } else if (type === 3) {                                  // PUBLISH
      const tlen = (body[0] << 8) | body[1];
      const topic = dec.decode(body.subarray(2, 2 + tlen));
      const qos = (flags >> 1) & 3;
      const start = 2 + tlen + (qos ? 2 : 0);
      this.emit("message", topic, dec.decode(body.subarray(start)), Boolean(flags & 1));
    }
  }
}
