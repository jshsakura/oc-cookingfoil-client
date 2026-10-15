#pragma once
// Where a solid NCZ download stands (one zstd frame per NCA), kept as text in
// the download's plan so it can go on from the last block boundary instead of
// starting the NCA over: how far into the compressed stream it got, how much
// it wrote, the decoder's saved state and the hash's midstate. The decoded
// window restore() needs is too big for a plan; it goes in a file of its own
// beside the download (windowFileName).
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace foil {
struct NczPoint {
  static constexpr uint32_t CurrentVersion = 1;
  uint32_t version = CurrentVersion;
  uint64_t zstdStart = 0;  // where the zstd stream begins in the NCA file
  uint64_t consumed = 0;   // compressed bytes taken, from zstdStart
  uint64_t produced = 0;   // decoded bytes written, from the body's start
  uint64_t windowSize = 0; // the frame's window
  std::string decoder;     // ZstdBlockDecoder::save(), opaque
  std::string sha;         // the SHA-256 midstate, opaque
  // "ncz1:" and base64 of the fields, little-endian.
  std::string encode() const;
  // False on anything this build did not write: another prefix or version,
  // bad base64, a short or overlong body, sizes no real point has.
  static bool decode(const std::string &text, NczPoint &point);
  // Whether a plan string is one of these (and not a segmented plan).
  static bool isNcz(const std::string &plan);
};
// The file holding a point's window, named for the download's key ("sd:0123…")
// and the decoded position the window ends at: only letters, digits, '_', '-'
// and '.', ending ".win". windowFileOf reads one back; false for any other
// name.
std::string windowFileName(const std::string &key, uint64_t produced);
bool windowFileOf(const std::string &name, std::string &key, uint64_t &produced);
// The console is the truth and a resume point only a note of it: a point
// whose placeholder is gone goes (nothing to resume into), and so does a
// window file no point is left to own. `placeholderThere` answers nothing
// when it could not look, and then the point stays.
struct ResumeLeftovers {
  std::vector<std::string> points;  // keys to forget
  std::vector<std::string> windows; // window file names to delete
};
ResumeLeftovers resumeLeftovers(const std::vector<std::string> &points,
                                const std::function<std::optional<bool>(const std::string &key)> &placeholderThere,
                                const std::vector<std::string> &windowNames);
} // namespace foil
