// Host test for neighbours.h:
//   g++ -std=c++17 -Wall -I../../lib/neighbours test_neighbours.cpp && ./a.out
#include <cassert>
#include <cstdio>
#include "neighbours.h"

int main() {
  using namespace bb;

  // median: odd, even, and robust against a single outlier (protocol.md §9 finding 2)
  Neighbour n;
  assert(n.median() == RSSI_NONE);
  n.add(-60, 0); n.add(-62, 0); n.add(-58, 0);
  assert(n.median() == -60);
  n.add(-64, 0);
  assert(n.median() == -61);  // (-62 + -60) / 2
  for (int i = 0; i < 9; i++) n.add(-50, 0);
  n.add(-100, 0);             // dropout: window now 9 x -50 + 1 x -100
  assert(n.count == RSSI_WINDOW && n.median() == -50);

  // ring buffer only keeps the last 10 values
  for (int i = 0; i < 10; i++) n.add(-70, 0);
  assert(n.median() == -70 && n.total == 24);

  // table: add, active window, best median, expire
  NeighbourTable t;
  t.add(2, -55, 1000);
  t.add(3, -75, 1000);
  t.add(2, -57, 1100);
  assert(t.countActive(1100, 4000) == 2);
  assert(t.bestMedian(1100, 4000) == -56);
  assert(t.find(3)->median() == -75);

  assert(t.countActive(5050, 4000) == 1);     // node 3 age 4050 -> inactive, node 2 age 3950 -> active
  assert(t.bestMedian(5050, 4000) == -56);
  t.expire(9050, 8000);                        // node 3 (age 8050) removed, node 2 (age 7950) kept
  assert(!t.find(3) && t.find(2));
  assert(t.countActive(20000, 4000) == 0 && t.bestMedian(20000, 4000) == RSSI_NONE);

  // gap reset: after > GAP_RESET_MS silence old samples are discarded
  Neighbour g;
  for (int i = 0; i < 10; i++) g.add(-40, 100 * i);
  g.add(-80, 900 + GAP_RESET_MS + 1);
  assert(g.count == 1 && g.median() == -80);

  // min_samples: a neighbour with too few samples is ignored by bestMedian
  NeighbourTable m;
  m.add(7, -45, 0); m.add(7, -45, 100);
  assert(m.bestMedian(200, 4000, 5) == RSSI_NONE && m.bestMedian(200, 4000) == -45);

  // millis() wrap-around
  NeighbourTable w;
  w.add(5, -60, 0xFFFFFF00u);
  assert(w.countActive(0x00000100u, 4000) == 1);  // 512 ms later across the wrap

  // full table: least recently seen entry is replaced
  NeighbourTable f;
  for (uint8_t id = 1; id <= MAX_NEIGHBOURS; id++) f.add(id, -60, 100 + id);
  f.add(99, -40, 1000);
  assert(f.find(99) && !f.find(1) && f.find(2));

  // strongest(): sorted by median, limited to max, inactive ones skipped
  NeighbourTable s;
  s.add(1, -80, 0); s.add(2, -50, 0); s.add(0, -65, 0); s.add(3, -70, 0); s.add(4, -90, 0);
  s.add(9, -30, 0);
  s.add(1, -80, 6000); s.add(2, -50, 6000); s.add(0, -65, 6000); s.add(3, -70, 6000); s.add(4, -90, 6000);
  uint8_t ids[4]; int8_t rs[4];
  uint8_t cnt = s.strongest(6000, 4000, ids, rs, 4);   // 9 is inactive
  assert(cnt == 4 && ids[0] == 2 && ids[1] == 0 && ids[2] == 3 && ids[3] == 1);
  assert(rs[0] == -50 && rs[3] == -80);
  assert(s.strongest(20000, 4000, ids, rs, 4) == 0);

  puts("neighbours.h: all tests passed");
}
