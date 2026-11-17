// Minimal NMEA parser for the child band GPS (WP4). Header-only, Arduino-free,
// host-tested in firmware/test/test_nmea.
//
// Only GGA is used ($GPGGA / $GNGGA): fix quality, satellites, position.
// Sentences with a wrong checksum are counted and ignored.
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace bb {

struct GpsFix {
  bool     valid = false;   // a fix has been seen at least once
  int32_t  lat_e7 = 0;      // degrees * 1e7 (south negative)
  int32_t  lon_e7 = 0;      // degrees * 1e7 (west negative)
  uint8_t  sats = 0;        // satellites in use (last GGA, also without fix)
  uint8_t  quality = 0;     // GGA fix quality of the last sentence (0 = none)
  uint32_t at_ms = 0;       // time of the last valid fix
};

class NmeaParser {
 public:
  // Feed one character. Returns true when a GGA with a valid fix was parsed.
  bool feed(char c, uint32_t now) {
    if (c == '$') { len_ = 0; }
    if (c == '\r') return false;
    if (c == '\n') {
      line_[len_] = 0;
      bool r = len_ > 6 && handle(now);
      len_ = 0;
      return r;
    }
    if (len_ < sizeof line_ - 1) line_[len_++] = c;
    return false;
  }

  const GpsFix& fix() const { return fix_; }
  uint32_t sentences() const { return sentences_; }
  uint32_t checksumErrors() const { return bad_; }

  // "ddmm.mmmm" / "dddmm.mmmm" + hemisphere -> degrees * 1e7
  static int32_t toE7(const char* v, char hemi) {
    if (!*v) return 0;
    double x = strtod(v, nullptr);
    int deg = static_cast<int>(x / 100);
    double minutes = x - deg * 100.0;
    double d = deg + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') d = -d;
    return static_cast<int32_t>(d * 1e7 + (d >= 0 ? 0.5 : -0.5));
  }

 private:
  static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }

  bool checksumOk() const {
    const char* star = strchr(line_, '*');
    if (line_[0] != '$' || !star || !star[1] || !star[2]) return false;
    uint8_t cs = 0;
    for (const char* p = line_ + 1; p < star; p++) cs ^= static_cast<uint8_t>(*p);
    int hi = hex(star[1]), lo = hex(star[2]);
    return hi >= 0 && lo >= 0 && cs == ((hi << 4) | lo);
  }

  // Copies field `idx` (0 = sentence id) into out.
  void field(int idx, char* out, size_t n) const {
    const char* s = line_ + 1;
    for (int f = 0; *s && f < idx; s++) if (*s == ',') f++;
    size_t i = 0;
    while (*s && *s != ',' && *s != '*' && i < n - 1) out[i++] = *s++;
    out[i] = 0;
  }

  bool handle(uint32_t now) {
    if (!checksumOk()) { bad_++; return false; }
    sentences_++;
    if (strncmp(line_ + 3, "GGA", 3) != 0) return false;
    char q[4], sats[4], lat[16], ns[2], lon[16], ew[2];
    field(6, q, sizeof q);
    field(7, sats, sizeof sats);
    fix_.quality = static_cast<uint8_t>(atoi(q));
    fix_.sats = static_cast<uint8_t>(atoi(sats));
    if (fix_.quality == 0) return false;
    field(2, lat, sizeof lat); field(3, ns, sizeof ns);
    field(4, lon, sizeof lon); field(5, ew, sizeof ew);
    if (!lat[0] || !lon[0]) return false;
    fix_.lat_e7 = toE7(lat, ns[0]);
    fix_.lon_e7 = toE7(lon, ew[0]);
    fix_.valid = true;
    fix_.at_ms = now;
    return true;
  }

  char     line_[100] = {};
  size_t   len_ = 0;
  GpsFix   fix_;
  uint32_t sentences_ = 0, bad_ = 0;
};

}  // namespace bb
