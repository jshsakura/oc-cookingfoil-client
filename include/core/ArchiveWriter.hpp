#pragma once
// An archive unpacked as its bytes arrive in order (a tar or a zip), so a
// ranged download can feed it and resume it at a whole-file boundary.
#include <cstddef>
#include <cstdint>
#include <string>
namespace foil {
class ArchiveWriter {
public:
  virtual ~ArchiveWriter() = default;
  // False stops the download; error() says why.
  virtual bool feed(const char *data, size_t size) = 0;
  // True when every file the archive should give is on the card.
  virtual bool finish() = 0;
  virtual const std::string &error() const = 0;
  // Resuming: a fresh writer told where in the stream its first byte sits,
  // which must be a boundary an earlier writer reported.
  virtual void startAt(uint64_t offset) = 0;
  // Stream offset after the last whole file: everything before it is
  // complete on the card, so a download can continue here.
  virtual uint64_t boundary() const = 0;
  // Closes and removes the file an unfinished entry was writing.
  virtual void abandon() = 0;
};
} // namespace foil
