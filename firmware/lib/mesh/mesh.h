// Managed flooding building blocks (WP2, protocol.md §4). Header-only,
// Arduino-free, host-tested in firmware/test/test_mesh.
//
//   DupCache     – (src, seq) of the last 32 new messages; a copy seen before is dropped
//   TxScheduler  – one BLE advertiser carries one packet at a time: queued packets
//                  (own STATUS, relayed copies, later ALARM/ACK) each get an
//                  advertising slot; when idle the band advertises its latest STATUS
//   OriginTable  – what we know about every original sender (src): last seq, hops,
//                  stage. The teacher band uses it for isolation detection.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

namespace bb {

// ---------------------------------------------------------------------------
class DupCache {
 public:
  static constexpr size_t SIZE = 32;

  // true if (src, seq) is new (and remembers it), false if it is a duplicate.
  bool checkAndAdd(uint8_t src, uint16_t seq) {
    for (size_t i = 0; i < count_; i++)
      if (src_[i] == src && seq_[i] == seq) return false;
    src_[head_] = src;
    seq_[head_] = seq;
    head_ = (head_ + 1) % SIZE;
    if (count_ < SIZE) count_++;
    return true;
  }

 private:
  uint8_t  src_[SIZE] = {};
  uint16_t seq_[SIZE] = {};
  size_t   head_ = 0;
  size_t   count_ = 0;
};

// ---------------------------------------------------------------------------
class TxScheduler {
 public:
  static constexpr size_t QUEUE_LEN = 8;

  explicit TxScheduler(uint32_t slot_ms = 300) : slot_ms_(slot_ms) {}

  // Queues a packet to be advertised for one slot, not before `ready_at`.
  // Returns false (and counts a drop) if the queue is full.
  bool enqueue(const Packet& p, uint32_t ready_at) {
    if (n_ == QUEUE_LEN) { drops_++; return false; }
    q_[n_].pkt = p;
    q_[n_].ready_at = ready_at;
    n_++;
    return true;
  }

  // Packet that is advertised whenever nothing else is due (own latest STATUS).
  void setBackground(const Packet& p) { bg_ = p; has_bg_ = true; bg_on_air_ = false; }

  // Call often. Returns the packet that must be put on air now, or nullptr if the
  // advertisement does not need to change.
  const Packet* tick(uint32_t now) {
    if (busy_ && (int32_t)(now - slot_end_) < 0) return nullptr;  // slot still running

    // earliest-due ready item (FIFO among equal times)
    size_t best = n_;
    for (size_t i = 0; i < n_; i++)
      if ((int32_t)(now - q_[i].ready_at) >= 0 &&
          (best == n_ || (int32_t)(q_[i].ready_at - q_[best].ready_at) < 0))
        best = i;
    if (best < n_) {
      current_ = q_[best].pkt;
      for (size_t i = best + 1; i < n_; i++) q_[i - 1] = q_[i];
      n_--;
      busy_ = true;
      bg_on_air_ = false;
      slot_end_ = now + slot_ms_;
      sent_++;
      return &current_;
    }
    busy_ = false;
    if (has_bg_ && !bg_on_air_) { bg_on_air_ = true; return &bg_; }
    return nullptr;
  }

  size_t   pending() const { return n_; }
  uint32_t drops() const { return drops_; }
  uint32_t sent() const { return sent_; }

 private:
  struct Item { Packet pkt; uint32_t ready_at; };
  Item     q_[QUEUE_LEN];
  size_t   n_ = 0;
  Packet   current_;
  Packet   bg_;
  bool     has_bg_ = false;
  bool     bg_on_air_ = false;
  bool     busy_ = false;
  uint32_t slot_end_ = 0;
  uint32_t slot_ms_;
  uint32_t drops_ = 0;
  uint32_t sent_ = 0;
};

// ---------------------------------------------------------------------------
struct Origin {
  bool     used = false;
  uint8_t  src = 0;
  uint16_t last_seq = 0;
  uint8_t  hops = 0;          // hops of the first copy of the latest message
  uint32_t last_seen = 0;     // ms, any new message from src
  uint32_t messages = 0;      // new (non-duplicate) messages
  // from the latest STATUS
  uint8_t  stage = STAGE_COMPLETE;
  uint8_t  neighbours = 0;
  int8_t   best_rssi = -128;
};

class OriginTable {
 public:
  static constexpr size_t SIZE = 16;

  // Call only for NEW messages (after DupCache).
  void update(const Packet& p, uint32_t now) {
    const Header& h = p.header();
    Origin* o = slot(h.src, now);
    o->last_seq = h.seq;
    o->hops = hops(h);
    o->last_seen = now;
    o->messages++;
    if (h.type == MSG_STATUS) {
      const StatusPayload& s = p.payload<StatusPayload>();
      o->stage = s.stage;
      o->neighbours = s.neighbours;
      o->best_rssi = s.best_rssi;
    } else if (h.type == MSG_ALARM) {
      o->stage = p.payload<AlarmPayload>().stage;
    }
  }

  const Origin* find(uint8_t src) const {
    for (const auto& o : e_) if (o.used && o.src == src) return &o;
    return nullptr;
  }
  static constexpr size_t capacity() { return SIZE; }
  const Origin& at(size_t i) const { return e_[i]; }

 private:
  Origin* slot(uint8_t src, uint32_t now) {
    Origin* free_slot = nullptr;
    Origin* oldest = &e_[0];
    for (auto& o : e_) {
      if (o.used && o.src == src) return &o;
      if (!o.used && !free_slot) free_slot = &o;
      if ((now - o.last_seen) > (now - oldest->last_seen)) oldest = &o;
    }
    Origin* s = free_slot ? free_slot : oldest;
    *s = Origin{};
    s->used = true;
    s->src = src;
    return s;
  }
  Origin e_[SIZE];
};

// ---------------------------------------------------------------------------
// Relay decision (protocol.md §4 rules 4+5). `teacher` = this node is the teacher band.
inline bool shouldRelay(const Packet& p, uint8_t self, bool teacher) {
  const Header& h = p.header();
  if (teacher) return false;                 // teacher is the sink, never relays
  if (h.src == self || h.ttl <= 1) return false;
  if (h.type == MSG_ACK) return p.payload<AckPayload>().dst != self;
  if (h.type == MSG_CMD) return p.payload<CmdPayload>().dst != self;
  return true;                               // STATUS / ALARM / GPS towards the teacher
}

// Copy prepared for relaying: one hop less, this node as transmitter.
inline Packet relayCopy(const Packet& p, uint8_t self) {
  Packet c = p;
  c.header().ttl--;
  c.header().tx = self;
  return c;
}

}  // namespace bb
