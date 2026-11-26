#include "spool.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using myduck::cdc::Spool;

namespace
{
constexpr uint64_t LIMIT = 512ULL << 20;

void Check(bool value)
{
  if (!value) throw std::runtime_error("test assertion failed");
}

template <class F> void Fails(F operation)
{
  bool failed = false;
  try { operation(); }
  catch (const std::runtime_error &) { failed = true; }
  Check(failed);
}

struct Temp
{
  std::string path;
  Temp()
  {
    char name[] = "/tmp/myduck-spool-test-XXXXXX";
    char *created = mkdtemp(name);
    Check(created != nullptr);
    path = created;
  }
  std::vector<std::string> Files() const
  {
    DIR *dir = opendir(path.c_str());
    Check(dir != nullptr);
    std::vector<std::string> files;
    while (auto *entry = readdir(dir))
      if (entry->d_name[0] != '.') files.push_back(path + "/" + entry->d_name);
    closedir(dir);
    std::sort(files.begin(), files.end());
    return files;
  }
  std::vector<std::string> Segments() const
  {
    auto files = Files();
    files.erase(std::remove_if(files.begin(), files.end(), [](const std::string &file) {
      return file.find("/spool-") == std::string::npos;
    }), files.end());
    return files;
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

uint64_t Size(const std::string &path)
{
  struct stat st{};
  Check(stat(path.c_str(), &st) == 0);
  return st.st_size;
}

void Flip(const std::string &path, off_t offset)
{
  int fd = open(path.c_str(), O_RDWR | O_NOFOLLOW);
  Check(fd >= 0);
  char byte;
  Check(pread(fd, &byte, 1, offset) == 1);
  byte ^= 1;
  Check(pwrite(fd, &byte, 1, offset) == 1);
  Check(close(fd) == 0);
}

void Normal()
{
  Temp temp;
  uint64_t bytes;
  {
    Spool spool(temp.path, "source-A", 7, LIMIT);
    Check(spool.LastSequence() == 6 && spool.Last().sequence == 0);
    Check(spool.Append("bin.001", 12, std::string("a\0b", 3)) == 7);
    Check(spool.Append("bin.002", 44, "format+transaction") == 8);
    bytes = spool.Bytes();
    Check(bytes == 96 + 14 + 3 + 18);
    Check(spool.ReadAfter(0, 0).empty());
    auto one = spool.ReadAfter(0, 1);
    Check(one.size() == 1 && one[0].sequence == 7 && one[0].payload == std::string("a\0b", 3));
    Check(spool.ReadAfter(7, LIMIT).size() == 1);
    Check(spool.ReadAfter(8, LIMIT).empty());
    Check(spool.Last().source_position == 44 && spool.Last().source_file == "bin.002");
    Fails([&] { Spool collision(temp.path, "source-A", 1, LIMIT); });
    spool.DiscardThrough(8);
    Check(spool.Bytes() == bytes);
  }
  {
    Spool spool(temp.path, "source-A", 1, LIMIT);
    Check(spool.Bytes() == bytes && spool.LastSequence() == 8);
    Check(spool.Append("bin.002", 45, "next") == 9);
  }
  {
    Spool spool(temp.path, "source-A", 500, LIMIT);
    Check(spool.LastSequence() == 9);
  }
  Fails([&] { Spool wrong(temp.path, "source-B", 1, LIMIT); });
  for (const auto &file : temp.Files())
  {
    struct stat st{};
    Check(lstat(file.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
  }
}

void Capacity()
{
  Temp temp;
  Spool spool(temp.path, "source", 1, 50);
  Check(spool.Append("f", 0, "x") == 1);
  Fails([&] { spool.Append("f", 1, ""); });
  Check(spool.Bytes() == 50 && spool.LastSequence() == 1);
  Fails([&] { spool.Append("", 1, std::string(256ULL << 20, 'x')); });
  Check(spool.LastSequence() == 1);
}

void Recovery()
{
  for (int cut : {1, 47, 49, 60})
  {
    Temp temp;
    uint64_t prefix;
    {
      Spool spool(temp.path, "source", 1, LIMIT);
      spool.Append("f", 1, "first");
      prefix = spool.Bytes();
      spool.Append("f", 2, std::string(100, 'x'));
    }
    auto file = temp.Segments()[0];
    Check(truncate(file.c_str(), prefix + cut) == 0);
    {
      Spool spool(temp.path, "source", 1, LIMIT);
      Check(spool.LastSequence() == 1 && spool.Bytes() == prefix && Size(file) == prefix);
      Check(spool.Append("f", 3, "replacement") == 2);
    }
    Spool spool(temp.path, "source", 1, LIMIT);
    Check(spool.Last().payload == "replacement");
  }
  for (off_t offset : {off_t(8), off_t(32), off_t(49)})
  {
    Temp temp;
    { Spool spool(temp.path, "source", 1, LIMIT); spool.Append("f", 1, "payload"); }
    auto file = temp.Segments()[0];
    uint64_t before = Size(file);
    Flip(file, offset);
    Fails([&] { Spool spool(temp.path, "source", 1, LIMIT); });
    Check(Size(file) == before);
  }
}

void Sequences()
{
  Temp empty;
  { Spool spool(empty.path, "source", 9, LIMIT); Check(spool.LastSequence() == 8); }
  {
    Spool spool(empty.path, "source", 20, LIMIT);
    Check(spool.Append("f", 1, "x") == 20);
  }
  Temp duplicate;
  {
    Spool spool(duplicate.path, "source", 1, LIMIT);
    spool.Append("f", 1, "x");
    spool.Append("f", 2, "y");
  }
  auto file = duplicate.Segments()[0];
  int fd = open(file.c_str(), O_RDWR | O_NOFOLLOW);
  Check(fd >= 0);
  char record[50];
  Check(pread(fd, record, sizeof(record), 0) == sizeof(record));
  Check(pwrite(fd, record, sizeof(record), sizeof(record)) == sizeof(record));
  Check(close(fd) == 0);
  Fails([&] { Spool spool(duplicate.path, "source", 1, LIMIT); });
  Temp tail;
  { Spool spool(tail.path, "source", 1, LIMIT); spool.Append("f", 1, "x"); }
  std::string name = tail.path + "/spool-00000000000000000002.log";
  fd = open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  Check(fd >= 0 && close(fd) == 0);
  {
    Spool spool(tail.path, "source", 1, LIMIT);
    Check(spool.LastSequence() == 1 && spool.Append("f", 2, "y") == 2);
  }
  Spool spool(tail.path, "source", 1, LIMIT);
  Check(spool.LastSequence() == 2);
}

void Collection()
{
  Temp temp;
  const std::string large(9ULL << 20, 'x');
  {
    Spool spool(temp.path, "source", 1, LIMIT);
    spool.Append("f", 1, "small");
    spool.Append("f", 2, large);
    spool.Append("f", 3, large);
    spool.Append("f", 4, large);
    Check(temp.Segments().size() == 3);
    spool.DiscardThrough(1);
    Check(temp.Segments().size() == 3);
    spool.DiscardThrough(2);
    Check(temp.Segments().size() == 2 && spool.ReadAfter(0, LIMIT)[0].sequence == 3);
    spool.DiscardThrough(100);
    Check(temp.Segments().size() == 1 && spool.Bytes() == 48 + 1 + large.size());
  }
  {
    Spool spool(temp.path, "source", 2, LIMIT);
    Check(spool.LastSequence() == 4 && spool.Append("f", 5, "small") == 5);
  }
  auto file = temp.Segments()[0];
  Check(rename(file.c_str(), (temp.path + "/spool-00000000000000000002.log").c_str()) == 0);
  Fails([&] { Spool spool(temp.path, "source", 1, LIMIT); });
}

void NonfinalTail()
{
  Temp temp;
  {
    Spool spool(temp.path, "source", 1, LIMIT);
    spool.Append("f", 1, std::string(9ULL << 20, 'x'));
    spool.Append("f", 2, std::string(9ULL << 20, 'x'));
  }
  auto file = temp.Segments()[0];
  Check(truncate(file.c_str(), Size(file) - 1) == 0);
  Fails([&] { Spool spool(temp.path, "source", 1, LIMIT); });
}

void Security()
{
  Temp target, alias;
  Check(symlink(target.path.c_str(), (alias.path + "/link").c_str()) == 0);
  Fails([&] { Spool spool(alias.path + "/link/", "source", 1, LIMIT); });
  Check(target.Files().empty());
  Temp temp;
  { Spool spool(temp.path, "source", 1, LIMIT); spool.Append("f", 1, "payload"); }
  auto file = temp.Segments()[0];
  Check(chmod(file.c_str(), 0644) == 0);
  Fails([&] { Spool spool(temp.path, "source", 1, LIMIT); });
  Check(chmod(file.c_str(), 0600) == 0);
  std::string backup = temp.path + "/backup";
  Check(rename(file.c_str(), backup.c_str()) == 0);
  Check(symlink(backup.c_str(), file.c_str()) == 0);
  Fails([&] { Spool spool(temp.path, "source", 1, LIMIT); });
  Check(Size(backup) == 56);
  Check(unlink(file.c_str()) == 0);
  Check(rename(backup.c_str(), file.c_str()) == 0);
  Check(unlink((temp.path + "/identity").c_str()) == 0);
  Fails([&] { Spool spool(temp.path, "source", 1, LIMIT); });
  Check(Size(file) == 56);
}

void Concurrent()
{
  Temp temp;
  Spool spool(temp.path, "source", 1, LIMIT);
  std::atomic<bool> done{false}, ok{true};
  std::thread reader([&] {
    try
    {
      while (!done)
      {
        auto records = spool.ReadAfter(0, LIMIT);
        for (size_t i = 0; i < records.size(); ++i)
          if (records[i].sequence != i + 1 || records[i].payload != "payload") ok = false;
        spool.Last(); spool.Bytes(); spool.LastSequence(); spool.DiscardThrough(0);
      }
    }
    catch (...) { ok = false; }
  });
  try { for (int i = 0; i < 100; ++i) spool.Append("f", i, "payload"); }
  catch (...) { done = true; reader.join(); throw; }
  done = true;
  reader.join();
  Check(ok && spool.LastSequence() == 100);
}
}

int main()
{
  try
  {
    Normal(); Capacity(); Recovery(); Sequences(); Collection(); NonfinalTail(); Security(); Concurrent();
    std::cout << "spool tests passed\n";
    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
