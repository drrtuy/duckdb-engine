#include "spool.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <utility>

namespace myduck::cdc
{
namespace
{
constexpr uint64_t HEADER = SPOOL_RECORD_HEADER_BYTES, SEGMENT = 16ULL << 20, MAX_RECORD = 256ULL << 20;

void Require(bool ok, const char *message)
{
  if (!ok) throw std::runtime_error(std::string("CDC spool: ") + message);
}

struct Fd
{
  int value;
  explicit Fd(int fd = -1) : value(fd) {}
  ~Fd() { if (value >= 0) close(value); }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
};

uint32_t Crc(const std::string &data)
{
  static const auto table = [] {
    std::array<uint32_t, 256> result{};
    for (uint32_t i = 0; i < 256; ++i)
    {
      uint32_t c = i;
      for (int bit = 0; bit < 8; ++bit) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1)));
      result[i] = c;
    }
    return result;
  }();
  uint32_t crc = ~0U;
  for (unsigned char c : data) crc = table[(crc ^ c) & 255] ^ (crc >> 8);
  return ~crc;
}

void Put(std::string &out, uint64_t value, unsigned bytes = 8)
{
  for (unsigned i = 0; i < bytes; ++i) out.push_back(static_cast<char>(value >> (i * 8)));
}

uint64_t Get(const std::string &in, size_t offset, unsigned bytes = 8)
{
  uint64_t value = 0;
  for (unsigned i = 0; i < bytes; ++i) value |= uint64_t(static_cast<unsigned char>(in[offset + i])) << (i * 8);
  return value;
}

void Sync(int fd) { Require(fsync(fd) == 0, "fsync failed"); }

void Write(int fd, const std::string &data)
{
  size_t done = 0;
  while (done < data.size())
  {
    ssize_t n = write(fd, data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR) continue;
    Require(n > 0, "write failed");
    done += static_cast<size_t>(n);
  }
}

std::string Read(int fd, uint64_t offset, size_t size)
{
  std::string data(size, '\0');
  size_t done = 0;
  while (done < size)
  {
    ssize_t n = pread(fd, &data[done], size - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    Require(n > 0, "read failed");
    done += static_cast<size_t>(n);
  }
  return data;
}

uint64_t FileSize(int fd)
{
  struct stat st{};
  Require(fd >= 0 && fstat(fd, &st) == 0, "cannot stat file");
  Require(S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
          (st.st_mode & 07777) == 0600 && st.st_nlink == 1, "unsafe file ownership, type or permissions");
  Require(st.st_size >= 0, "invalid file size");
  return static_cast<uint64_t>(st.st_size);
}

std::string SegmentName(uint64_t sequence)
{
  char name[40];
  std::snprintf(name, sizeof(name), "spool-%020llu.log", static_cast<unsigned long long>(sequence));
  return name;
}

std::vector<std::string> Names(int directory)
{
  DIR *dir = fdopendir(openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  Require(dir != nullptr, "cannot list directory");
  std::vector<std::string> names;
  try
  {
    errno = 0;
    while (auto *entry = readdir(dir))
    {
      std::string name(entry->d_name);
      if (name.compare(0, 6, "spool-") == 0)
      {
        Require(name.size() == 30 && name.substr(26) == ".log" &&
                name.find_first_not_of("0123456789", 6) == 26, "unrecognized segment name");
        names.push_back(name);
      }
    }
    Require(errno == 0, "directory read failed");
  }
  catch (...) { closedir(dir); throw; }
  closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}
}

struct Spool::Impl
{
  struct SegmentInfo { std::string name; uint64_t last, bytes; };
  struct Entry { uint64_t sequence, offset, size; std::string segment; };
  Fd directory, lock, active;
  mutable std::mutex mutex;
  std::vector<SegmentInfo> segments;
  std::vector<Entry> entries;
  uint64_t bytes = 0, last = 0, limit = 0;
  bool failed = false;

  SpoolRecord Decode(int fd, uint64_t offset, uint64_t available, uint64_t &size)
  {
    size = 0;
    if (available < HEADER) return {};
    std::string h = Read(fd, offset, HEADER);
    Require(h.substr(0, 8) == "CDCS0001" && Get(h, 40, 4) == Crc(h.substr(0, 40)), "corrupt record header");
    uint64_t file = Get(h, 24), payload = Get(h, 32);
    Require(file <= MAX_RECORD - HEADER && payload <= MAX_RECORD - HEADER - file, "record too large");
    if (HEADER + file + payload > available) return {};
    size = HEADER + file + payload;
    std::string body = Read(fd, offset + HEADER, static_cast<size_t>(file + payload));
    Require(Get(h, 44, 4) == Crc(body), "record checksum mismatch");
    return {Get(h, 8), body.substr(0, file), Get(h, 16), body.substr(file)};
  }

  SpoolRecord Load(const Entry &entry)
  {
    Fd fd(openat(directory.value, entry.segment.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    uint64_t available = FileSize(fd.value), size;
    Require(available >= entry.offset + entry.size, "record disappeared");
    auto record = Decode(fd.value, entry.offset, available - entry.offset, size);
    Require(size == entry.size && record.sequence == entry.sequence, "record changed");
    return record;
  }

  void Init(std::string path, const std::string &identity, uint64_t next, uint64_t max_bytes)
  {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    Require(next > 0 && !identity.empty() && identity.size() <= 65536, "invalid initial sequence or identity");
    last = next - 1;
    limit = max_bytes;
    bool made_directory = mkdir(path.c_str(), 0700) == 0;
    Require(made_directory || errno == EEXIST, "cannot create directory");
    if (made_directory)
    {
      auto slash = path.find_last_of('/');
      std::string parent = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
      Fd fd(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
      Require(fd.value >= 0, "cannot open parent directory");
      Sync(fd.value);
    }
    directory.value = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    Require(directory.value >= 0, "cannot open directory");
    struct stat st{};
    Require(fstat(directory.value, &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 0022) == 0, "unsafe directory");
    auto names = Names(directory.value);
    bool created = false;
    lock.value = openat(directory.value, "identity", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (lock.value < 0 && errno == ENOENT)
    {
      Require(names.empty(), "identity missing for existing segments");
      lock.value = openat(directory.value, "identity", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      created = true;
    }
    uint64_t identity_size = FileSize(lock.value);
    Require(flock(lock.value, LOCK_EX | LOCK_NB) == 0, "directory already in use");
    std::string expected = "CDCI0001" + identity;
    Put(expected, Crc(expected), 4);
    if (created) { Write(lock.value, expected); Sync(lock.value); Sync(directory.value); }
    else Require(identity_size == expected.size() && Read(lock.value, 0, identity_size) == expected, "identity mismatch or corruption");
    names = Names(directory.value);
    bool have_record = false;
    for (size_t i = 0; i < names.size(); ++i)
    {
      Fd fd(openat(directory.value, names[i].c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
      uint64_t length = FileSize(fd.value), offset = 0;
      segments.push_back({names[i], 0, 0});
      while (offset < length)
      {
        uint64_t size;
        auto record = Decode(fd.value, offset, length - offset, size);
        if (!size)
        {
          Require(i + 1 == names.size(), "incomplete nonfinal segment");
          Require(ftruncate(fd.value, static_cast<off_t>(offset)) == 0, "tail truncation failed");
          break;
        }
        Require(record.sequence > 0 && (!have_record || (last != UINT64_MAX && record.sequence == last + 1)), "nonmonotonic sequence");
        Require(offset != 0 || names[i] == SegmentName(record.sequence), "segment sequence mismatch");
        entries.push_back({record.sequence, offset, size, names[i]});
        have_record = true;
        last = segments.back().last = record.sequence;
        offset += size;
      }
      Require(offset != 0 || (i + 1 == names.size() && last != UINT64_MAX && names[i] == SegmentName(last + 1)), "invalid empty segment");
      segments.back().bytes = offset;
      bytes += offset;
      Sync(fd.value);
    }
    Sync(directory.value);
    if (!segments.empty())
    {
      active.value = openat(directory.value, segments.back().name.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
      FileSize(active.value);
    }
  }
};

Spool::Spool(const std::string &directory, const std::string &identity,
             uint64_t next_sequence, uint64_t max_bytes) : impl_(new Impl)
{
  impl_->Init(directory, identity, next_sequence, max_bytes);
}
Spool::~Spool() = default;

uint64_t Spool::Append(const std::string &file, uint64_t pos, const std::string &payload)
{
  std::vector<SpoolRecord> records;
  records.push_back({0, file, pos, payload});
  return AppendBatch(records);
}

uint64_t Spool::AppendBatch(const std::vector<SpoolRecord> &records)
{
  auto &s = *impl_;
  std::lock_guard<std::mutex> guard(s.mutex);
  Require(!s.failed, "previous I/O failure requires restart");
  if (records.empty()) return s.last;
  Require(records.size() <= UINT64_MAX - s.last, "sequence exhausted");
  uint64_t bytes = s.bytes;
  for (const auto &record : records)
  {
    Require(record.source_file.size() <= MAX_RECORD - HEADER &&
            record.payload.size() <= MAX_RECORD - HEADER - record.source_file.size(), "record too large");
    uint64_t size = HEADER + record.source_file.size() + record.payload.size();
    Require(bytes <= s.limit && size <= s.limit - bytes, "capacity exceeded");
    bytes += size;
  }
  Require(records.size() <= s.entries.max_size() - s.entries.size(), "too many records");
  if (s.entries.size() + records.size() > s.entries.capacity())
    s.entries.reserve(std::max(s.entries.size() + records.size(),
        s.entries.capacity() + std::min(s.entries.capacity(), s.entries.max_size() - s.entries.capacity())));
  auto segments = s.segments;
  std::vector<Impl::Entry> entries;
  entries.reserve(records.size());
  uint64_t sequence = s.last;
  try
  {
    for (const auto &record : records)
    {
      ++sequence;
      uint64_t size = HEADER + record.source_file.size() + record.payload.size();
      std::string body = record.source_file + record.payload, header = "CDCS0001";
      Put(header, sequence); Put(header, record.source_position);
      Put(header, record.source_file.size()); Put(header, record.payload.size());
      Put(header, Crc(header), 4); Put(header, Crc(body), 4);
      if (segments.empty() || (segments.back().bytes && segments.back().bytes + size > SEGMENT))
      {
        if (s.active.value >= 0) Sync(s.active.value);
        std::string name = SegmentName(sequence);
        Fd fd(openat(s.directory.value, name.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        FileSize(fd.value);
        Sync(s.directory.value);
        segments.push_back({name, 0, 0});
        if (s.active.value >= 0) close(s.active.value);
        s.active.value = fd.value; fd.value = -1;
      }
      auto &segment = segments.back();
      Write(s.active.value, header); Write(s.active.value, body);
      entries.push_back({sequence, segment.bytes, size, segment.name});
      segment.bytes += size; segment.last = sequence;
    }
    Sync(s.active.value);
    for (auto &entry : entries) s.entries.push_back(std::move(entry));
    s.segments.swap(segments);
    s.bytes = bytes;
    s.last = sequence;
    return sequence;
  }
  catch (...) { s.failed = true; throw; }
}

std::vector<SpoolRecord> Spool::ReadAfter(uint64_t sequence, uint64_t max_bytes)
{
  auto &s = *impl_;
  std::lock_guard<std::mutex> guard(s.mutex);
  Require(!s.failed, "previous I/O failure requires restart");
  std::vector<SpoolRecord> result;
  uint64_t used = 0;
  for (const auto &entry : s.entries)
  {
    if (entry.sequence <= sequence) continue;
    if (max_bytes == 0 || (!result.empty() && entry.size > max_bytes - std::min(used, max_bytes))) break;
    result.push_back(s.Load(entry));
    used += entry.size;
  }
  return result;
}

void Spool::DiscardThrough(uint64_t sequence)
{
  auto &s = *impl_;
  std::lock_guard<std::mutex> guard(s.mutex);
  Require(!s.failed, "previous I/O failure requires restart");
  try
  {
    while (s.segments.size() > 1 && s.segments.front().last <= sequence)
    {
      const auto segment = s.segments.front();
      Require(unlinkat(s.directory.value, segment.name.c_str(), 0) == 0, "segment deletion failed");
      Sync(s.directory.value);
      s.bytes -= segment.bytes;
      s.entries.erase(s.entries.begin(), std::find_if(s.entries.begin(), s.entries.end(),
          [&](const Impl::Entry &entry) { return entry.segment != segment.name; }));
      s.segments.erase(s.segments.begin());
    }
  }
  catch (...) { s.failed = true; throw; }
}

uint64_t Spool::Bytes() const
{
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->bytes;
}
uint64_t Spool::LastSequence() const
{
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->last;
}
SpoolRecord Spool::Last() const
{
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Require(!impl_->failed, "previous I/O failure requires restart");
  return impl_->entries.empty() ? SpoolRecord{} : impl_->Load(impl_->entries.back());
}

}
