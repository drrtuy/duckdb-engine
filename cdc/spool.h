#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace myduck::cdc
{

constexpr uint64_t SPOOL_RECORD_HEADER_BYTES = 48;

struct SpoolRecord
{
  uint64_t sequence = 0;
  std::string source_file;
  uint64_t source_position = 0;
  std::string payload;
};

class Spool
{
public:
  Spool(const std::string &directory, const std::string &identity,
        uint64_t next_sequence, uint64_t max_bytes);
  ~Spool();
  uint64_t Append(const std::string &file, uint64_t pos, const std::string &payload);
  uint64_t AppendBatch(const std::vector<SpoolRecord> &records);
  std::vector<SpoolRecord> ReadAfter(uint64_t sequence, uint64_t max_bytes);
  void DiscardThrough(uint64_t sequence);
  uint64_t Bytes() const;
  uint64_t LastSequence() const;
  SpoolRecord Last() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
