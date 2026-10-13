#define ZSTD_STATIC_LINKING_ONLY
#include "core/ZstdBlockDecoder.hpp"
#include "ZstdSnapshot.h"
#include <zstd.h>
#include <algorithm>
#include <cstring>
namespace foil {
namespace {
constexpr uint32_t SavedMagic = 0x4344425au; // "ZBDC"
// A window above this is not a frame this app writes (NSZ uses 8 MB).
constexpr uint64_t MaxWindow = 1ull << 27;
template <typename T> void put(std::string &out, T value) { out.append(reinterpret_cast<const char *>(&value), sizeof(T)); }
template <typename T> bool get(const std::string &in, size_t &at, T &value) {
  if (at + sizeof(T) > in.size())
    return false;
  std::memcpy(&value, in.data() + at, sizeof(T));
  at += sizeof(T);
  return true;
}
} // namespace
ZstdBlockDecoder::ZstdBlockDecoder(Output out) : out_(std::move(out)), dctx_(ZSTD_createDCtx()) {
  if (dctx_)
    ZSTD_decompressBegin(dctx_);
}
ZstdBlockDecoder::~ZstdBlockDecoder() { ZSTD_freeDCtx(dctx_); }
bool ZstdBlockDecoder::atBoundary() const {
  return dctx_ && frameStarted_ && pending_.empty() && ZSTDR_atBlockBoundary(dctx_);
}
bool ZstdBlockDecoder::ensureRing(std::string &error) {
  if (!ring_.empty())
    return true;
  if (windowSize_ == 0 || windowSize_ > MaxWindow) {
    error = "zstd window " + std::to_string(windowSize_);
    return false;
  }
  ring_.resize(ZSTD_decodingBufferSize_min(windowSize_, ZSTD_CONTENTSIZE_UNKNOWN));
  pos_ = 0;
  return true;
}
// One step of the buffer-less decoder: a frame header, a block header, a
// block or the checksum, each exactly the size zstd asks for next.
bool ZstdBlockDecoder::step(std::string &error) {
  const size_t need = ZSTD_nextSrcSizeToDecompress(dctx_);
  const auto type = ZSTD_nextInputType(dctx_);
  if (type == ZSTDnit_frameHeader) {
    header_.insert(header_.end(), pending_.begin(), pending_.end());
    frameStarted_ = false;
  }
  uint8_t *dst = nullptr;
  size_t room = 0;
  if (type == ZSTDnit_block || type == ZSTDnit_lastBlock) {
    if (!ensureRing(error))
      return false;
    // The ring wraps when a block might not fit; zstd keeps track of the
    // two stretches (decodingBufferSize_min leaves room for a full window).
    if (pos_ + ZSTD_BLOCKSIZE_MAX > ring_.size())
      pos_ = 0;
    dst = ring_.data() + pos_;
    room = ring_.size() - pos_;
  }
  const size_t made = ZSTD_decompressContinue(dctx_, dst, room, pending_.data(), need);
  if (ZSTD_isError(made)) {
    error = ZSTD_getErrorName(made);
    return false;
  }
  consumed_ += need;
  pending_.clear();
  if (made) {
    out_(dst, made);
    pos_ += made;
    produced_ += made;
  }
  if (type == ZSTDnit_frameHeader && ZSTD_nextInputType(dctx_) != ZSTDnit_frameHeader) {
    // The whole header is in: its window sizes the ring.
    ZSTD_FrameHeader frame{};
    if (ZSTD_getFrameHeader(&frame, header_.data(), header_.size()) != 0) {
      error = "zstd frame header";
      return false;
    }
    windowSize_ = frame.windowSize;
    header_.clear();
    ring_.clear();
    frameStarted_ = true;
  }
  if (boundary_ && atBoundary())
    boundary_();
  return true;
}
bool ZstdBlockDecoder::write(const uint8_t *data, size_t size, std::string &error) {
  if (!dctx_) {
    error = "zstd";
    return false;
  }
  while (size) {
    size_t need = ZSTD_nextSrcSizeToDecompress(dctx_);
    if (need == 0) { // a frame ended; another may follow
      ZSTD_decompressBegin(dctx_);
      frameStarted_ = false;
      need = ZSTD_nextSrcSizeToDecompress(dctx_);
    }
    const size_t take = std::min(size, need - pending_.size());
    pending_.insert(pending_.end(), data, data + take);
    data += take;
    size -= take;
    if (pending_.size() == need && !step(error))
      return false;
  }
  return true;
}
std::string ZstdBlockDecoder::save() const {
  if (!atBoundary())
    return {};
  std::string state(ZSTDR_snapshotSize(), '\0');
  if (ZSTDR_save(dctx_, state.data(), state.size()) != 0)
    return {};
  std::string out;
  put(out, SavedMagic);
  put(out, consumed_);
  put(out, produced_);
  put(out, windowSize_);
  put(out, uint64_t(state.size()));
  out += state;
  return out;
}
bool ZstdBlockDecoder::pointOf(const std::string &saved, Point &point) {
  size_t at = 0;
  uint32_t magic = 0;
  uint64_t window = 0, size = 0;
  if (!get(saved, at, magic) || magic != SavedMagic || !get(saved, at, point.consumed) ||
      !get(saved, at, point.produced) || !get(saved, at, window) || !get(saved, at, size) ||
      size != saved.size() - at || window == 0 || window > MaxWindow)
    return false;
  point.window = std::min(window, point.produced);
  return true;
}
std::unique_ptr<ZstdBlockDecoder> ZstdBlockDecoder::restore(const std::string &saved, const uint8_t *window,
                                                            size_t windowSize, Output out, std::string &error) {
  Point point;
  if (!pointOf(saved, point) || windowSize != point.window) {
    error = "zstd resume point";
    return nullptr;
  }
  auto decoder = std::make_unique<ZstdBlockDecoder>(std::move(out));
  size_t at = sizeof(uint32_t) + 3 * sizeof(uint64_t);
  uint64_t window64 = 0;
  std::memcpy(&window64, saved.data() + sizeof(uint32_t) + 2 * sizeof(uint64_t), sizeof(window64));
  at += sizeof(uint64_t);
  decoder->windowSize_ = window64;
  decoder->consumed_ = point.consumed;
  decoder->produced_ = point.produced;
  decoder->frameStarted_ = true;
  if (!decoder->dctx_ || !decoder->ensureRing(error))
    return nullptr;
  // The window goes first in the ring; the next block lands right after it.
  std::memcpy(decoder->ring_.data(), window, windowSize);
  decoder->pos_ = windowSize;
  if (ZSTDR_restore(decoder->dctx_, saved.data() + at, saved.size() - at, decoder->ring_.data(), windowSize) != 0) {
    error = "zstd resume state";
    return nullptr;
  }
  return decoder;
}
} // namespace foil
