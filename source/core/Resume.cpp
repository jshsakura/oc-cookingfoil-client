#include "core/Resume.hpp"
#include <cerrno>
#include <cstdlib>
namespace foil {
namespace {
// Digits only, the whole of `text`; nullopt for anything else or overflow.
std::optional<uint64_t> number(const std::string &text) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 20)
    return std::nullopt;
  errno = 0;
  const auto value = std::strtoull(text.c_str(), nullptr, 10);
  if (errno == ERANGE)
    return std::nullopt;
  return uint64_t(value);
}
std::string trim(const std::string &text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return "";
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
} // namespace
std::vector<std::string> rangeHeaders(uint64_t from, const std::string &validator) {
  if (from == 0)
    return {};
  std::vector<std::string> headers{"Range: bytes=" + std::to_string(from) + "-"};
  // A weak validator may not be used with If-Range; without one the server
  // answers 200 on any doubt, which restarts cleanly.
  if (!validator.empty() && validator.rfind("W/", 0) != 0 && validator.find_first_of("\r\n") == std::string::npos)
    headers.push_back("If-Range: " + validator);
  return headers;
}
std::optional<ContentRange> parseContentRange(const std::string &header) {
  const auto text = trim(header);
  if (text.rfind("bytes ", 0) != 0)
    return std::nullopt;
  const auto dash = text.find('-', 6), slash = text.find('/', 6);
  if (dash == std::string::npos || slash == std::string::npos || dash > slash)
    return std::nullopt;
  const auto first = number(text.substr(6, dash - 6)), last = number(text.substr(dash + 1, slash - dash - 1));
  if (!first || !last || *last < *first)
    return std::nullopt;
  ContentRange range{*first, *last, 0};
  const auto total = text.substr(slash + 1);
  if (total != "*") {
    const auto value = number(total);
    if (!value || *value <= *last)
      return std::nullopt;
    range.total = *value;
  }
  return range;
}
RangeReply judgeRange(uint64_t from, long status, const std::string &contentRange) {
  if (status == 200)
    return from == 0 ? RangeReply::Continue : RangeReply::Restart;
  if (status == 206) {
    const auto range = parseContentRange(contentRange);
    if (range && range->first == from)
      return RangeReply::Continue;
    return from == 0 ? RangeReply::Fail : RangeReply::Reject;
  }
  if (status == 416 && from > 0)
    return RangeReply::Reject;
  return RangeReply::Fail;
}
uint64_t declaredSize(long status, const std::string &contentRange, uint64_t contentLength) {
  if (status == 206) {
    const auto range = parseContentRange(contentRange);
    return range ? range->total : 0;
  }
  return contentLength;
}
std::string serializePartInfo(const PartInfo &info) {
  const auto line = [](const char *key, const std::string &value) {
    return value.find_first_of("\r\n") == std::string::npos ? std::string(key) + "=" + value + "\n" : "";
  };
  return line("url", info.url) + line("tag", info.tag) + line("etag", info.etag) +
         line("size", std::to_string(info.size)) + line("offset", std::to_string(info.offset)) +
         (info.plan.empty() ? "" : line("plan", info.plan));
}
PartInfo parsePartInfo(const std::string &text) {
  PartInfo info;
  size_t at = 0;
  while (at < text.size()) {
    auto end = text.find('\n', at);
    if (end == std::string::npos)
      end = text.size();
    const auto line = text.substr(at, end - at);
    at = end + 1;
    const auto equals = line.find('=');
    if (equals == std::string::npos)
      continue;
    const auto key = line.substr(0, equals), value = line.substr(equals + 1);
    if (key == "url")
      info.url = value;
    else if (key == "tag")
      info.tag = value;
    else if (key == "etag")
      info.etag = value;
    else if (key == "plan")
      info.plan = value;
    else if (key == "size" || key == "offset") {
      const auto parsed = number(value);
      if (!parsed)
        return {}; // a damaged sidecar resumes nothing
      (key == "size" ? info.size : info.offset) = *parsed;
    }
  }
  return info;
}
std::string partInfoPath(const std::string &part) { return part + ResumeFile; }
std::string partPlanPath(const std::string &part) { return part + PlanFile; }
bool samePart(const PartInfo &kept, const PartInfo &wanted) {
  if (kept.url.empty() || kept.url != wanted.url || kept.size != wanted.size || kept.tag != wanted.tag)
    return false;
  return wanted.size || !wanted.tag.empty() || !kept.etag.empty();
}
uint64_t partResumeAt(const PartInfo &kept, const PartInfo &wanted, uint64_t have) {
  if (!samePart(kept, wanted) || (wanted.size && have > wanted.size))
    return 0;
  return have;
}
} // namespace foil
