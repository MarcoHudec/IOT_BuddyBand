// Host test for mesh.h:
//   g++ -std=c++17 -Wall -I../../lib/protocol -I../../lib/mesh test_mesh.cpp && ./a.out
#include <cassert>
#include <cstdio>
#include "alarm.h"
#include "mesh.h"

using namespace bb;

static Packet status(uint8_t src, uint16_t seq, uint8_t stage = 0) {
  Packet p;
  buildPacket(p, 1, MSG_STATUS, src, seq, StatusPayload{stage, 2, -60, 255});
  return p;
}

int main() {
  // --- duplicate cache
  DupCache d;
  assert(d.checkAndAdd(3, 100));
  assert(!d.checkAndAdd(3, 100));            // same message again (other relay / adv event)
  assert(d.checkAndAdd(3, 101) && d.checkAndAdd(2, 100));
  for (uint16_t s = 0; s < DupCache::SIZE; s++) d.checkAndAdd(9, s);
  assert(d.checkAndAdd(3, 100));             // pushed out after 32 newer entries

  // --- relay rules
  Packet s3 = status(3, 7);
  assert(shouldRelay(s3, 1, false));         // band relays other band's STATUS
  assert(!shouldRelay(s3, 3, false));        // never own packet
  assert(!shouldRelay(s3, 0, true));         // teacher never relays
  Packet r = relayCopy(s3, 1);
  assert(r.header().ttl == TTL_MAX - 1 && r.header().tx == 1 && r.header().src == 3 && hops(r.header()) == 1);
  r = relayCopy(r, 2);                       // second hop: ttl now 1
  assert(r.header().ttl == 1 && !shouldRelay(r, 2, false));

  Packet ack;
  buildPacket(ack, 1, MSG_ACK, 0, 5, AckPayload{3, 7});
  assert(shouldRelay(ack, 1, false) && !shouldRelay(ack, 3, false));  // addressed to 3

  // --- scheduler: background, slot length, jitter, FIFO, drops
  TxScheduler tx(300);
  assert(tx.tick(0) == nullptr);             // nothing yet
  tx.setBackground(status(1, 1));
  const Packet* p = tx.tick(0);
  assert(p && p->header().seq == 1);         // background goes on air
  assert(tx.tick(50) == nullptr);            // unchanged

  tx.enqueue(status(3, 9), 20);              // relay with 20 ms jitter
  assert(tx.tick(10) == nullptr);            // not ready yet
  p = tx.tick(20);
  assert(p && p->header().src == 3);         // relay slot starts
  tx.enqueue(status(2, 4), 30);
  assert(tx.tick(200) == nullptr);           // slot (300 ms) still running
  p = tx.tick(320);
  assert(p && p->header().src == 2);         // next queued packet
  p = tx.tick(620);
  assert(p && p->header().src == 1);         // queue empty -> background again
  assert(tx.tick(700) == nullptr && tx.sent() == 2);

  for (int i = 0; i < 8; i++) assert(tx.enqueue(status(4, i), 1000));
  assert(!tx.enqueue(status(4, 99), 1000) && tx.drops() == 1);
  p = tx.tick(1000);
  assert(p && p->header().seq == 0);         // FIFO for equal ready time
  assert(tx.pending() == 7);

  // --- origin table
  OriginTable o;
  o.update(status(3, 50, STAGE_DRIFTING), 1000);
  Packet relayed = relayCopy(status(3, 51, STAGE_CRITICAL), 2);
  o.update(relayed, 3000);
  const Origin* e = o.find(3);
  assert(e && e->last_seq == 51 && e->hops == 1 && e->stage == STAGE_CRITICAL && e->messages == 2);
  assert(!o.find(2));

  // --- alarm sender: fast retries, latch, ACK matching, supersede
  AlarmSender a(1000, 6, 5000);
  assert(!a.due(0));
  a.raise(AlarmPayload{STAGE_DRIFTING, STAGE_COMPLETE, REASON_NEIGHBOUR_LOSS}, 100);
  assert(a.due(100));
  a.sent(500, 100);                          // first transmission, seq 500
  assert(!a.due(900) && a.due(1100));
  a.sent(501, 1100);
  assert(!a.onAck(499, 1200));               // not ours
  assert(a.onAck(500, 1300));                // ACK of the first copy also counts
  assert(!a.pending() && a.lastRtt() == 1200 && a.lastTries() == 2);
  assert(!a.onAck(501, 1400));               // late ACK of the second copy: ignored

  AlarmSender b(1000, 6, 5000);
  b.raise(AlarmPayload{STAGE_ISOLATED, STAGE_CRITICAL, REASON_NEIGHBOUR_LOSS}, 0);
  uint32_t t = 0;
  uint16_t sq = 10;
  for (int i = 0; i < 6; i++) { assert(b.due(t)); b.sent(sq++, t); t += 1000; }
  assert(b.latched() && !b.due(t) && !b.due(5000 + 4999));  // last tx at 5000
  assert(b.due(10000));                      // latched: every 5 s
  b.sent(sq++, 10000);
  assert(b.onAck(15, 12000) && b.lastTries() == 7);

  AlarmSender c;
  c.raise(AlarmPayload{STAGE_DRIFTING, STAGE_COMPLETE, 1}, 0);
  c.sent(100, 0);
  c.raise(AlarmPayload{STAGE_CRITICAL, STAGE_DRIFTING, 1}, 500);  // newer state
  assert(c.superseded() == 1 && c.due(500) && c.payload().stage == STAGE_CRITICAL);
  c.sent(101, 500);
  assert(!c.onAck(100, 600));                // ACK for the superseded alarm
  assert(c.onAck(101, 700));
  // seq wrap-around
  AlarmSender w;
  w.raise(AlarmPayload{1, 0, 1}, 0);
  w.sent(65535, 0); w.sent(0, 1000);
  assert(w.onAck(0, 1100));

  puts("mesh.h + alarm.h: all tests passed");
}
