#ifdef ROLE_TEACHER

#include "uplink.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <WiFi.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "firmware/src/app/secrets.h missing: run tools/make_secrets.sh (or copy secrets.h.example) and set WIFI_SSID / WIFI_PASS"
#endif

namespace uplink {
namespace {

WiFiClient net;
PubSubClient mqtt(net);
uint8_t grp = 1;
CmdHandler handler = nullptr;
char topicOnline[40], topicCmd[40];
IPAddress brokerIp;
bool haveIp = false, mdnsStarted = false, wifiWasUp = false;
uint32_t lastMqttTry = 0, nPublished = 0, nFailed = 0;

bool resolveBroker() {
  if (haveIp) return true;
  const char* host = MQTT_HOST;
  size_t n = strlen(host);
  if (n > 6 && strcmp(host + n - 6, ".local") == 0) {
    if (!mdnsStarted) mdnsStarted = MDNS.begin("buddyband-teacher");
    char name[64];
    size_t len = n - 6 < sizeof name - 1 ? n - 6 : sizeof name - 1;
    memcpy(name, host, len);
    name[len] = 0;
    IPAddress ip = MDNS.queryHost(name, 1500);
    if (ip != IPAddress(0, 0, 0, 0)) { brokerIp = ip; haveIp = true; }
  } else if (WiFi.hostByName(host, brokerIp)) {
    haveIp = true;
  }
#ifdef MQTT_HOST_IP
  if (!haveIp && brokerIp.fromString(MQTT_HOST_IP)) haveIp = true;
#endif
  return haveIp;
}

void onMessage(char* topic, uint8_t* payload, unsigned int len) {
  const char* last = strrchr(topic, '/');
  if (!last || !handler) return;
  int dst = atoi(last + 1);
  char buf[64];
  len = len < sizeof buf - 1 ? len : sizeof buf - 1;
  memcpy(buf, payload, len);
  buf[len] = 0;
  uint8_t cmd = strstr(buf, "recall") ? bb::CMD_RECALL : strstr(buf, "stop") ? bb::CMD_STOP : 0;
  Serial.printf("# MQTT cmd %s -> %s\n", topic, buf);
  if (cmd && dst > 0 && dst < 255) handler(static_cast<uint8_t>(dst), cmd);
}

bool pub(const char* topic, const char* json, bool retain = false) {
  if (!mqtt.connected()) { nFailed++; return false; }
  bool ok = mqtt.publish(topic, json, retain);
  ok ? nPublished++ : nFailed++;
  return ok;
}

void bandTopic(char* out, size_t n, uint8_t id, const char* kind) {
  snprintf(out, n, "buddyband/%u/band/%u/%s", grp, id, kind);
}

}  // namespace

void begin(uint8_t group, CmdHandler onCmd) {
  grp = group;
  handler = onCmd;
  snprintf(topicOnline, sizeof topicOnline, "buddyband/%u/teacher/online", grp);
  snprintf(topicCmd, sizeof topicCmd, "buddyband/%u/cmd/+", grp);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);           // modem sleep is required for Wi-Fi/BLE coexistence on the ESP32
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(2);
  mqtt.setCallback(onMessage);
  Serial.printf("# uplink: Wi-Fi '%s', broker %s:%d as %s\n", WIFI_SSID, MQTT_HOST, MQTT_PORT, MQTT_USER);
}

void loop(uint32_t now) {
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp != wifiWasUp) {
    wifiWasUp = wifiUp;
    if (wifiUp) Serial.printf("# Wi-Fi up, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else { Serial.println("# Wi-Fi down"); haveIp = false; }
  }
  if (!wifiUp) return;

  if (!mqtt.connected()) {
    if (now - lastMqttTry < 5000) return;
    lastMqttTry = now;
    if (!resolveBroker()) { Serial.printf("# MQTT: cannot resolve %s\n", MQTT_HOST); return; }
    mqtt.setServer(brokerIp, MQTT_PORT);
    char id[32];
    snprintf(id, sizeof id, "buddyband-teacher-%u", grp);
    if (mqtt.connect(id, MQTT_USER, MQTT_PASS, topicOnline, 0, true, "false")) {
      mqtt.publish(topicOnline, "true", true);
      mqtt.subscribe(topicCmd);
      Serial.printf("# MQTT connected to %s (%s)\n", MQTT_HOST, brokerIp.toString().c_str());
    } else {
      Serial.printf("# MQTT connect failed, state %d\n", mqtt.state());
      haveIp = false;  // re-resolve next time (hotspot may have given the Pi a new IP)
    }
    return;
  }
  mqtt.loop();
}

bool online() { return mqtt.connected(); }
uint32_t published() { return nPublished; }
uint32_t failed() { return nFailed; }

const char* stateText() {
  if (WiFi.status() != WL_CONNECTED) return "wifi-down";
  return mqtt.connected() ? "online" : "mqtt-down";
}

void publishStatus(const bb::Packet& p) {
  const bb::Header& h = p.header();
  const bb::StatusPayload& s = p.payload<bb::StatusPayload>();
  char topic[48], json[128];
  bandTopic(topic, sizeof topic, h.src, "status");
  snprintf(json, sizeof json, "{\"seq\":%u,\"hops\":%u,\"stage\":%u,\"neighbours\":%u,\"rssi\":%d,\"battery\":%u}",
           h.seq, bb::hops(h), s.stage, s.neighbours, s.best_rssi, s.battery);
  pub(topic, json, true);  // retained: a new dashboard sees the last state at once
}

void publishAlarm(const bb::Packet& p) {
  const bb::Header& h = p.header();
  const bb::AlarmPayload& a = p.payload<bb::AlarmPayload>();
  char topic[48], json[128];
  bandTopic(topic, sizeof topic, h.src, "alarm");
  snprintf(json, sizeof json, "{\"seq\":%u,\"hops\":%u,\"stage\":%u,\"prev\":%u,\"reason\":%u}",
           h.seq, bb::hops(h), a.stage, a.prev_stage, a.reason);
  pub(topic, json);
}

void publishGps(const bb::Packet& p) {
  const bb::Header& h = p.header();
  const bb::GpsPayload& g = p.payload<bb::GpsPayload>();
  char topic[48], json[128];
  bandTopic(topic, sizeof topic, h.src, "gps");
  snprintf(json, sizeof json, "{\"seq\":%u,\"hops\":%u,\"lat\":%.7f,\"lon\":%.7f,\"fix_age\":%u}",
           h.seq, bb::hops(h), g.lat_e7 / 1e7, g.lon_e7 / 1e7, g.fix_age);
  pub(topic, json);
}

void publishTimeout(uint8_t src, uint8_t lastStage, uint8_t hops, uint32_t silentMs) {
  char topic[48], json[128];
  bandTopic(topic, sizeof topic, src, "alarm");
  snprintf(json, sizeof json, "{\"seq\":0,\"hops\":%u,\"stage\":%u,\"prev\":%u,\"reason\":%u,\"silent_ms\":%lu}",
           hops, bb::STAGE_ISOLATED, lastStage, REASON_TEACHER_TIMEOUT, (unsigned long)silentMs);
  pub(topic, json);
}

void publishSummary(uint8_t connected, uint8_t total, const char* led) {
  char topic[40], json[96];
  snprintf(topic, sizeof topic, "buddyband/%u/summary", grp);
  snprintf(json, sizeof json, "{\"connected\":%u,\"total\":%u,\"led\":\"%s\"}", connected, total, led);
  pub(topic, json);
}

}  // namespace uplink

#endif  // ROLE_TEACHER
