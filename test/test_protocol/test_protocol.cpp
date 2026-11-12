// Host test for protocol.h: g++ -std=c++17 -I../../lib/protocol test_protocol.cpp && ./a.out
#include <cassert>
#include <cstdio>
#include "protocol.h"

int main() {
  // STATUS round trip
  bb::Packet p;
  bb::StatusPayload s{bb::STAGE_DRIFTING, 2, -67, 80};
  assert(bb::buildPacket(p, 1, bb::MSG_STATUS, 3, 42, s));
  assert(p.len == 12);

  bb::Packet q;
  assert(bb::parsePacket(p.data, p.len, 1, q));
  assert(q.header().src == 3 && q.header().tx == 3 && q.header().seq == 42);
  assert(q.header().ttl == bb::TTL_MAX && bb::hops(q.header()) == 0);
  assert(q.payload<bb::StatusPayload>().best_rssi == -67);

  // other group is rejected, wrong length is rejected
  assert(!bb::parsePacket(p.data, p.len, 2, q));
  assert(!bb::parsePacket(p.data, p.len - 1, 1, q));

  // GPS is the largest packet and must fit into one advertisement
  bb::GpsPayload g{470432000, 83097000, 3};
  assert(bb::buildPacket(p, 1, bb::MSG_GPS, 2, 7, g));
  assert(p.len == 17 && p.len <= bb::MAX_PACKET);

  // relay: decrement ttl, set own tx
  q = p; q.header().ttl--; q.header().tx = 1;
  assert(bb::hops(q.header()) == 1 && q.header().src == 2);

  puts("protocol.h: all tests passed");
}
