#pragma once

#include "events.h"

#include <cstdint>
#include <deque>
#include <string>

namespace myduck::cdc
{

struct EventTimes
{
  uint64_t oldest= 0;
  uint64_t commit= 0;
};

inline EventTimes PayloadTimes(const std::string &payload)
{
  EventTimes result;
  for (const auto &event : Events(payload))
  {
    unsigned type= static_cast<unsigned char>(event[4]);
    if (type == 15 || type == 4)
      continue;
    uint64_t timestamp= EventLength(event.data());
    if (timestamp && (!result.oldest || timestamp < result.oldest))
      result.oldest= timestamp;
    if (type == 16 || type == 2)
      result.commit= timestamp;
  }
  return result;
}

inline std::string PendingAge(uint64_t records, uint64_t oldest, uint64_t now)
{
  if (!records)
    return "0";
  if (!oldest)
    return "UNKNOWN";
  return std::to_string(now > oldest ? now - oldest : 0);
}

inline std::string ApplyLag(bool connected, uint64_t records, uint64_t oldest, uint64_t now)
{
  return connected ? PendingAge(records, oldest, now) : "UNKNOWN";
}

class PendingMetrics
{
  struct Entry
  {
    uint64_t sequence;
    uint64_t bytes;
    EventTimes times;
  };
  std::deque<Entry> entries;
  std::deque<Entry> oldest;
  uint64_t bytes= 0;

public:
  void Add(uint64_t sequence, uint64_t size, EventTimes times)
  {
    entries.push_back({sequence, size, times});
    bytes+= size;
    while (!oldest.empty() && times.oldest && oldest.back().times.oldest >= times.oldest)
      oldest.pop_back();
    if (times.oldest)
      oldest.push_back({sequence, size, times});
  }

  uint64_t AppliedThrough(uint64_t sequence)
  {
    uint64_t commit= 0;
    while (!entries.empty() && entries.front().sequence <= sequence)
    {
      bytes-= entries.front().bytes;
      if (entries.front().times.commit)
        commit= entries.front().times.commit;
      entries.pop_front();
    }
    while (!oldest.empty() && oldest.front().sequence <= sequence)
      oldest.pop_front();
    return commit;
  }

  uint64_t Records() const { return entries.size(); }
  uint64_t Bytes() const { return bytes; }
  uint64_t Oldest() const { return oldest.empty() ? 0 : oldest.front().times.oldest; }
};

}
