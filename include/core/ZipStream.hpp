#pragma once
// A zip read as it downloads: its central directory first (from the last
// bytes of the file), then each wanted entry written to the card as its
// bytes arrive, inflated and checked against its CRC. No temporary copy of
// the zip goes to the card.
#include "core/ArchiveWriter.hpp"
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>
namespace foil {
// One file in a zip, as its central directory gives it.
struct ZipEntry {
  std::string name;
  uint64_t offset = 0; // of its local header
  uint64_t compressed = 0, size = 0;
  uint32_t crc = 0;
  int method = 0; // 0 stored, 8 deflate
  bool encrypted = false;
};
// The last bytes of a zip that hold its end record (and a zip64 one).
constexpr uint64_t ZipTailBytes = 65557 + 20 + 56;
// Where the central directory sits, from `tail`: the file's bytes from
// `tailStart` to its end (`zipSize`). False when there is no end record.
bool zipDirectory(const std::string &tail, uint64_t tailStart, uint64_t zipSize, uint64_t &offset, uint64_t &size);
// The central directory's entries; false when it does not read as one.
bool zipEntries(const std::string &directory, std::vector<ZipEntry> &out);

// Writes the wanted entries (each to root/<path>) from the zip's bytes in
// order; everything else in the zip is read past.
class ZipWriter : public ArchiveWriter {
public:
  ZipWriter(std::string root, std::vector<std::pair<ZipEntry, std::string>> wanted);
  ~ZipWriter() override;
  bool feed(const char *data, size_t size) override;
  bool finish() override;
  const std::string &error() const override { return error_; }
  void startAt(uint64_t offset) override;
  uint64_t boundary() const override { return boundary_; }
  void abandon() override;

private:
  struct Job {
    ZipEntry entry;
    std::string path;
  };
  struct Inflater;
  enum class Phase { Skip, Header, Name, Data };
  bool fail(const std::string &message);
  bool open();
  bool write(const char *data, size_t size);
  bool put(const char *data, size_t size);
  bool close();
  std::string root_, error_, header_;
  std::vector<Job> jobs_; // by offset
  size_t next_ = 0;
  Phase phase_ = Phase::Skip;
  uint64_t fed_ = 0, boundary_ = 0, left_ = 0, produced_ = 0;
  uint32_t crc_ = 0;
  std::unique_ptr<Inflater> inflater_;
  std::FILE *file_ = nullptr;
};
} // namespace foil
