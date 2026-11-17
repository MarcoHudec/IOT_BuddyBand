// Neighbour table with rolling RSSI median (WP1, protocol.md §9 finding 2).
// Header-only and Arduino-free so it can be unit-tested on the host
// (firmware/test/test_neighbours).
//
// A "neighbour" is identified by the `tx` field of a received packet: the node
// that transmitted this copy (relays overwrite tx), so the RSSI belongs to it.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bb {

constexpr size_t  RSSI_WINDOW     = 10;    // samples in the rolling median
constexpr size_t  MAX_NEIGHBOURS  = 16;
constexpr int8_t  RSSI_NONE       = -128;  // "no value" (also sent in STATUS.best_rssi)
constexpr uint32_t GAP_RESET_MS   = 4000;  // silent longer than this -> old samples are discarded

struct Neighbour {
  bool     used = false;
  uint8_t  id = 0;
  int8_t   samples[RSSI_WINDOW] = {};
  uint8_t  count = 0;      // valid samples (<= RSSI_WINDOW)
  uint8_t  head = 0;       // next write position in the ring buffer
  int8_t   last = RSSI_NONE;
  uint32_t last_seen = 0;  // ms
  uint32_t total = 0;      // packets received since first seen

  void add(int8_t rssi, uint32_t now) {
    // After a long gap the old values describe a past situation (field test 14 Nov:
    // one stray packet after 8 s silence revived a stale median of -38 dBm).
    if (count && (now - last_seen) > GAP_RESET_MS) count = head = 0;
    samples[head] = rssi;
    head = (head + 1) % RSSI_WINDOW;
    if (count < RSSI_WINDOW) count++;
    last = rssi;
    last_seen = now;
    total++;
  }

  // Median of the stored samples; RSSI_NONE if empty. Order in the ring does not matter.
  int8_t median() const {
    if (count == 0) return RSSI_NONE;
    int8_t tmp[RSSI_WINDOW];
    for (uint8_t i = 0; i < count; i++) tmp[i] = samples[i];
    for (uint8_t i = 1; i < count; i++) {  // insertion sort, at most 10 elements
      int8_t v = tmp[i];
      int j = i - 1;
      while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
      tmp[j + 1] = v;
    }
    if (count % 2) return tmp[count / 2];
    return static_cast<int8_t>((tmp[count / 2 - 1] + tmp[count / 2]) / 2);
  }

  // Heard within `window_ms` before `now`? (unsigned subtraction handles millis() wrap)
  bool active(uint32_t now, uint32_t window_ms) const {
    return used && (now - last_seen) <= window_ms;
  }
};

class NeighbourTable {
 public:
  // Adds one RSSI sample. If the table is full, the least recently seen entry is replaced.
  void add(uint8_t id, int8_t rssi, uint32_t now) {
    Neighbour* slot = nullptr;
    for (auto& n : entries_)                       // 1. existing entry
      if (n.used && n.id == id) { slot = &n; break; }
    if (!slot)
      for (auto& n : entries_)                     // 2. free slot
        if (!n.used) { slot = &n; break; }
    if (!slot) {                                   // 3. replace least recently seen
      slot = &entries_[0];
      for (auto& n : entries_)
        if ((now - n.last_seen) > (now - slot->last_seen)) slot = &n;
    }
    if (!slot->used || slot->id != id) {
      *slot = Neighbour{};
      slot->used = true;
      slot->id = id;
    }
    slot->add(rssi, now);
  }

  // Removes neighbours not heard for longer than timeout_ms.
  void expire(uint32_t now, uint32_t timeout_ms) {
    for (auto& n : entries_)
      if (n.used && (now - n.last_seen) > timeout_ms) n = Neighbour{};
  }

  uint8_t countActive(uint32_t now, uint32_t window_ms) const {
    uint8_t c = 0;
    for (const auto& n : entries_) if (n.active(now, window_ms)) c++;
    return c;
  }

  // Strongest median among active neighbours with at least `min_samples` samples,
  // RSSI_NONE if there is none.
  int8_t bestMedian(uint32_t now, uint32_t window_ms, uint8_t min_samples = 1) const {
    int8_t best = RSSI_NONE;
    for (const auto& n : entries_)
      if (n.active(now, window_ms) && n.count >= min_samples && n.median() > best)
        best = n.median();
    return best;
  }

  // Writes up to `max` active neighbours (strongest median first) to ids/rssi,
  // returns how many were written.
  uint8_t strongest(uint32_t now, uint32_t window_ms, uint8_t* ids, int8_t* rssi, uint8_t max) const {
    uint8_t n = 0;
    for (const auto& e : entries_) {
      if (!e.active(now, window_ms) || e.count == 0) continue;
      int8_t m = e.median();
      uint8_t pos = n;                               // insertion into the sorted output
      while (pos > 0 && rssi[pos - 1] < m) {
        if (pos < max) { ids[pos] = ids[pos - 1]; rssi[pos] = rssi[pos - 1]; }
        pos--;
      }
      if (pos < max) { ids[pos] = e.id; rssi[pos] = m; }
      if (n < max) n++;
    }
    return n;
  }

  const Neighbour* find(uint8_t id) const {
    for (const auto& n : entries_) if (n.used && n.id == id) return &n;
    return nullptr;
  }

  static constexpr size_t capacity() { return MAX_NEIGHBOURS; }
  const Neighbour& at(size_t i) const { return entries_[i]; }

 private:
  Neighbour entries_[MAX_NEIGHBOURS];
};

}  // namespace bb
