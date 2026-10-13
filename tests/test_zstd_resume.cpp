#include "Test.hpp"
#include "core/ZstdBlockDecoder.hpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>
using foil::ZstdBlockDecoder;
namespace {
std::vector<uint8_t> readAll(const std::string &path) {
  std::vector<uint8_t> out;
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  std::fseek(f, 0, SEEK_END);
  out.resize(size_t(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  if (!out.empty() && std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
  std::fclose(f);
  return out;
}
// Data that compresses like a game does: runs, repeats far back, and noise.
std::vector<uint8_t> sample(size_t size, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> data;
  data.reserve(size);
  while (data.size() < size) {
    const int kind = int(rng() % 4);
    const size_t len = 64 + rng() % 4096;
    if (kind == 0 && data.size() > 1 << 20) { // a stretch from far back
      const size_t from = rng() % (data.size() - len);
      for (size_t i = 0; i < len && data.size() < size; ++i) data.push_back(data[from + i]);
    } else if (kind == 1) {
      for (size_t i = 0; i < len && data.size() < size; ++i) data.push_back(uint8_t(rng()));
    } else {
      const uint8_t b = uint8_t(rng());
      for (size_t i = 0; i < len && data.size() < size; ++i) data.push_back(uint8_t(b + i % 7));
    }
  }
  return data;
}
std::vector<uint8_t> compress(const std::vector<uint8_t> &plain, const std::string &level, const std::filesystem::path &dir) {
  const auto in = (dir / "plain.bin").string(), out = (dir / "plain.zst").string();
  FILE *f = std::fopen(in.c_str(), "wb");
  std::fwrite(plain.data(), 1, plain.size(), f);
  std::fclose(f);
  const std::string command = "zstd -q -f " + level + " -o '" + out + "' '" + in + "' 2>/dev/null";
  if (std::system(command.c_str()) != 0) return {};
  return readAll(out);
}
} // namespace
int runZstdResumeTests() {
  int failures = 0;
  if (std::system("zstd --version >/dev/null 2>&1") != 0) {
    std::cerr << "zstd resume tests skipped: no zstd command\n";
    return 0;
  }
  // One per run: two test binaries at once must not share plain.zst.
  const auto dir = std::filesystem::temp_directory_path() / ("cookingfoil-zstd-test-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  // NSZ's level (18), a fast one, and a long window past the usual 8 MB.
  for (const std::string level : {"-18", "-3", "--long=25 -19"}) {
    const auto plain = sample(24u << 20, unsigned(level.size()));
    const auto packed = compress(plain, level, dir);
    CHECK(!packed.empty());
    if (packed.empty()) continue;
    // Whole, in odd-sized pieces, keeping every block boundary's state.
    std::vector<uint8_t> whole;
    ZstdBlockDecoder decoder([&](const uint8_t *d, size_t n) { whole.insert(whole.end(), d, d + n); });
    std::vector<std::string> points;
    decoder.onBoundary([&] { points.push_back(decoder.save()); });
    std::mt19937 rng(7);
    std::string error;
    for (size_t at = 0; at < packed.size();) {
      const size_t n = std::min(packed.size() - at, size_t(1 + rng() % 300000));
      CHECK(decoder.write(packed.data() + at, n, error));
      at += n;
    }
    CHECK(whole == plain);
    CHECK(points.size() > 4);
    // Cut off at several of them: a new decoder goes on from each with only
    // the window, and the rest comes out exactly.
    for (size_t k = 1; k < points.size(); k += std::max<size_t>(1, points.size() / 5)) {
      ZstdBlockDecoder::Point point;
      CHECK(ZstdBlockDecoder::pointOf(points[k], point));
      std::vector<uint8_t> rest;
      auto resumed = ZstdBlockDecoder::restore(points[k], plain.data() + point.produced - point.window, point.window,
                                               [&](const uint8_t *d, size_t n) { rest.insert(rest.end(), d, d + n); }, error);
      CHECK(resumed != nullptr);
      if (!resumed) {
        std::cerr << "restore: " << error << '\n';
        continue;
      }
      CHECK(resumed->write(packed.data() + point.consumed, packed.size() - point.consumed, error));
      const bool same = rest.size() == plain.size() - point.produced &&
                        std::equal(rest.begin(), rest.end(), plain.begin() + long(point.produced));
      if (!same) std::cerr << "zstd " << level << " resume at " << point.produced << " differs: " << error << '\n';
      CHECK(same);
    }
    // The decoder hands out its own window: a second pass keeps save() and
    // window() at a spread of boundaries (the ring has wrapped by the later
    // ones), and each restores into a decoder that finishes the stream.
    struct Kept {
      std::string state;
      std::vector<uint8_t> window;
    };
    std::vector<Kept> kept;
    size_t seen = 0;
    ZstdBlockDecoder again([](const uint8_t *, size_t) {});
    again.onBoundary([&] {
      if (seen++ % 37 != 1) return;
      Kept k{again.save(), {}};
      CHECK(again.window(k.window));
      kept.push_back(std::move(k));
    });
    CHECK(again.write(packed.data(), packed.size(), error));
    CHECK(kept.size() > 3);
    std::vector<uint8_t> none;
    CHECK(!again.window(none)); // past the last block: not a boundary
    for (const auto &k : kept) {
      ZstdBlockDecoder::Point point;
      CHECK(ZstdBlockDecoder::pointOf(k.state, point));
      CHECK(k.window.size() == point.window);
      CHECK(std::equal(k.window.begin(), k.window.end(), plain.begin() + long(point.produced - point.window)));
      // Resume from it, and from that resumed decoder resume once more.
      std::vector<uint8_t> rest;
      std::vector<Kept> hop;
      std::unique_ptr<ZstdBlockDecoder> resumed;
      resumed = ZstdBlockDecoder::restore(k.state, k.window.data(), k.window.size(),
                                          [&](const uint8_t *d, size_t n) { rest.insert(rest.end(), d, d + n); }, error);
      CHECK(resumed != nullptr);
      if (!resumed) continue;
      size_t hopSeen = 0;
      resumed->onBoundary([&] {
        if (hopSeen++ != 20) return;
        Kept h{resumed->save(), {}};
        CHECK(resumed->window(h.window));
        hop.push_back(std::move(h));
      });
      CHECK(resumed->write(packed.data() + point.consumed, packed.size() - point.consumed, error));
      CHECK(rest.size() == plain.size() - point.produced &&
            std::equal(rest.begin(), rest.end(), plain.begin() + long(point.produced)));
      for (const auto &h : hop) {
        ZstdBlockDecoder::Point second;
        CHECK(ZstdBlockDecoder::pointOf(h.state, second));
        CHECK(h.window.size() == second.window);
        std::vector<uint8_t> tail;
        auto again2 = ZstdBlockDecoder::restore(h.state, h.window.data(), h.window.size(),
                                                [&](const uint8_t *d, size_t n) { tail.insert(tail.end(), d, d + n); }, error);
        CHECK(again2 && again2->write(packed.data() + second.consumed, packed.size() - second.consumed, error));
        const bool same = tail.size() == plain.size() - second.produced &&
                          std::equal(tail.begin(), tail.end(), plain.begin() + long(second.produced));
        if (!same) std::cerr << "zstd " << level << " second resume at " << second.produced << " differs\n";
        CHECK(same);
      }
    }
    if (points.size() < 2) continue;
    // A damaged block after a resume is caught, not passed on.
    ZstdBlockDecoder::Point point;
    CHECK(ZstdBlockDecoder::pointOf(points[1], point));
    auto bad = ZstdBlockDecoder::restore(points[1], plain.data() + point.produced - point.window, point.window,
                                         [](const uint8_t *, size_t) {}, error);
    auto broken = std::vector<uint8_t>(packed.begin() + long(point.consumed), packed.end());
    for (size_t i = 3; i < broken.size(); i += 4093) broken[i] ^= 0x5a;
    CHECK(bad && !bad->write(broken.data(), broken.size(), error));
  }
  CHECK(ZstdBlockDecoder::restore("not a state", nullptr, 0, [](const uint8_t *, size_t) {}, *new std::string) == nullptr);
  std::filesystem::remove_all(dir);
  return failures;
}
