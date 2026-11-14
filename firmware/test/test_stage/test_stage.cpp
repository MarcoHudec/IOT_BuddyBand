// Host test for stage.h (+ replay of recorded RSSI traces).
//
//   g++ -std=c++17 -Wall -I../../lib/neighbours -I../../lib/protocol -I../../lib/stage test_stage.cpp -o test_stage
//   ./test_stage                                   # unit tests
//   ./test_stage ../../../evaluation/rssi/indoor_5m.csv [t_near] [t_far]   # replay a log_rssi.py CSV
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "stage.h"

using namespace bb;

// Small simulator: one neighbour sending at 10 packets/s, stage machine ticked every 100 ms.
struct Sim {
  NeighbourTable table;
  StageMachine sm;
  uint32_t now = 0;
  uint32_t last_rx = 0;
  int changes = 0;

  // rssi == RSSI_NONE -> neighbour silent
  void run(int8_t rssi, uint32_t duration_ms) {
    for (uint32_t end = now + duration_ms; now < end; now += 100) {
      if (rssi != RSSI_NONE) { table.add(1, rssi, now); last_rx = now; }
      table.expire(now, 8000);
      if (sm.update(now, table, last_rx)) changes++;
    }
  }
};

static void unit_tests() {
  // starts complete, stays complete with a strong neighbour
  { Sim s; s.run(-55, 10000); assert(s.sm.stage() == STAGE_COMPLETE && s.changes == 0); }

  // drifting needs the condition for 5 s
  {
    Sim s; s.run(-55, 2000);
    s.run(-78, 4500);                       // below T_near(-70), above T_far(-85)
    assert(s.sm.stage() == STAGE_COMPLETE); // median needs ~0.5 s to follow + 5 s dwell
    s.run(-78, 1500);
    assert(s.sm.stage() == STAGE_DRIFTING && s.sm.previous() == STAGE_COMPLETE);
  }

  // a short dip (body shadowing, 3 s) never causes an alarm
  { Sim s; s.run(-55, 2000); s.run(-90, 3000); s.run(-55, 5000); assert(s.changes == 0); }

  // critical: only very weak signal for > 5 s
  { Sim s; s.run(-60, 2000); s.run(-92, 7000); assert(s.sm.stage() == STAGE_CRITICAL); }

  // isolated: silence for 8 s; first packet afterwards -> critical
  {
    Sim s; s.run(-60, 2000);
    s.run(RSSI_NONE, 7900);
    assert(s.sm.stage() != STAGE_ISOLATED);
    s.run(RSSI_NONE, 200);
    assert(s.sm.stage() == STAGE_ISOLATED);
    s.run(-60, 600);                        // >= 5 fresh samples needed
    assert(s.sm.stage() == STAGE_CRITICAL && s.sm.previous() == STAGE_ISOLATED);
    s.run(-60, 4000);                       // strong again for > 3 s -> complete
    assert(s.sm.stage() == STAGE_COMPLETE);
  }

  // hysteresis: in drifting, -69 dBm (above T_near but within the 3 dB margin) is not enough
  {
    Sim s; s.run(-60, 1000); s.run(-78, 7000);
    assert(s.sm.stage() == STAGE_DRIFTING);
    s.run(-69, 10000);
    assert(s.sm.stage() == STAGE_DRIFTING);
    s.run(-66, 4000);                       // -66 >= -70 + 3 for > 3 s
    assert(s.sm.stage() == STAGE_COMPLETE);
  }

  // regression (field test 14 Nov, t=1966..1976 s): after isolation, two stray packets
  // (power flicker while unplugging) must neither leave isolation nor revive the old median
  {
    Sim s; s.run(-40, 3000);
    s.run(RSSI_NONE, 8500);
    assert(s.sm.stage() == STAGE_ISOLATED);
    s.run(-40, 100);                        // stray packet 1
    s.run(RSSI_NONE, 1500);
    s.run(-40, 100);                        // stray packet 2
    s.run(RSSI_NONE, 5000);
    assert(s.sm.stage() == STAGE_ISOLATED && s.changes == 1);
  }

  // band boots alone: isolated after 8 s
  { Sim s; s.run(RSSI_NONE, 8100); assert(s.sm.stage() == STAGE_ISOLATED); }

  puts("stage.h: all tests passed");
}

// Replays a CSV from evaluation/rssi (ms,self,tx,seq,rssi) and prints every transition.
static int replay(const char* path, int t_near, int t_far) {
  FILE* f = fopen(path, "r");
  if (!f) { perror(path); return 1; }
  StageConfig cfg;
  cfg.t_near_dbm = static_cast<int8_t>(t_near);
  cfg.t_far_dbm = static_cast<int8_t>(t_far);
  NeighbourTable table;
  StageMachine sm(cfg);
  char line[128];
  unsigned long ms, self, tx, seq;
  long rssi;
  uint32_t t0 = 0, next_tick = 0, last_rx = 0;
  bool first = true;
  printf("replay %s  T_near=%d T_far=%d\n", path, t_near, t_far);
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "%lu,%lu,%lu,%lu,%ld", &ms, &self, &tx, &seq, &rssi) != 5) continue;
    uint32_t now = static_cast<uint32_t>(ms);
    if (first) { t0 = next_tick = last_rx = now; first = false; }
    while (next_tick <= now) {  // tick every 250 ms like the firmware
      table.expire(next_tick, 8000);
      if (sm.update(next_tick, table, last_rx))
        printf("  t=%6.1fs  %s -> %s   (best median %d dBm)\n", (next_tick - t0) / 1000.0,
               stageName(sm.previous()), stageName(sm.stage()),
               table.bestMedian(next_tick, cfg.heard_window_ms));
      next_tick += 250;
    }
    table.add(static_cast<uint8_t>(tx), static_cast<int8_t>(rssi), now);
    last_rx = now;
  }
  fclose(f);
  printf("  end: %s after %.1f s\n", stageName(sm.stage()), (last_rx - t0) / 1000.0);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 2)
    return replay(argv[1], argc >= 3 ? atoi(argv[2]) : -70, argc >= 4 ? atoi(argv[3]) : -85);
  unit_tests();
  return 0;
}
