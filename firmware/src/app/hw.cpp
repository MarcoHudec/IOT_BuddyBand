#include "hw.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

namespace hw {

// ===================================================================== buzzer
void Buzzer::begin(int pin) {
  pin_ = pin;
  pinMode(pin_, OUTPUT);
  digitalWrite(pin_, LOW);
}

void Buzzer::play(const Note* notes, size_t n) {
  stop();
  notes_ = notes;
  n_ = n;
  i_ = 0;
  on_ = false;
  next_ = millis();
}

void Buzzer::stop() {
  if (pin_ >= 0) noTone(pin_);
  n_ = 0;
  on_ = false;
}

void Buzzer::update(uint32_t now) {
  if (!n_ || (int32_t)(now - next_) < 0) return;
  if (!on_) {
    if (i_ >= n_) { stop(); return; }
    if (notes_[i_].hz) tone(pin_, notes_[i_].hz);
    on_ = true;
    next_ = now + notes_[i_].on_ms;
  } else {
    noTone(pin_);
    on_ = false;
    next_ = now + notes_[i_].off_ms;
    i_++;
  }
}

namespace tunes {
#define TUNE(name, ...) const Note name[] = {__VA_ARGS__}; const size_t name##_N = sizeof(name) / sizeof(Note);
TUNE(OK,       {3000, 60, 0})
TUNE(DRIFT,    {2000, 150, 0})
TUNE(CRITICAL, {2500, 150, 120}, {2500, 150, 0})
TUNE(ISOLATED, {1500, 400, 200}, {1500, 400, 200}, {1500, 400, 0})
TUNE(TAMPER,   {3200, 80, 60}, {3200, 80, 60}, {3200, 80, 60}, {3200, 80, 60}, {3200, 80, 0})
TUNE(SOS,      {2700, 100, 100}, {2700, 100, 100}, {2700, 100, 250},
               {2700, 300, 100}, {2700, 300, 100}, {2700, 300, 250},
               {2700, 100, 100}, {2700, 100, 100}, {2700, 100, 0})
TUNE(ACKED,    {2500, 80, 60}, {3500, 120, 0})
TUNE(RECALL,   {2000, 200, 50}, {3000, 200, 50}, {2000, 200, 50}, {3000, 200, 50}, {2000, 200, 50}, {3000, 200, 0})
TUNE(BOOT,     {2000, 60, 40}, {3000, 60, 0})
#undef TUNE
}  // namespace tunes

// ===================================================================== LEDs
void ledsBegin() {
  for (int p : {pins::LED_G, pins::LED_Y, pins::LED_R}) {
    pinMode(p, OUTPUT);
    digitalWrite(p, LOW);
  }
}

void ledsSet(Led l) {
  digitalWrite(pins::LED_G, l == Led::GREEN);
  digitalWrite(pins::LED_Y, l == Led::YELLOW);
  digitalWrite(pins::LED_R, l == Led::RED);
}

// ===================================================================== reed
void Reed::begin(int pin) {
  pin_ = pin;
  pinMode(pin_, INPUT_PULLUP);
  closed_ = digitalRead(pin_) == LOW;
  armed_ = closed_;
  changed_at_ = millis();
}

bool Reed::poll(uint32_t now, uint32_t open_ms) {
  bool c = digitalRead(pin_) == LOW;
  if (c != closed_) {
    closed_ = c;
    changed_at_ = now;
    if (c) { armed_ = true; reported_ = false; }  // clasp closed (again): arm
  }
  if (armed_ && !closed_ && !reported_ && now - changed_at_ >= open_ms) {
    reported_ = true;
    return true;
  }
  return false;
}

// ===================================================================== MMA8452Q
namespace {
bool i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
bool i2cRead(uint8_t addr, uint8_t reg, uint8_t& val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
  val = Wire.read();
  return true;
}
}  // namespace

bool Accel::begin(int sda, int scl, uint8_t ths) {
  Wire.begin(sda, scl, 400000);
  for (uint8_t a : {0x1D, 0x1C}) {
    uint8_t who = 0;
    if (i2cRead(a, 0x0D, who) && who == 0x2A) { addr_ = a; break; }
  }
  if (!addr_) return false;
  // MMA8452Q datasheet / AN4072: double pulse on all axes, latched, 800 Hz ODR
  i2cWrite(addr_, 0x2A, 0x00);   // CTRL_REG1: standby while configuring
  i2cWrite(addr_, 0x0E, 0x00);   // XYZ_DATA_CFG: +-2 g
  i2cWrite(addr_, 0x21, 0x6A);   // PULSE_CFG: ELE | ZDPEFE | YDPEFE | XDPEFE
  i2cWrite(addr_, 0x23, ths);    // PULSE_THSX
  i2cWrite(addr_, 0x24, ths);    // PULSE_THSY
  i2cWrite(addr_, 0x25, ths);    // PULSE_THSZ
  i2cWrite(addr_, 0x26, 0x30);   // PULSE_TMLT: 48 * 0.625 ms = 30 ms max pulse width
  i2cWrite(addr_, 0x27, 0xA0);   // PULSE_LTCY: 160 * 1.25 ms = 200 ms latency
  i2cWrite(addr_, 0x28, 0xFF);   // PULSE_WIND: 255 * 1.25 ms = 318 ms window for 2nd tap
  i2cWrite(addr_, 0x2D, 0x08);   // CTRL_REG4: pulse interrupt enable
  i2cWrite(addr_, 0x2E, 0x08);   // CTRL_REG5: route pulse to INT1 (polled anyway)
  i2cWrite(addr_, 0x2A, 0x01);   // CTRL_REG1: active, 800 Hz
  return true;
}

bool Accel::pollDoubleTap() {
  uint8_t src = 0;
  if (!addr_ || !i2cRead(addr_, 0x22, src)) return false;  // PULSE_SRC, read clears latch
  return (src & 0x80) && (src & 0x08);                     // EA && DPE
}

// ===================================================================== GPS
void Gps::begin(int rx, int tx, int en, uint32_t baud) {
  en_ = en;
  pinMode(en_, OUTPUT);
  digitalWrite(en_, LOW);
  Serial2.begin(baud, SERIAL_8N1, rx, tx);
}

void Gps::power(bool on, uint32_t now) {
  if (on == on_) return;
  on_ = on;
  digitalWrite(en_, on ? HIGH : LOW);
  if (on) { on_since_ = now; first_fix_ms_ = 0; }
}

void Gps::poll(uint32_t now) {
  while (Serial2.available()) {
    if (parser_.feed(Serial2.read(), now) && on_ && first_fix_ms_ == 0)
      first_fix_ms_ = now - on_since_ ? now - on_since_ : 1;
  }
}

}  // namespace hw
