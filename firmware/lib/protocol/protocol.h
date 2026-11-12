// BuddyBand over-the-air protocol (see docs/protocol.md).
// Shared by child bands and the teacher band. Keep in sync with the spec.
#pragma once

#include <stdint.h>
#include <string.h>

namespace bb {

constexpr uint8_t  PROTOCOL_VERSION = 1;
constexpr uint16_t COMPANY_ID       = 0xFFFF;  // BLE "reserved for testing"
constexpr uint8_t  TTL_MAX          = 3;
constexpr uint8_t  TEACHER_ID       = 0;
constexpr size_t   MAX_PACKET       = 24;      // bytes left in a legacy advertisement

enum MsgType : uint8_t {
  MSG_STATUS = 1,
  MSG_ALARM  = 2,
  MSG_GPS    = 3,
  MSG_ACK    = 4,
  MSG_CMD    = 5,
};

enum Stage : uint8_t {
  STAGE_COMPLETE = 0,
  STAGE_DRIFTING = 1,
  STAGE_CRITICAL = 2,
  STAGE_ISOLATED = 3,
};

enum AlarmReason : uint8_t {
  REASON_NEIGHBOUR_LOSS = 1,
  REASON_TAMPER         = 2,
  REASON_SOS            = 3,
};

enum Command : uint8_t {
  CMD_RECALL = 1,
  CMD_STOP   = 2,
};

#pragma pack(push, 1)

struct Header {
  uint8_t  version;
  uint8_t  group;
  uint8_t  type;
  uint8_t  src;   // original sender
  uint8_t  tx;    // node that sent this copy (relays overwrite it)
  uint16_t seq;   // per src
  uint8_t  ttl;   // remaining hops
};

struct StatusPayload {
  uint8_t stage;
  uint8_t neighbours;
  int8_t  best_rssi;
  uint8_t battery;  // percent, 255 = unknown
};

struct AlarmPayload {
  uint8_t stage;
  uint8_t prev_stage;
  uint8_t reason;
};

struct GpsPayload {
  int32_t lat_e7;   // degrees * 1e7
  int32_t lon_e7;
  uint8_t fix_age;  // seconds
};

struct AckPayload {
  uint8_t  dst;
  uint16_t acked_seq;
};

struct CmdPayload {
  uint8_t dst;
  uint8_t cmd;
};

#pragma pack(pop)

static_assert(sizeof(Header) == 8, "header must be 8 bytes");
static_assert(sizeof(Header) + sizeof(GpsPayload) <= MAX_PACKET, "GPS packet too large");

// Payload size for a message type, 0 if unknown.
inline size_t payloadSize(uint8_t type) {
  switch (type) {
    case MSG_STATUS: return sizeof(StatusPayload);
    case MSG_ALARM:  return sizeof(AlarmPayload);
    case MSG_GPS:    return sizeof(GpsPayload);
    case MSG_ACK:    return sizeof(AckPayload);
    case MSG_CMD:    return sizeof(CmdPayload);
    default:         return 0;
  }
}

// Hops travelled so far.
inline uint8_t hops(const Header& h) { return TTL_MAX - h.ttl; }

// A complete packet as it travels over the air.
struct Packet {
  uint8_t data[MAX_PACKET];
  uint8_t len = 0;

  Header& header() { return *reinterpret_cast<Header*>(data); }
  const Header& header() const { return *reinterpret_cast<const Header*>(data); }

  template <typename T>
  T& payload() { return *reinterpret_cast<T*>(data + sizeof(Header)); }
  template <typename T>
  const T& payload() const { return *reinterpret_cast<const T*>(data + sizeof(Header)); }
};

// Builds a packet with header + payload. Returns false if it does not fit.
template <typename T>
inline bool buildPacket(Packet& p, uint8_t group, MsgType type, uint8_t self,
                        uint16_t seq, const T& body) {
  if (sizeof(Header) + sizeof(T) > MAX_PACKET) return false;
  Header h{PROTOCOL_VERSION, group, static_cast<uint8_t>(type), self, self, seq, TTL_MAX};
  memcpy(p.data, &h, sizeof(Header));
  memcpy(p.data + sizeof(Header), &body, sizeof(T));
  p.len = sizeof(Header) + sizeof(T);
  return true;
}

// Checks version, group and length of received bytes and copies them into p.
inline bool parsePacket(const uint8_t* raw, size_t len, uint8_t group, Packet& p) {
  if (len < sizeof(Header) || len > MAX_PACKET) return false;
  const Header* h = reinterpret_cast<const Header*>(raw);
  if (h->version != PROTOCOL_VERSION || h->group != group) return false;
  if (len != sizeof(Header) + payloadSize(h->type)) return false;
  memcpy(p.data, raw, len);
  p.len = len;
  return true;
}

}  // namespace bb
