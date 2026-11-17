// Stage state machine (WP1, architecture.md §4.1). Header-only, Arduino-free,
// host-tested in firmware/test/test_stage.
//
//   0 complete  : a neighbour with median >= T_near
//   1 drifting  : no neighbour >= T_near, but one >= T_far        (held DWELL_UP)
//   2 critical  : nothing >= T_far, but something heard recently  (held DWELL_UP)
//   3 isolated  : nothing at all from the own group for ISOLATION_MS (immediate)
//
// Going back to a lower stage needs the better condition with +HYST_DB margin,
// held for DWELL_DOWN (prevents flapping from body shadowing, protocol.md §9 finding 3).
// A neighbour only counts once it has MIN_SAMPLES fresh samples (~0.5 s of reception),
// so single stray packets (e.g. power flicker while unplugging) are ignored.
// Leaving "isolated": as soon as such a neighbour exists (-> critical, then normal rules).
#pragma once

#include <stdint.h>
#include "neighbours.h"
#include "protocol.h"

namespace bb {

struct StageConfig {
  int8_t   t_near_dbm      = -70;   // PROVISIONAL – replace with outdoor calibration (X1)
  int8_t   t_far_dbm       = -85;   // PROVISIONAL
  int8_t   hyst_db         = 3;
  uint32_t dwell_up_ms     = 5000;
  uint32_t dwell_down_ms   = 3000;
  uint32_t heard_window_ms = 4000;  // neighbour counts if heard within this window
  uint32_t isolation_ms    = 8000;  // no packet at all -> isolated
  uint8_t  min_samples     = 5;     // fresh samples before a neighbour counts (~0.5 s at 10/s)
};

class StageMachine {
 public:
  explicit StageMachine(const StageConfig& c = StageConfig{}) : cfg_(c) {}

  // Call periodically (e.g. every 250 ms). `last_rx_ms` = time of the last packet from
  // the own group (initialise with boot time). Returns true if the stage changed;
  // then stage()/previous() describe the transition.
  bool update(uint32_t now, const NeighbourTable& table, uint32_t last_rx_ms) {
    const int8_t best = table.bestMedian(now, cfg_.heard_window_ms, cfg_.min_samples);
    const bool silent = (now - last_rx_ms) >= cfg_.isolation_ms;

    // Leaving isolation: a neighbour with enough fresh samples -> "critical" again.
    if (stage_ == STAGE_ISOLATED) {
      if (!silent && best != RSSI_NONE) return change(STAGE_CRITICAL, now);
      return false;
    }
    if (silent) return change(STAGE_ISOLATED, now);

    const uint8_t up   = level(best, 0);
    const uint8_t down = level(best, cfg_.hyst_db);

    if (up > stage_) {                       // worse: must hold for dwell_up
      down_since_ = NONE;
      if (up_since_ == NONE) up_since_ = now;
      if (now - up_since_ >= cfg_.dwell_up_ms) return change(up, now);
    } else if (down < stage_) {              // better (with margin): must hold for dwell_down
      up_since_ = NONE;
      if (down_since_ == NONE) down_since_ = now;
      if (now - down_since_ >= cfg_.dwell_down_ms) return change(down, now);
    } else {
      up_since_ = down_since_ = NONE;
    }
    return false;
  }

  uint8_t  stage() const { return stage_; }
  uint8_t  previous() const { return prev_; }
  uint32_t since() const { return changed_at_; }
  const StageConfig& config() const { return cfg_; }

 private:
  static constexpr uint32_t NONE = 0xFFFFFFFFu;

  // Stage that the current best median corresponds to, thresholds raised by `margin`.
  uint8_t level(int8_t best, int8_t margin) const {
    if (best == RSSI_NONE) return STAGE_CRITICAL;  // heard recently, but no active neighbour
    if (best >= cfg_.t_near_dbm + margin) return STAGE_COMPLETE;
    if (best >= cfg_.t_far_dbm + margin) return STAGE_DRIFTING;
    return STAGE_CRITICAL;
  }

  bool change(uint8_t s, uint32_t now) {
    prev_ = stage_;
    stage_ = s;
    changed_at_ = now;
    up_since_ = down_since_ = NONE;
    return true;
  }

  StageConfig cfg_;
  uint8_t  stage_ = STAGE_COMPLETE;
  uint8_t  prev_ = STAGE_COMPLETE;
  uint32_t changed_at_ = 0;
  uint32_t up_since_ = NONE;
  uint32_t down_since_ = NONE;
};

inline const char* stageName(uint8_t s) {
  switch (s) {
    case STAGE_COMPLETE: return "complete";
    case STAGE_DRIFTING: return "drifting";
    case STAGE_CRITICAL: return "critical";
    case STAGE_ISOLATED: return "isolated";
    default:             return "?";
  }
}

}  // namespace bb
