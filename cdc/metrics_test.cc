#include "metrics.h"

#include <stdexcept>

using namespace myduck::cdc;

static void Need(bool ok)
{
  if (!ok)
    throw std::runtime_error("CDC metrics test failed");
}

static std::string Event(unsigned type, uint32_t timestamp)
{
  std::string event(19, 0);
  event[4]= type;
  event[9]= 19;
  for (unsigned i= 0; i < 4; ++i)
    event[i]= (timestamp >> (8 * i)) & 255;
  return event;
}

int main()
{
  auto format= Event(15, 4000000000U);
  auto rotate= Event(4, 4000000001U);
  Need(PayloadTimes(format).oldest == 0);
  Need(PayloadTimes(format + rotate).commit == 0);
  auto payload= format + Event(162, 100) + Event(2, 101) + Event(23, 102) + Event(16, 103);
  auto times= PayloadTimes(payload);
  Need(times.oldest == 100 && times.commit == 103);
  auto ddl= PayloadTimes(format + Event(162, 110) + Event(2, 111));
  Need(ddl.oldest == 110 && ddl.commit == 111);
  Need(PayloadTimes(format + Event(23, 120)).commit == 0);
  Need(PayloadTimes(format + Event(16, 0)).oldest == 0);
  Need(PayloadTimes(format + Event(16, 0)).commit == 0);
  auto backwards= PayloadTimes(format + Event(162, 500) + Event(16, 400));
  Need(backwards.oldest == 400 && backwards.commit == 400);
  Need(PayloadTimes(Event(16, 0xabcdef12)).commit == 0xabcdef12);
  bool rejected= false;
  try { PayloadTimes(std::string(18, 0)); }
  catch (const std::runtime_error &) { rejected= true; }
  Need(rejected);
  rejected= false;
  try { auto bad= Event(16, 1); bad[9]= 20; PayloadTimes(bad); }
  catch (const std::runtime_error &) { rejected= true; }
  Need(rejected);

  Need(PendingAge(0, 100, 500) == "0");
  Need(PendingAge(1, 100, 500) == "400");
  Need(PendingAge(1, 0, 500) == "UNKNOWN");
  Need(PendingAge(1, 600, 500) == "0");
  Need(PendingAge(1, 100, 0) == "0");
  Need(ApplyLag(false, 0, 0, 500) == "UNKNOWN");
  Need(ApplyLag(false, 1, 100, 500) == "UNKNOWN");
  Need(ApplyLag(true, 0, 0, 500) == "0");
  Need(ApplyLag(true, 1, 0, 500) == "UNKNOWN");
  Need(ApplyLag(true, 1, 100, 500) == "400");

  PendingMetrics pending;
  pending.Add(41, format.size(), PayloadTimes(format));
  pending.Add(42, payload.size(), times);
  pending.Add(43, 77, ddl);
  Need(pending.Records() == 3);
  Need(pending.Bytes() == format.size() + payload.size() + 77);
  Need(pending.Oldest() == 100);
  Need(pending.AppliedThrough(41) == 0);
  Need(pending.Records() == 2 && pending.Oldest() == 100);
  Need(pending.AppliedThrough(42) == 103);
  Need(pending.Records() == 1 && pending.Bytes() == 77 && pending.Oldest() == 110);
  pending.Add(44, 99, {90, 95});
  Need(pending.Oldest() == 90);
  Need(pending.AppliedThrough(43) == 111);
  Need(pending.Oldest() == 90 && pending.Bytes() == 99);
  pending.Add(45, format.size(), {});
  Need(pending.AppliedThrough(45) == 95);
  Need(pending.Records() == 0 && pending.Bytes() == 0 && pending.Oldest() == 0);
  Need(pending.AppliedThrough(45) == 0);
  pending.Add(46, 1, {80, 81});
  pending.Add(47, 2, {80, 82});
  Need(pending.AppliedThrough(46) == 81 && pending.Oldest() == 80);
  Need(pending.AppliedThrough(47) == 82 && pending.Oldest() == 0);
  for (uint64_t i= 48; i < 10048; ++i)
    pending.Add(i, 3, {i, i});
  Need(pending.AppliedThrough(10046) == 10046);
  Need(pending.Oldest() == 10047 && pending.Bytes() == 3);
  Need(pending.AppliedThrough(10047) == 10047);
  Need(PendingAge(pending.Records(), pending.Oldest(), 20000) == "0");
  return 0;
}
