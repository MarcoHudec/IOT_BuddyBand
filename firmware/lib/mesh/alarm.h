// Reliable ALARM delivery (WP2, protocol.md §5, architecture.md §4 D5).
// Header-only, Arduino-free, host-tested in firmware/test/test_mesh.
//
// - A band has at most one pending alarm; a newer alarm replaces an older one
//   (the latest state is what the teacher needs).
// - Every (re)transmission is a NEW message with a new seq: relays drop copies
//   of a (src, seq) they have already seen, so a retransmission with the old seq
//   would never get past the first hop.
// - The teacher ACKs each copy it receives; an ACK for any seq used by the
//   pending alarm completes it.
// - Fast phase: retry every `retry_ms` up to `fast_tries` transmissions.
//   Then the alarm is LATCHED and re-sent every `latch_ms` until ACKed, so an
//   isolated child's alarm still arrives when it comes back into range.
#pragma once

#include <stdint.h>
#include "protocol.h"

namespace bb {

class AlarmSender {
 public:
  AlarmSender(uint32_t retry_ms = 1000, uint8_t fast_tries = 6, uint32_t latch_ms = 5000)
      : retry_ms_(retry_ms), fast_tries_(fast_tries), latch_ms_(latch_ms) {}

  void raise(const AlarmPayload& a, uint32_t now) {
    if (pending_) superseded_++;
    alarm_ = a;
    pending_ = true;
    tries_ = 0;
    raised_at_ = now;
  }

  // A (re)transmission is due now. The caller builds the packet with a fresh seq
  // and then calls sent().
  bool due(uint32_t now) const {
    if (!pending_) return false;
    if (tries_ == 0) return true;
    uint32_t wait = tries_ < fast_tries_ ? retry_ms_ : latch_ms_;
    return (now - last_tx_) >= wait;
  }

  void sent(uint16_t seq, uint32_t now) {
    if (tries_ == 0) first_seq_ = seq;
    last_seq_ = seq;
    last_tx_ = now;
    tries_++;
    total_tx_++;
  }

  // Returns true if `acked_seq` belongs to the pending alarm (-> alarm completed).
  bool onAck(uint16_t acked_seq, uint32_t now) {
    if (!pending_ || tries_ == 0) return false;
    uint16_t span = static_cast<uint16_t>(last_seq_ - first_seq_);
    uint16_t off  = static_cast<uint16_t>(acked_seq - first_seq_);
    if (off > span) return false;  // ACK for an older, superseded alarm
    pending_ = false;
    acked_++;
    last_rtt_ = now - raised_at_;
    last_tries_ = tries_;
    return true;
  }

  bool pending() const { return pending_; }
  bool latched() const { return pending_ && tries_ >= fast_tries_; }
  const AlarmPayload& payload() const { return alarm_; }
  uint8_t  tries() const { return tries_; }
  uint32_t totalTx() const { return total_tx_; }
  uint32_t acked() const { return acked_; }
  uint32_t superseded() const { return superseded_; }
  uint32_t lastRtt() const { return last_rtt_; }      // raise -> ACK, ms
  uint8_t  lastTries() const { return last_tries_; }  // transmissions until ACK

 private:
  uint32_t retry_ms_;
  uint8_t  fast_tries_;
  uint32_t latch_ms_;

  AlarmPayload alarm_{};
  bool     pending_ = false;
  uint8_t  tries_ = 0;
  uint16_t first_seq_ = 0, last_seq_ = 0;
  uint32_t raised_at_ = 0, last_tx_ = 0;

  uint32_t total_tx_ = 0, acked_ = 0, superseded_ = 0, last_rtt_ = 0;
  uint8_t  last_tries_ = 0;
};

}  // namespace bb
