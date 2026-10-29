#include "platform/FileSystem.hpp"
#include "core/ZipStream.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "platform/Storage.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <zlib.h>
namespace foil {
namespace {
uint16_t le16(const std::string &b, size_t at) {
  return uint16_t(uint8_t(b[at]) | uint8_t(b[at + 1]) << 8);
}
uint32_t le32(const std::string &b, size_t at) { return uint32_t(le16(b, at)) | uint32_t(le16(b, at + 2)) << 16; }
uint64_t le64(const std::string &b, size_t at) { return uint64_t(le32(b, at)) | uint64_t(le32(b, at + 4)) << 32; }
constexpr uint32_t EndRecord = 0x06054b50, Zip64Locator = 0x07064b50, Zip64End = 0x06064b50,
                   DirectoryRecord = 0x02014b50, LocalHeader = 0x04034b50;
constexpr size_t LocalHeaderBytes = 30;
} // namespace

bool zipDirectory(const std::string &tail, uint64_t tailStart, uint64_t zipSize, uint64_t &offset, uint64_t &size) {
  if (tail.size() < 22)
    return false;
  // The end record is the last thing in the file, after a comment of at most
  // 64 KB: the last signature whose comment reaches exactly to the end.
  for (size_t at = tail.size() - 22;; --at) {
    if (le32(tail, at) == EndRecord && at + 22 + le16(tail, at + 20) == tail.size()) {
      size = le32(tail, at + 12);
      offset = le32(tail, at + 16);
      if ((size == 0xFFFFFFFF || offset == 0xFFFFFFFF || le16(tail, at + 10) == 0xFFFF) && at >= 20 &&
          le32(tail, at - 20) == Zip64Locator) {
        const uint64_t end64 = le64(tail, at - 20 + 8);
        // Subtracted, never added: a forged end64 near 2^64 would wrap a sum
        // past the check and read outside the tail.
        if (end64 < tailStart || end64 - tailStart > tail.size() || tail.size() - (end64 - tailStart) < 56)
          return false;
        const size_t z = size_t(end64 - tailStart);
        if (le32(tail, z) != Zip64End)
          return false;
        size = le64(tail, z + 40);
        offset = le64(tail, z + 48);
      }
      return offset <= zipSize && size <= zipSize - offset;
    }
    if (at == 0)
      return false;
  }
}

bool zipEntries(const std::string &d, std::vector<ZipEntry> &out) {
  out.clear();
  size_t at = 0;
  while (at + 46 <= d.size() && le32(d, at) == DirectoryRecord) {
    ZipEntry e;
    e.encrypted = le16(d, at + 8) & 1;
    e.method = le16(d, at + 10);
    e.crc = le32(d, at + 16);
    e.compressed = le32(d, at + 20);
    e.size = le32(d, at + 24);
    const size_t name = le16(d, at + 28), extra = le16(d, at + 30), comment = le16(d, at + 32);
    e.offset = le32(d, at + 42);
    if (at + 46 + name + extra + comment > d.size())
      return false;
    e.name = d.substr(at + 46, name);
    // zip64: the fields that read 0xFFFFFFFF follow in its extra field.
    for (size_t x = at + 46 + name; x + 4 <= at + 46 + name + extra;) {
      const size_t id = le16(d, x), length = le16(d, x + 2);
      if (x + 4 + length > at + 46 + name + extra)
        break;
      if (id == 1) {
        size_t f = x + 4;
        const auto take = [&](uint64_t &field) {
          if (field == 0xFFFFFFFF && f + 8 <= x + 4 + length) {
            field = le64(d, f);
            f += 8;
          }
        };
        take(e.size);
        take(e.compressed);
        take(e.offset);
      }
      x += 4 + length;
    }
    out.push_back(std::move(e));
    at += 46 + name + extra + comment;
  }
  return at == d.size() || (at + 4 <= d.size() && le32(d, at) != DirectoryRecord);
}

struct ZipWriter::Inflater {
  z_stream z{};
  bool ended = false;
  Inflater() {
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
      throw std::bad_alloc();
  }
  ~Inflater() { inflateEnd(&z); }
};

ZipWriter::ZipWriter(std::string root, std::vector<std::pair<ZipEntry, std::string>> wanted) : root_(std::move(root)) {
  for (auto &[entry, path] : wanted)
    jobs_.push_back({std::move(entry), std::move(path)});
  std::sort(jobs_.begin(), jobs_.end(), [](const Job &a, const Job &b) { return a.entry.offset < b.entry.offset; });
}
ZipWriter::~ZipWriter() {
  if (file_)
    std::fclose(file_);
}
bool ZipWriter::fail(const std::string &message) {
  if (error_.empty())
    error_ = message;
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
    std::error_code ec;
    platform::removePath(writingPart(root_ + "/" + jobs_[next_].path), ec); // the file it would replace stays
  }
  return false;
}
void ZipWriter::startAt(uint64_t offset) {
  fed_ = boundary_ = offset;
  next_ = 0;
  while (next_ < jobs_.size() && jobs_[next_].entry.offset < offset)
    ++next_;
  phase_ = Phase::Skip;
}
void ZipWriter::abandon() {
  const bool writing = file_ != nullptr;
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  if (writing && next_ < jobs_.size()) {
    std::error_code ec;
    platform::removePath(writingPart(root_ + "/" + jobs_[next_].path), ec);
  }
}
bool ZipWriter::open() {
  const auto &job = jobs_[next_];
  if (job.entry.encrypted || (job.entry.method != 0 && job.entry.method != 8))
    return fail(trf("This archive cannot be unpacked: {0}", {job.entry.name}));
  // Written beside its place, which it takes once whole and checked.
  const std::filesystem::path full = writingPart(root_ + "/" + job.path);
  makeDirectories(full.parent_path().string());
  // 4 GiB or more: made as a split file first, or FAT32 would cut it short.
  if (std::string reason; platform::needsBigFile(job.entry.size) && !platform::createBigFile(full.string(), reason))
    return fail(trf("Could not create a file over 4 GB on the SD card: {0}", {job.path}) + " (" + reason + ")");
  file_ = std::fopen(full.string().c_str(), "wb");
  if (!file_) {
    const std::string reason = std::strerror(errno);
    logLine("zip: cannot create " + full.string() + ": " + reason);
    return fail(trf("Could not create file: {0}", {job.path}) + " (" + reason + ")");
  }
  writeBuffer(file_);
  inflater_.reset();
  if (job.entry.method == 8)
    inflater_ = std::make_unique<Inflater>();
  produced_ = 0;
  crc_ = uint32_t(crc32(0, nullptr, 0));
  return true;
}
bool ZipWriter::put(const char *data, size_t size) {
  produced_ += size;
  if (produced_ > jobs_[next_].entry.size)
    return fail(trf("The archive does not match the server's list: {0}", {jobs_[next_].path}));
  crc_ = uint32_t(crc32(crc_, reinterpret_cast<const Bytef *>(data), uInt(size)));
  if (size && std::fwrite(data, 1, size, file_) != size)
    return fail(tr("Could not write to the SD card. Check the free space."));
  return true;
}
bool ZipWriter::write(const char *data, size_t size) {
  if (!inflater_)
    return put(data, size);
  auto &z = inflater_->z;
  z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data));
  z.avail_in = uInt(size);
  char out[64 * 1024];
  while (z.avail_in > 0 && !inflater_->ended) {
    z.next_out = reinterpret_cast<Bytef *>(out);
    z.avail_out = sizeof out;
    const int rc = inflate(&z, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END)
      return fail(trf("This archive cannot be unpacked: {0}", {jobs_[next_].entry.name}));
    if (!put(out, sizeof out - z.avail_out))
      return false;
    inflater_->ended = rc == Z_STREAM_END;
  }
  // Output the window still holds once the input is in.
  while (!inflater_->ended) {
    z.next_out = reinterpret_cast<Bytef *>(out);
    z.avail_out = sizeof out;
    const int rc = inflate(&z, Z_NO_FLUSH);
    if (rc == Z_BUF_ERROR)
      break; // needs more input
    if (rc != Z_OK && rc != Z_STREAM_END)
      return fail(trf("This archive cannot be unpacked: {0}", {jobs_[next_].entry.name}));
    if (!put(out, sizeof out - z.avail_out))
      return false;
    inflater_->ended = rc == Z_STREAM_END;
    if (z.avail_out != 0 && rc == Z_OK)
      break;
  }
  return true;
}
bool ZipWriter::close() {
  const auto &job = jobs_[next_];
  const auto part = writingPart(root_ + "/" + job.path);
  const bool closed = std::fclose(file_) == 0;
  file_ = nullptr;
  std::error_code ec;
  if (!closed) {
    platform::removePath(part, ec);
    return fail(tr("Could not write to the SD card. Check the free space."));
  }
  if ((inflater_ && !inflater_->ended) || produced_ != job.entry.size || crc_ != job.entry.crc) {
    platform::removePath(part, ec);
    return fail(trf("The archive does not match the server's list: {0}", {job.path}));
  }
  if (std::string reason; !replaceFile(part, root_ + "/" + job.path, reason)) {
    platform::removePath(part, ec);
    logLine("zip: cannot put " + job.path + " in place: " + reason);
    return fail(trf("Could not write the file: {0}", {job.path}) + " (" + reason + ")");
  }
  inflater_.reset();
  boundary_ = fed_;
  ++next_;
  phase_ = Phase::Skip;
  return true;
}
bool ZipWriter::feed(const char *data, size_t size) {
  if (!error_.empty())
    return false;
  const auto consume = [&](size_t take) {
    data += take;
    size -= take;
    fed_ += take;
  };
  while (size > 0) {
    if (next_ >= jobs_.size()) { // all written: the rest (the directory) is read past
      consume(size);
      return true;
    }
    const auto &job = jobs_[next_];
    switch (phase_) {
    case Phase::Skip: {
      if (fed_ > job.entry.offset)
        return fail(tr("This archive cannot be unpacked."));
      const auto take = size_t(std::min<uint64_t>(job.entry.offset - fed_, size));
      consume(take);
      if (fed_ == job.entry.offset) {
        header_.clear();
        phase_ = Phase::Header;
      }
      break;
    }
    case Phase::Header: {
      const auto take = std::min(LocalHeaderBytes - header_.size(), size);
      header_.append(data, take);
      consume(take);
      if (header_.size() == LocalHeaderBytes) {
        if (le32(header_, 0) != LocalHeader)
          return fail(tr("This archive cannot be unpacked."));
        left_ = uint64_t(le16(header_, 26)) + le16(header_, 28);
        phase_ = Phase::Name;
      }
      break;
    }
    case Phase::Name: {
      const auto take = size_t(std::min<uint64_t>(left_, size));
      consume(take);
      left_ -= take;
      break;
    }
    case Phase::Data: {
      const auto take = size_t(std::min<uint64_t>(left_, size));
      if (!write(data, take))
        return false;
      consume(take);
      left_ -= take;
      break;
    }
    }
    if (phase_ == Phase::Name && left_ == 0) {
      if (!open())
        return false;
      left_ = job.entry.compressed;
      phase_ = Phase::Data;
    }
    if (phase_ == Phase::Data && left_ == 0 && !close())
      return false;
  }
  return true;
}
bool ZipWriter::finish() {
  if (!error_.empty())
    return false;
  if (next_ < jobs_.size())
    return fail(tr("The download was interrupted. Download it again."));
  return true;
}
} // namespace foil
