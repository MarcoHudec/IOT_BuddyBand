// Hardware self-test for BuddyBand boards (WP4). Not the application!
//
//   pio run -e hwtest_band -t upload      child band: piezo, reed, MMA8452Q, GPS
//   pio run -e hwtest_teacher -t upload   teacher band: LEDs, piezo
//   pio device monitor -e hwtest_band
//
// Pin map = architecture.md §2.1. Change the constants below if your wiring differs.
// Auto-detection: I2C bus scan, GPS baud rate (4800/9600/38400/115200) and
// swapped RX/TX lines are tried automatically.
//
// Serial commands: b = beep, s = status now, g = re-detect GPS,
//                  h = piezo DC 2 s, t = tone sweep, p = beep on every free GPIO

#include <Arduino.h>
#include <Wire.h>

// ---- pins (architecture.md §2.1) ----
constexpr int PIN_SDA      = 21;
constexpr int PIN_SCL      = 22;
constexpr int PIN_ACC_INT1 = 34;
constexpr int PIN_GPS_A    = 16;  // expected: ESP RX  <- GPS TX
constexpr int PIN_GPS_B    = 17;  // expected: ESP TX  -> GPS RX
constexpr int PIN_GPS_EN   = 25;  // HIGH = GPS on (harmless if not connected)
constexpr int PIN_REED     = 27;
constexpr int PIN_PIEZO    = 14;  // was 26 in the first pin map; band 2 is wired to 14 (pin scan 14 Nov)
constexpr int PIN_LED_G    = 25;  // teacher band (board 0 wiring, 14 Nov)
constexpr int PIN_LED_Y    = 26;
constexpr int PIN_LED_R    = 27;

constexpr uint8_t MMA_ADDR_A = 0x1D;  // SparkFun default (SA0 high)
constexpr uint8_t MMA_ADDR_B = 0x1C;

static void beep(int ms = 120, int hz = 2700) {
  tone(PIN_PIEZO, hz, ms);
  delay(ms + 30);
}

// ---- piezo diagnostics (commands h / t / p) ----
static void piezoDC() {  // active buzzer: beeps on plain DC
  Serial.printf("# h: GPIO%d HIGH for 2 s (active buzzer should beep continuously)\n", PIN_PIEZO);
  noTone(PIN_PIEZO);
  pinMode(PIN_PIEZO, OUTPUT);
  digitalWrite(PIN_PIEZO, HIGH);
  delay(2000);
  digitalWrite(PIN_PIEZO, LOW);
}

static void piezoSweep() {  // passive piezo: very frequency dependent
  Serial.println("# t: sweep 500..5000 Hz, 300 ms each");
  for (int hz = 500; hz <= 5000; hz += 500) {
    Serial.printf("#   %d Hz\n", hz);
    tone(PIN_PIEZO, hz, 300);
    delay(350);
  }
}

// Beeps on every GPIO that is not used by another part, so a piezo on a
// different pin than expected is found. Skips 16/17 (GPS), 21/22 (I2C),
// 25 (GPS EN), 27 (reed), 34 (input only), 0/2/12/15 (strapping) and 1/3 (USB serial).
static void piezoPinScan() {
  const int pins[] = {4, 5, 13, 14, 18, 19, 23, 26, 32, 33};  // 14 = expected piezo pin
  Serial.println("# p: pin scan - listen and note the pin number when you hear a beep");
  for (int p : pins) {
    Serial.printf("#   GPIO%d ...\n", p);
    Serial.flush();
    pinMode(p, OUTPUT);
    for (int i = 0; i < 3; i++) {  // 3 short beeps: tone + DC, works for both buzzer types
      tone(p, 2500, 150);
      delay(200);
      digitalWrite(p, HIGH);
      delay(150);
      digitalWrite(p, LOW);
      delay(100);
    }
    noTone(p);
    pinMode(p, INPUT);
    delay(600);
  }
  pinMode(PIN_PIEZO, OUTPUT);
  Serial.println("# p: done");
}

#ifndef ROLE_TEACHER
// ===================================================================== child band
static uint8_t mmaAddr = 0;
static int gpsRx = -1, gpsTx = -1;
static uint32_t gpsBaud = 0;
static bool lastReed = false;

static bool i2cRead(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)n) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
static bool i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static void scanI2C() {
  Serial.printf("# I2C scan (SDA=%d SCL=%d):", PIN_SDA, PIN_SCL);
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); found++; }
  }
  Serial.println(found ? "" : " nothing found -> check 3V3/GND/SDA/SCL");
}

static void initMMA() {
  for (uint8_t a : {MMA_ADDR_A, MMA_ADDR_B}) {
    uint8_t who = 0;
    if (i2cRead(a, 0x0D, &who, 1)) {  // WHO_AM_I
      Serial.printf("# MMA8452Q at 0x%02X, WHO_AM_I=0x%02X %s\n", a, who,
                    who == 0x2A ? "(OK)" : "(unexpected, expected 0x2A)");
      if (who == 0x2A) {
        mmaAddr = a;
        i2cWrite(a, 0x2A, 0x00);  // CTRL_REG1: standby
        i2cWrite(a, 0x0E, 0x00);  // XYZ_DATA_CFG: +-2 g
        i2cWrite(a, 0x2A, 0x01);  // active, 800 Hz
      }
      return;
    }
  }
  Serial.println("# MMA8452Q NOT found at 0x1D/0x1C");
}

static bool readAccel(float& x, float& y, float& z) {
  uint8_t b[6];
  if (!mmaAddr || !i2cRead(mmaAddr, 0x01, b, 6)) return false;
  auto conv = [](uint8_t msb, uint8_t lsb) {
    int16_t v = (int16_t)((msb << 8) | lsb) >> 4;  // 12-bit, left aligned
    return v / 1024.0f;                            // +-2 g -> 1024 counts/g
  };
  x = conv(b[0], b[1]); y = conv(b[2], b[3]); z = conv(b[4], b[5]);
  return true;
}

// Listens ~1.2 s and counts NMEA-looking lines ("$G..." / "$P..." ending in \n).
static int nmeaScore(int rx, int tx, uint32_t baud) {
  Serial2.end();
  Serial2.begin(baud, SERIAL_8N1, rx, tx);
  uint32_t end = millis() + 1200;
  int score = 0;
  char prev = 0;
  bool sawDollar = false;
  while (millis() < end) {
    while (Serial2.available()) {
      char c = Serial2.read();
      if (c == '$') sawDollar = true;
      if (c == '\n' && prev == '\r' && sawDollar) { score++; sawDollar = false; }
      prev = c;
    }
  }
  return score;
}

static void detectGPS() {
  pinMode(PIN_GPS_EN, OUTPUT);
  digitalWrite(PIN_GPS_EN, HIGH);
  delay(300);
  Serial.println("# GPS: detecting baud rate and RX/TX (takes ~10 s) ...");
  const uint32_t bauds[] = {9600, 38400, 115200, 4800};
  const int pairs[2][2] = {{PIN_GPS_A, PIN_GPS_B}, {PIN_GPS_B, PIN_GPS_A}};
  gpsBaud = 0;
  for (auto& p : pairs) {
    for (uint32_t b : bauds) {
      int s = nmeaScore(p[0], p[1], b);
      if (s >= 2) {
        gpsRx = p[0]; gpsTx = p[1]; gpsBaud = b;
        Serial.printf("# GPS found: %lu baud, ESP RX=GPIO%d TX=GPIO%d (%d sentences/1.2 s)%s\n",
                      b, gpsRx, gpsTx, s,
                      gpsRx == PIN_GPS_A ? "" : "  -> RX/TX are SWAPPED vs. pin map (works, but note it)");
        return;
      }
    }
  }
  Serial2.end();
  Serial.println("# GPS: no NMEA data -> check VIN/GND, GPS TX -> GPIO16, EN pin; some modules need 5V (VIN pin)");
}

// Minimal NMEA parsing: GGA (fix, sats, position), RMC (time).
static struct { int fix = 0, sats = 0; char lat[16] = "", lon[16] = "", utc[12] = ""; uint32_t lines = 0; } gps;

static void parseField(const char* s, int idx, char* out, size_t n) {
  int f = 0;
  for (; *s && f < idx; s++) if (*s == ',') f++;
  size_t i = 0;
  for (; *s && *s != ',' && *s != '*' && i < n - 1; s++) out[i++] = *s;
  out[i] = 0;
}

static void pollGPS() {
  if (!gpsBaud) return;
  static char line[100];
  static size_t len = 0;
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      line[len] = 0;
      len = 0;
      gps.lines++;
      char f[16];
      if (strncmp(line + 3, "GGA", 3) == 0) {
        parseField(line, 6, f, sizeof f); gps.fix = atoi(f);
        parseField(line, 7, f, sizeof f); gps.sats = atoi(f);
        parseField(line, 2, gps.lat, sizeof gps.lat);
        parseField(line, 4, gps.lon, sizeof gps.lon);
        parseField(line, 1, gps.utc, sizeof gps.utc);
      }
    } else if (c != '\r' && len < sizeof line - 1) {
      line[len++] = c;
    }
  }
}

static void printStatus() {
  bool reed = digitalRead(PIN_REED) == LOW;
  float x, y, z;
  Serial.printf("# t=%lus | reed: %s | ", millis() / 1000, reed ? "CLOSED (magnet near)" : "open");
  if (readAccel(x, y, z))
    Serial.printf("accel x=%+.2f y=%+.2f z=%+.2f g |a|=%.2f | ", x, y, z, sqrtf(x * x + y * y + z * z));
  else
    Serial.print("accel n/a | ");
  if (gpsBaud)
    Serial.printf("GPS lines=%lu fix=%d sats=%d utc=%s lat=%s lon=%s%s\n", gps.lines, gps.fix, gps.sats,
                  gps.utc, gps.lat, gps.lon, gps.fix ? "" : "  (no fix yet - needs open sky)");
  else
    Serial.println("GPS n/a");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n# ===== BuddyBand HW test – CHILD BAND =====");
  Serial.println("# commands: b=beep  s=status  g=re-detect GPS  h=piezo DC 2s  t=tone sweep  p=pin scan");

  Serial.printf("# piezo on GPIO%d: 3 beeps now\n", PIN_PIEZO);
  for (int i = 0; i < 3; i++) beep(120, 2000 + i * 700);

  pinMode(PIN_REED, INPUT_PULLUP);
  lastReed = digitalRead(PIN_REED) == LOW;
  Serial.printf("# reed on GPIO%d: currently %s - move the magnet to test\n", PIN_REED,
                lastReed ? "CLOSED" : "open");

  pinMode(PIN_ACC_INT1, INPUT);
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  scanI2C();
  initMMA();

  detectGPS();
  printStatus();
}

void loop() {
  pollGPS();

  bool reed = digitalRead(PIN_REED) == LOW;
  if (reed != lastReed) {
    delay(20);  // debounce
    if ((digitalRead(PIN_REED) == LOW) == reed) {
      lastReed = reed;
      Serial.printf("# REED changed -> %s\n", reed ? "CLOSED (magnet near)" : "OPEN (magnet away) = would be TAMPER");
      beep(60, reed ? 3000 : 1500);
    }
  }

  // movement indicator: report big changes in |a| (shake / tap / drop)
  static float lastMag = 1.0f;
  float x, y, z;
  if (readAccel(x, y, z)) {
    float m = sqrtf(x * x + y * y + z * z);
    if (fabsf(m - lastMag) > 0.6f) Serial.printf("# ACCEL jolt |a|=%.2f g\n", m);
    lastMag = m;
  }

  static uint32_t last = 0;
  if (millis() - last >= 2000) { last = millis(); printStatus(); }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'b') beep(200);
    if (c == 's') printStatus();
    if (c == 'g') detectGPS();
    if (c == 'h') piezoDC();
    if (c == 't') piezoSweep();
    if (c == 'p') piezoPinScan();
  }
  delay(5);
}

#else
// ===================================================================== teacher band
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n# ===== BuddyBand HW test – TEACHER BAND =====");
  Serial.println("# LEDs cycle green -> yellow -> red, short beep each step. commands: b=beep");
  for (int p : {PIN_LED_G, PIN_LED_Y, PIN_LED_R}) pinMode(p, OUTPUT);
}

void loop() {
  static int step = 0;
  const int pins[] = {PIN_LED_G, PIN_LED_Y, PIN_LED_R};
  const char* names[] = {"GREEN (GPIO25)", "YELLOW (GPIO26)", "RED (GPIO27)"};
  for (int i = 0; i < 3; i++) digitalWrite(pins[i], i == step ? HIGH : LOW);
  Serial.printf("# LED %s on\n", names[step]);
  beep(60, 2000 + step * 600);
  step = (step + 1) % 3;
  uint32_t end = millis() + 1000;
  while (millis() < end) {
    if (Serial.available() && Serial.read() == 'b') beep(200);
    delay(5);
  }
}
#endif
