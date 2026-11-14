// Application timing and radio parameters (protocol.md §6 – start values, calibrate in week 2).
#pragma once

#include <stdint.h>

namespace cfg {

constexpr uint32_t STATUS_INTERVAL_MS   = 2000;  // new STATUS packet (new seq)
constexpr uint32_t ACTIVE_WINDOW_MS     = 4000;  // neighbour counts as present if heard within
constexpr uint32_t NEIGHBOUR_TIMEOUT_MS = 8000;  // neighbour removed from table (= isolation time)
constexpr uint32_t DEBUG_INTERVAL_MS    = 2000;  // '#' neighbour table on serial
constexpr int8_t   TX_POWER_DBM         = 9;     // same as RSSI calibration – keep identical on all boards
constexpr uint32_t STAGE_TICK_MS        = 250;   // stage machine evaluation period
// Stage thresholds/dwell times: see bb::StageConfig in lib/stage/stage.h (PROVISIONAL until X1).

}  // namespace cfg
