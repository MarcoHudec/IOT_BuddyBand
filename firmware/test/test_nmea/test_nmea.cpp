// Host test for nmea.h:
//   g++ -std=c++17 -Wall -I../../lib/gps test_nmea.cpp && ./a.out
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "nmea.h"

using namespace bb;

static bool feedLine(NmeaParser& p, const char* s, uint32_t now) {
  bool r = false;
  for (const char* c = s; *c; c++) r |= p.feed(*c, now);
  r |= p.feed('\r', now);
  r |= p.feed('\n', now);
  return r;
}

int main() {
  NmeaParser p;

  // no fix yet (what the board printed indoors on 14 Nov)
  assert(!feedLine(p, "$GPGGA,000542.800,,,,,0,00,,,M,,M,,*73", 100));
  assert(!p.fix().valid && p.fix().quality == 0 && p.sentences() == 1);

  // valid fix near HSLU Rotkreuz: 47 deg 08.6112' N, 8 deg 25.8876' E
  assert(feedLine(p, "$GPGGA,123519.000,4708.6112,N,00825.8876,E,1,07,1.2,430.0,M,48.0,M,,*5F", 2000));
  const GpsFix& f = p.fix();
  assert(f.valid && f.sats == 7 && f.at_ms == 2000);
  assert(std::abs(f.lat_e7 - 471435200) <= 1);   // 47 + 8.6112/60
  assert(std::abs(f.lon_e7 - 84314600) <= 1);    // 8 + 25.8876/60

  // GN talker id (multi-GNSS modules) is accepted, other sentences ignored
  assert(!feedLine(p, "$GPRMC,123519.000,A,4708.6112,N,00825.8876,E,0.0,0.0,141126,,,A*63", 2100));

  // wrong checksum is rejected and does not change the fix
  assert(!feedLine(p, "$GPGGA,123520.000,0000.0000,N,00000.0000,E,1,07,1.2,430.0,M,48.0,M,,*00", 3000));
  assert(p.checksumErrors() == 1 && p.fix().lat_e7 != 0);

  // southern / western hemisphere
  assert(NmeaParser::toE7("3351.0000", 'S') == -338500000);
  assert(NmeaParser::toE7("15112.0000", 'W') == -1512000000);

  // garbage and partial lines do not crash
  feedLine(p, "GPGGA,,,", 4000);
  feedLine(p, "$", 4000);
  feedLine(p, "$GPGGA,1,2,3*ZZ", 4000);

  puts("nmea.h: all tests passed");
}
