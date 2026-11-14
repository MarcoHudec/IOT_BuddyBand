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
constexpr uint32_t ADV_SLOT_MS          = 300;   // airtime per queued packet (~3 adv events) – try 200 (D4b)
constexpr uint32_t RELAY_JITTER_MIN_MS  = 5;     // random delay before a relay copy (protocol.md §4)
constexpr uint32_t RELAY_JITTER_MAX_MS  = 30;
// ALARM delivery (protocol.md §5). Retry > worst-case 2-hop round trip (~1.3 s with 300 ms slots
// would argue for more; 1 s is a compromise for 1 hop, measure in X3).
constexpr uint32_t ALARM_RETRY_MS       = 1000;
constexpr uint8_t  ALARM_FAST_TRIES     = 6;     // 1 + 5 retries, then latched
constexpr uint32_t ALARM_LATCH_MS       = 5000;  // latched: resend until ACKed
constexpr uint32_t TEACHER_CHECK_MS     = 500;   // teacher: per-child isolation check period

// ---- sensors / feedback (WP4) ----
constexpr uint32_t REED_OPEN_MS         = 500;   // clasp open this long (after being closed) -> TAMPER
constexpr uint8_t  TAP_THRESHOLD        = 0x18;  // MMA8452Q pulse threshold, 0.063 g/LSB (~1.5 g)
constexpr uint32_t GPS_OFF_DELAY_MS     = 60000; // GPS off this long after returning to "complete"
constexpr uint32_t GPS_INTERVAL_MS      = 5000;  // GPS packet period from stage "critical"
constexpr uint32_t BAND_REMIND_MS       = 15000; // child band repeats its warning beep (critical/isolated)
constexpr uint32_t TEACHER_REALARM_MS   = 10000; // same alarm from same band: beep again only after this

}  // namespace cfg

// Pin map: docs/architecture.md §2.1 (verified with hwtest on band 2, 14 Nov)
namespace pins {
constexpr int SDA = 21, SCL = 22;          // MMA8452Q
constexpr int GPS_RX = 16, GPS_TX = 17;    // ESP RX <- GPS TX, ESP TX -> GPS RX
constexpr int GPS_EN = 25;                 // HIGH = GPS powered (if the module has EN wired)
constexpr int REED = 27;                   // INPUT_PULLUP, LOW = magnet near = clasp closed
constexpr int PIEZO = 14;
constexpr int LED_G = 25, LED_Y = 26, LED_R = 27;  // teacher band only (as wired on board 0, 14 Nov)
}  // namespace pins

namespace cfg {
// Stage thresholds/dwell times: see bb::StageConfig in lib/stage/stage.h (PROVISIONAL until X1).

}  // namespace cfg
