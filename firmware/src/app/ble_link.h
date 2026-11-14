// BLE link layer: one non-connectable advertisement (our packet in the
// manufacturer data, company ID 0xFFFF) + continuous passive scan.
// See protocol.md §3. Classic ESP32 = BLE 4.2, only one advertising set.
#pragma once

#include <stdint.h>
#include "protocol.h"

namespace bb {

struct RxPacket {
  Packet   pkt;   // already checked: version, group, length
  int8_t   rssi;
  uint32_t ms;    // millis() at reception
};

// Starts NimBLE, advertising (100 ms interval) and scanning. Call once in setup().
void bleBegin(int8_t txPowerDbm);

// Replaces the packet that is currently being advertised.
void bleAdvertise(const Packet& p);

// Non-blocking: takes the next received packet from the scan queue.
bool bleReceive(RxPacket& out);

// Number of received packets lost because the queue was full (should stay 0).
uint32_t bleDropped();

}  // namespace bb
