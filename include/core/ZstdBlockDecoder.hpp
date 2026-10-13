#pragma once
// A zstd stream decoded one block at a time (zstd's buffer-less API), so it
// can stop between two blocks and go on from there later: save() at a block
// boundary, then restore() with the last stretch of output (the window, read
// back from where it was written). A compressed NCA cut off part way picks up
// at the block it reached instead of starting over.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
namespace foil {
class ZstdBlockDecoder {
public:
  using Output = std::function<void(const uint8_t *data, size_t size)>;
  explicit ZstdBlockDecoder(Output out);
  ~ZstdBlockDecoder();
  ZstdBlockDecoder(const ZstdBlockDecoder &) = delete;
  ZstdBlockDecoder &operator=(const ZstdBlockDecoder &) = delete;
  // Compressed bytes in; decoded bytes to `out`, in order. False (with the
  // reason) on data zstd cannot decode.
  bool write(const uint8_t *data, size_t size, std::string &error);
  uint64_t consumed() const { return consumed_ + pending_.size(); } // compressed bytes taken
  uint64_t produced() const { return produced_; }                   // decoded bytes handed out
  // Between two blocks with nothing held back: a point save() can keep.
  bool atBoundary() const;
  // Called at each such point as write() passes it (save() works inside).
  void onBoundary(std::function<void()> boundary) { boundary_ = std::move(boundary); }
  // The decoder's state at this boundary (empty when not at one), with the
  // compressed and decoded positions it stands at.
  std::string save() const;
  // Where a saved state stands: compressed bytes to skip, decoded bytes
  // already written, and how much of the last output restore() needs.
  struct Point {
    uint64_t consumed = 0, produced = 0, window = 0;
  };
  static bool pointOf(const std::string &saved, Point &point);
  // The last point.window bytes of output at this boundary, copied out of the
  // ring in order: what restore() takes. False when not at a boundary, or
  // when the window reaches back past this frame's start.
  bool window(std::vector<uint8_t> &out) const;
  // A decoder at a saved point. `window` holds the last point.window bytes of
  // output (the decoded bytes just before point.produced).
  static std::unique_ptr<ZstdBlockDecoder> restore(const std::string &saved, const uint8_t *window, size_t windowSize,
                                                   Output out, std::string &error);

private:
  bool step(std::string &error);
  bool ensureRing(std::string &error);
  Output out_;
  std::function<void()> boundary_;
  ZSTD_DCtx *dctx_ = nullptr;
  std::vector<uint8_t> pending_; // input gathered for the next step
  std::vector<uint8_t> header_;  // the frame header, read for its window size
  std::vector<uint8_t> ring_;    // output, kept for the window blocks refer back to
  size_t pos_ = 0;               // where the next block's output goes in ring_
  size_t lapEnd_ = 0;            // where the lap before ended (0 until the ring wraps)
  uint64_t windowSize_ = 0, consumed_ = 0, produced_ = 0;
  bool frameStarted_ = false;
};
} // namespace foil
