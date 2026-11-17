// Hardware drivers for the application firmware (WP4). Pins: config.h / architecture.md §2.1.
// Everything is non-blocking: call update()/poll() from loop().
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "nmea.h"

namespace hw {

// ---- piezo: plays short patterns without blocking loop() ----
struct Note { uint16_t hz; uint16_t on_ms; uint16_t off_ms; };

class Buzzer {
 public:
  void begin(int pin);
  void play(const Note* notes, size_t n);  // replaces whatever is playing
  void stop();
  void update(uint32_t now);
  bool busy() const { return n_ > 0; }
 private:
  int pin_ = -1;
  const Note* notes_ = nullptr;
  size_t n_ = 0, i_ = 0;
  bool on_ = false;
  uint32_t next_ = 0;
};

// Predefined patterns
namespace tunes {
extern const Note OK[], DRIFT[], CRITICAL[], ISOLATED[], TAMPER[], SOS[], ACKED[], RECALL[], BOOT[];
extern const size_t OK_N, DRIFT_N, CRITICAL_N, ISOLATED_N, TAMPER_N, SOS_N, ACKED_N, RECALL_N, BOOT_N;
}

// ---- teacher LEDs ----
enum class Led : uint8_t { OFF, GREEN, YELLOW, RED };
void ledsBegin();
void ledsSet(Led l);

// ---- reed switch: tamper = clasp opened after it was closed ----
class Reed {
 public:
  void begin(int pin);
  // Returns true exactly once when a tamper event is detected.
  bool poll(uint32_t now, uint32_t open_ms);
  bool closed() const { return closed_; }
  bool armed() const { return armed_; }
 private:
  int pin_ = -1;
  bool closed_ = false, armed_ = false, reported_ = false;
  uint32_t changed_at_ = 0;
};

// ---- MMA8452Q: double tap via built-in pulse detection ----
class Accel {
 public:
  bool begin(int sda, int scl, uint8_t tap_threshold);  // false if not found
  bool present() const { return addr_ != 0; }
  bool pollDoubleTap();  // reads PULSE_SRC (clears latch); true on double tap
 private:
  uint8_t addr_ = 0;
};

// ---- GPS on UART2 with power control ----
class Gps {
 public:
  void begin(int rx, int tx, int en, uint32_t baud = 9600);
  void power(bool on, uint32_t now);
  bool powered() const { return on_; }
  void poll(uint32_t now);
  const bb::GpsFix& fix() const { return parser_.fix(); }
  uint32_t sentences() const { return parser_.sentences(); }
  uint32_t poweredSince() const { return on_since_; }
  uint32_t firstFixMs() const { return first_fix_ms_; }  // time-to-first-fix of the current power cycle, 0 = none yet
 private:
  int en_ = -1;
  bool on_ = false;
  uint32_t on_since_ = 0, first_fix_ms_ = 0;
  bb::NmeaParser parser_;
};

}  // namespace hw
