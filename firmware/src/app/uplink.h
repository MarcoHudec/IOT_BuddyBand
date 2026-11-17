// Teacher band uplink: Wi-Fi + MQTT to the Raspberry Pi gateway (WP3, protocol.md §7).
// Only compiled into the teacher build (ROLE_TEACHER). Non-blocking except for the
// short MQTT connect attempt (socket timeout 2 s), retried every 5 s.
//
// Topics (group g, band id):
//   buddyband/g/band/id/status   retained, every STATUS  {"seq","hops","stage","neighbours","rssi","battery"}
//   buddyband/g/band/id/alarm    every ALARM copy + teacher timeout {"seq","hops","stage","prev","reason"}
//   buddyband/g/band/id/gps      every GPS                {"seq","hops","lat","lon","fix_age"}
//   buddyband/g/summary          every 2 s                {"connected","total","led"}
//   buddyband/g/teacher/online   retained, "true" / LWT "false" (architecture.md D11)
//   buddyband/g/cmd/id           subscribed               {"cmd":"recall"} | {"cmd":"stop"}
#pragma once

#include <stdint.h>
#include "protocol.h"

namespace uplink {

// reason value used for the isolation detected by the teacher (no message for 8 s);
// band alarms use bb::AlarmReason 1..3
constexpr uint8_t REASON_TEACHER_TIMEOUT = 0;

using CmdHandler = void (*)(uint8_t dst, uint8_t cmd);

void begin(uint8_t group, CmdHandler onCmd);
void loop(uint32_t now);
bool online();
const char* stateText();
uint32_t published();
uint32_t failed();

void publishStatus(const bb::Packet& p);
void publishAlarm(const bb::Packet& p);
void publishGps(const bb::Packet& p);
void publishTimeout(uint8_t src, uint8_t lastStage, uint8_t hops, uint32_t silentMs);
void publishSummary(uint8_t connected, uint8_t total, const char* led);

}  // namespace uplink
