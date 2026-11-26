#pragma once

#include "spool.h"

#include <cstddef>
#include <memory>

namespace myduck::cdc
{

struct Checkpoint
{
  uint64_t sequence = 0;
  std::string file;
  uint64_t position = 4;
};

struct ApplyTimings
{
  uint64_t injection_us = 0;
  uint64_t checkpoint_us = 0;
  uint64_t commit_us = 0;
  uint64_t ddl_us = 0;
};

class Applier
{
public:
  Applier(const std::string &database, const std::string &stream,
          const std::string &identity, const std::string &initial_file,
          uint64_t initial_position);
  ~Applier();
  Checkpoint Current();
  ApplyTimings Timings() const;
  size_t Apply(const std::vector<SpoolRecord> &records);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
