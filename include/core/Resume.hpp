#pragma once
// The decisions behind resuming a download, kept free of the network so the
// host tests can drive them: what to ask for, what a reply means, and whether
// a kept partial download still belongs to what is asked now. Every offset is
// 64-bit: a patch stream runs far past 4 GiB.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace foil {
// Range (and If-Range with a strong validator) for a request continuing at
// `from`; nothing at 0.
std::vector<std::string> rangeHeaders(uint64_t from, const std::string &validator);
// A Content-Range header ("bytes 100-199/200"): first and last byte, and the
// whole length (0 when the server sends "*").
struct ContentRange {
  uint64_t first = 0, last = 0, total = 0;
};
std::optional<ContentRange> parseContentRange(const std::string &header);
enum class RangeReply {
  Continue, // 206 starting at `from`, or 200 to a request from 0
  Restart,  // 200 to a ranged request: the whole body again, from 0
  Reject,   // 206 starting elsewhere, or 416: ask again from 0
  Fail,     // anything else: an error for the caller
};
RangeReply judgeRange(uint64_t from, long status, const std::string &contentRange);
// The whole length a taken reply declares: Content-Range's total for a 206,
// else Content-Length; 0 when unknown.
uint64_t declaredSize(long status, const std::string &contentRange, uint64_t contentLength);

// What a kept partial download was for, saved beside it as a sidecar.
struct PartInfo {
  std::string url;
  std::string tag;  // the expected hash, or a version, when known
  std::string etag; // the server's validator, sent back as If-Range
  uint64_t size = 0;   // expected whole size, 0 when unknown
  uint64_t offset = 0; // a tar's last whole entry; a file resumes at its own size
  std::string plan;    // a file fetched in parts: where each stood (net::SegmentPlan::save)
};
std::string serializePartInfo(const PartInfo &info);
PartInfo parsePartInfo(const std::string &text);
// The sidecar of a part file (or of a staging folder: <dir>/.resume).
std::string partInfoPath(const std::string &part);
constexpr const char *ResumeFile = ".resume";
// The sidecar of a part fetched in parts that stopped short. Its own file:
// the part is already its full size, and a build that knows no plans reads
// a full-size part beside a .resume as a finished file.
std::string partPlanPath(const std::string &part);
constexpr const char *PlanFile = ".plan";
// Whether `kept` is the same download as `wanted`: same address, size and
// tag, and something to tell a changed file by (a size, a tag or a
// validator). An unidentifiable part is never resumed.
bool samePart(const PartInfo &kept, const PartInfo &wanted);
// Where a kept file of `have` bytes resumes: `have` when it is the same
// download and not past the expected size, else 0 (start over).
uint64_t partResumeAt(const PartInfo &kept, const PartInfo &wanted, uint64_t have);
} // namespace foil
