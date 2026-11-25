#include "spool.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using myduck::cdc::Spool;
using myduck::cdc::SpoolRecord;

namespace
{
constexpr uint64_t LIMIT = 512ULL << 20;
using namespace std::chrono_literals;

void Check(bool value)
{
  if (!value) throw std::runtime_error("group test assertion failed");
}

template <class F> void Fails(F operation)
{
  bool failed = false;
  try { operation(); }
  catch (const std::runtime_error &) { failed = true; }
  Check(failed);
}

struct Hooks
{
  std::mutex mutex;
  std::condition_variable cv;
  unsigned data = 0, directories = 0, fail = 0, block = 0, crash = 0;
  bool entered = false, released = false, fail_directory = false;
  std::vector<uint64_t> sizes;

  void Reset(unsigned fail_at = 0, unsigned block_at = 0)
  {
    std::lock_guard<std::mutex> guard(mutex);
    data = directories = crash = 0;
    fail = fail_at; block = block_at;
    entered = released = fail_directory = false;
    sizes.clear();
  }
  bool Wait()
  {
    std::unique_lock<std::mutex> guard(mutex);
    return cv.wait_for(guard, 5s, [&] { return entered; });
  }
  void Release()
  {
    std::lock_guard<std::mutex> guard(mutex);
    released = true;
    cv.notify_all();
  }
} hooks;

struct Temp
{
  std::string path;
  Temp()
  {
    char name[] = "/tmp/myduck-spool-group-test-XXXXXX";
    char *created = mkdtemp(name);
    Check(created != nullptr);
    path = created;
  }
  std::vector<std::string> Segments() const
  {
    DIR *dir = opendir(path.c_str());
    Check(dir != nullptr);
    std::vector<std::string> files;
    while (auto *entry = readdir(dir))
      if (std::string(entry->d_name).compare(0, 6, "spool-") == 0)
        files.push_back(path + "/" + entry->d_name);
    closedir(dir);
    std::sort(files.begin(), files.end());
    return files;
  }
  uint64_t Bytes() const
  {
    uint64_t bytes = 0;
    for (const auto &file : Segments())
    {
      struct stat st{};
      Check(stat(file.c_str(), &st) == 0);
      bytes += st.st_size;
    }
    return bytes;
  }
  ~Temp()
  {
    DIR *dir = opendir(path.c_str());
    if (!dir) return;
    while (auto *entry = readdir(dir))
      if (entry->d_name[0] != '.') unlink((path + "/" + entry->d_name).c_str());
    closedir(dir);
    rmdir(path.c_str());
  }
};

std::vector<SpoolRecord> Small()
{
  return {{999, "bin.001", 17, std::string("a\0b", 3)},
          {0, "bin.002", 29, "transaction"}, {42, "bin.002", 35, "last"}};
}

void Basic()
{
  Temp temp;
  uint64_t bytes;
  {
    Spool spool(temp.path, "source", 7, LIMIT);
    hooks.Reset();
    Check(spool.AppendBatch({}) == 6 && hooks.data == 0 && hooks.directories == 0);
    Check(temp.Segments().empty());
    auto input = Small();
    Check(spool.AppendBatch(input) == 9);
    Check(input.front().sequence == 999 && hooks.data == 1 && hooks.directories == 1);
    auto records = spool.ReadAfter(0, LIMIT);
    Check(records.size() == 3);
    for (size_t i = 0; i < records.size(); ++i)
    {
      Check(records[i].sequence == i + 7 && records[i].source_file == input[i].source_file &&
            records[i].source_position == input[i].source_position && records[i].payload == input[i].payload);
    }
    Check(spool.Last().sequence == 9 && spool.Last().source_position == 35);
    hooks.Reset();
    Check(spool.Append("bin.003", 50, "single") == 10 && hooks.data == 1);
    bytes = spool.Bytes();
    hooks.Reset();
    Check(spool.AppendBatch({}) == 10 && hooks.data == 0 && hooks.directories == 0);
    Check(temp.Bytes() == bytes);
  }
  hooks.Reset();
  Spool spool(temp.path, "source", 1, LIMIT);
  Check(hooks.data == 1 && hooks.directories == 1);
  Check(spool.LastSequence() == 10 && spool.Bytes() == bytes && spool.Last().payload == "single");
}

void Validation()
{
  Temp temp;
  Spool spool(temp.path, "source", 1, 150);
  spool.Append("f", 1, "x");
  uint64_t bytes = spool.Bytes();
  hooks.Reset();
  Fails([&] { spool.AppendBatch({{0, "f", 2, "x"}, {0, "f", 3, "xx"}}); });
  Check(hooks.data == 0 && hooks.directories == 0 && temp.Bytes() == bytes);
  Check(spool.LastSequence() == 1 && spool.Bytes() == bytes);
  Check(spool.Append("f", 2, "x") == 2);

  Temp oversized;
  Spool large(oversized.path, "source", 1, LIMIT);
  auto input = Small();
  input.back().payload.assign(256ULL << 20, 'x');
  hooks.Reset();
  Fails([&] { large.AppendBatch(input); });
  Check(oversized.Segments().empty() && hooks.data == 0 && hooks.directories == 0);
  Check(large.LastSequence() == 0 && large.Bytes() == 0);
  input.clear();
  Check(large.Append("f", 1, "valid") == 1);

  Temp exhausted;
  Spool end(exhausted.path, "source", UINT64_MAX, LIMIT);
  hooks.Reset();
  Fails([&] { end.AppendBatch(Small()); });
  Check(exhausted.Segments().empty() && hooks.data == 0 && hooks.directories == 0);
  Check(end.LastSequence() == UINT64_MAX - 1);
  Check(end.Append("f", 1, "last") == UINT64_MAX);
  hooks.Reset();
  Check(end.AppendBatch({}) == UINT64_MAX);
  Fails([&] { end.Append("f", 2, "overflow"); });
  Check(hooks.data == 0 && hooks.directories == 0 && end.LastSequence() == UINT64_MAX);
}

void Segments()
{
  Temp temp;
  std::vector<SpoolRecord> input(3);
  for (size_t i = 0; i < input.size(); ++i)
    input[i] = {0, "f", i + 1, std::string(9ULL << 20, static_cast<char>('a' + i))};
  uint64_t bytes;
  {
    Spool spool(temp.path, "source", 1, LIMIT);
    spool.Append("f", 0, "prefix");
    hooks.Reset();
    Check(spool.AppendBatch(input) == 4);
    Check(temp.Segments().size() == 3 && hooks.data == 3 && hooks.directories == 2);
    Check(hooks.sizes.size() == 3 && hooks.sizes[0] == 49 + 6 + 49 + input[0].payload.size() &&
          hooks.sizes[1] == 49 + input[1].payload.size() && hooks.sizes[2] == 49 + input[2].payload.size());
    bytes = spool.Bytes();
  }
  hooks.Reset();
  {
    Spool spool(temp.path, "source", 1, LIMIT);
    Check(hooks.data == 3 && hooks.directories == 1 && spool.Bytes() == bytes);
    auto records = spool.ReadAfter(1, LIMIT);
    Check(records.size() == 3 && spool.LastSequence() == 4);
    for (size_t i = 0; i < records.size(); ++i)
      Check(records[i].sequence == i + 2 && records[i].payload == input[i].payload);
    spool.DiscardThrough(3);
    Check(temp.Segments().size() == 1 && spool.ReadAfter(0, LIMIT).front().sequence == 4);
  }
  Temp big;
  {
    Spool spool(big.path, "source", 1, LIMIT);
    hooks.Reset();
    Check(spool.AppendBatch({{0, "f", 1, std::string(17ULL << 20, 'x')}, {0, "f", 2, "tail"}}) == 2);
    Check(big.Segments().size() == 2 && hooks.data == 2);
    hooks.Reset();
    Check(spool.AppendBatch({{0, "f", 3, std::string(17ULL << 20, 'y')}}) == 3);
    Check(big.Segments().size() == 3 && hooks.data == 2 && hooks.directories == 1);
    Check(hooks.sizes.front() == 53 && hooks.sizes.back() == 49 + (17ULL << 20));
  }
  hooks.Reset();
  Spool spool(big.path, "source", 1, LIMIT);
  Check(hooks.data == 3 && hooks.directories == 1 && spool.LastSequence() == 3);
  Check(spool.ReadAfter(0, LIMIT).size() == 3 && spool.Last().payload == std::string(17ULL << 20, 'y'));
}

void DirectoryFailure()
{
  Temp temp;
  {
    Spool spool(temp.path, "source", 1, LIMIT);
    hooks.Reset();
    hooks.fail_directory = true;
    Fails([&] { spool.AppendBatch(Small()); });
    Check(hooks.data == 0 && hooks.directories == 1 && temp.Segments().size() == 1);
    Check(spool.LastSequence() == 0 && spool.Bytes() == 0 && temp.Bytes() == 0);
    Fails([&] { spool.ReadAfter(0, LIMIT); });
    Fails([&] { spool.Last(); });
  }
  hooks.Reset();
  Spool recovered(temp.path, "source", 1, LIMIT);
  Check(hooks.data == 1 && hooks.directories == 1 && recovered.LastSequence() == 0);
  hooks.Reset();
  Check(recovered.AppendBatch(Small()) == 3 && hooks.data == 1 && hooks.directories == 0);
}

void Visibility(bool fail)
{
  Temp temp;
  Spool spool(temp.path, "source", 1, LIMIT);
  spool.Append("f", 1, "prefix");
  const uint64_t bytes = spool.Bytes();
  hooks.Reset(fail ? 1 : 0, 1);
  auto writer = std::async(std::launch::async, [&] {
    try { return spool.AppendBatch(Small()) == 4; }
    catch (const std::runtime_error &) { return false; }
  });
  bool entered = hooks.Wait();
  auto reader = std::async(std::launch::async, [&] {
    try { return spool.ReadAfter(0, LIMIT).size() == 4; }
    catch (const std::runtime_error &) { return false; }
  });
  auto last = std::async(std::launch::async, [&] {
    try { return spool.Last().sequence == 4; }
    catch (const std::runtime_error &) { return false; }
  });
  auto sequence = std::async(std::launch::async, [&] { return spool.LastSequence(); });
  auto size = std::async(std::launch::async, [&] { return spool.Bytes(); });
  bool blocked = reader.wait_for(50ms) == std::future_status::timeout &&
                 last.wait_for(50ms) == std::future_status::timeout &&
                 sequence.wait_for(50ms) == std::future_status::timeout &&
                 size.wait_for(50ms) == std::future_status::timeout;
  hooks.Release();
  bool written = writer.get(), read = reader.get(), got_last = last.get();
  uint64_t published = sequence.get(), published_bytes = size.get();
  Check(entered && blocked && written == !fail && read == !fail && got_last == !fail);
  Check(published == (fail ? 1 : 4) && (fail ? published_bytes == bytes : published_bytes > bytes));
  if (fail)
  {
    Check(temp.Bytes() > bytes);
    Fails([&] { spool.ReadAfter(0, LIMIT); });
    Fails([&] { spool.Last(); });
    Fails([&] { spool.AppendBatch({}); });
    Fails([&] { spool.DiscardThrough(100); });
  }
  hooks.Reset();
}

void RotationFailure()
{
  for (unsigned failure : {1U, 2U, 3U})
  {
    Temp temp;
    {
      Spool spool(temp.path, "source", 1, LIMIT);
      spool.Append("f", 1, "prefix");
      uint64_t bytes = spool.Bytes();
      std::vector<SpoolRecord> input(3, {0, "f", 2, std::string(9ULL << 20, 'x')});
      hooks.Reset(failure);
      Fails([&] { spool.AppendBatch(input); });
      Check(hooks.data == failure && spool.LastSequence() == 1 && spool.Bytes() == bytes);
      Fails([&] { spool.ReadAfter(0, LIMIT); });
      Fails([&] { spool.Last(); });
      Fails([&] { spool.Append("f", 3, "no"); });
      Check(temp.Segments().size() == failure);
    }
    hooks.Reset();
    Spool recovered(temp.path, "source", 1, LIMIT);
    Check(hooks.data == failure && hooks.directories == 1 && recovered.LastSequence() == failure + 1);
    Check(recovered.ReadAfter(0, LIMIT).size() == failure + 1);
  }
}

void ValidTailRestart()
{
  Temp temp;
  { Spool spool(temp.path, "source", 1, LIMIT); }
  pid_t child = fork();
  Check(child >= 0);
  if (child == 0)
  {
    try
    {
      Spool spool(temp.path, "source", 1, LIMIT);
      hooks.Reset();
      hooks.crash = 1;
      spool.AppendBatch(Small());
    }
    catch (...) { _exit(2); }
    _exit(3);
  }
  int status;
  Check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  Check(temp.Segments().size() == 1 && temp.Bytes() > 0);
  hooks.Reset(1);
  Fails([&] { Spool spool(temp.path, "source", 1, LIMIT); });
  Check(hooks.data == 1);
  hooks.Reset();
  hooks.fail_directory = true;
  Fails([&] { Spool spool(temp.path, "source", 1, LIMIT); });
  Check(hooks.data == 1 && hooks.directories == 1);
  hooks.Reset();
  Spool spool(temp.path, "source", 1, LIMIT);
  Check(hooks.data == 1 && hooks.directories == 1 && spool.LastSequence() == 3);
  Check(spool.ReadAfter(0, LIMIT).size() == 3 && spool.Last().payload == "last");
  Check(spool.Append("f", 40, "next") == 4);
}
}

extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd)
{
  struct stat st{};
  if (fstat(fd, &st) != 0) return -1;
  {
    std::unique_lock<std::mutex> guard(hooks.mutex);
    if (S_ISREG(st.st_mode))
    {
      unsigned call = ++hooks.data;
      hooks.sizes.push_back(st.st_size);
      if (call == hooks.crash) kill(getpid(), SIGKILL);
      if (call == hooks.block)
      {
        hooks.entered = true;
        hooks.cv.notify_all();
        hooks.cv.wait(guard, [] { return hooks.released; });
      }
      if (call == hooks.fail) { errno = EIO; return -1; }
    }
    else if (S_ISDIR(st.st_mode))
    {
      ++hooks.directories;
      if (hooks.fail_directory) { errno = EIO; return -1; }
    }
  }
  return __real_fsync(fd);
}

int main()
{
  try
  {
    Basic(); Validation(); Segments(); DirectoryFailure(); Visibility(false); Visibility(true);
    RotationFailure(); ValidTailRestart();
    std::cout << "spool group tests passed\n";
    return 0;
  }
  catch (const std::exception &error)
  {
    hooks.Release();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
