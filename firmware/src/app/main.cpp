// BuddyBand application – entry point for child bands and the teacher band.
//
// This is a skeleton. Each TODO names the work package that owns it and the
// section of docs/protocol.md it implements.

#include <Arduino.h>
#include "protocol.h"

#ifndef NODE_ID
#error "NODE_ID must be set in platformio.ini"
#endif

#ifdef ROLE_TEACHER
static const char* ROLE = "teacher";
#else
static const char* ROLE = "band";
#endif

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("# BuddyBand %s, node %d, group %d, protocol v%d\n",
                ROLE, NODE_ID, GROUP_ID, bb::PROTOCOL_VERSION);

  // TODO WP1: BLE init, advertising + continuous scan (see src/rssi_test for a working start)
  // TODO WP1: neighbour table (tx -> smoothed RSSI, last seen)
  // TODO WP2: duplicate cache, relay queue, ACK/retry (protocol.md §4, §5)
  // TODO WP4: LEDs, piezo, GPS, accelerometer, reed switch
#ifdef ROLE_TEACHER
  // TODO WP3: Wi-Fi + MQTT (PubSubClient), publish JSON (protocol.md §7)
  // TODO WP3: subscribe buddyband/<group>/cmd/# and flood CMD packets
  // TODO WP2: isolation detection by STATUS timeout (protocol.md §6)
#endif
}

void loop() {
  // TODO WP1: every 2 s compute stage, send STATUS
  // TODO WP2: process received packets, relay, retransmit unacknowledged alarms
  static uint32_t last = 0;
  if (millis() - last >= 5000) {
    last = millis();
    Serial.printf("# alive %lu ms\n", millis());
  }
}
