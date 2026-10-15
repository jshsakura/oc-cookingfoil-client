#include "core/ClientUpdate.hpp"
#include "core/Json.hpp"
#include "net/DropboxUtil.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
namespace foil::update {
namespace {
std::vector<long> parts(const std::string &version) {
  std::vector<long> out;
  const auto end = version.find_first_of("-+");
  const auto core = version.substr(0, end);
  size_t at = 0;
  while (at <= core.size()) {
    const auto dot = core.find('.', at);
    const auto piece = core.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
    out.push_back(std::strtol(piece.c_str(), nullptr, 10));
    if (dot == std::string::npos) break;
    at = dot + 1;
  }
  return out;
}
} // namespace
int compareVersions(const std::string &a, const std::string &b) {
  auto x = parts(a), y = parts(b);
  const auto n = std::max(x.size(), y.size());
  x.resize(n, 0);
  y.resize(n, 0);
  for (size_t i = 0; i < n; ++i)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}
std::optional<Release> parseRelease(const std::string &json, const std::string &base) {
  Error error;
  auto root = parseJson(json, error);
  if (!root || !json_object_is_type(root.get(), json_type_object)) return std::nullopt;
  Release r;
  r.version = stringField(root.get(), "version");
  r.sha256 = stringField(root.get(), "sha256");
  std::transform(r.sha256.begin(), r.sha256.end(), r.sha256.begin(), ::tolower);
  const auto size = integerField(root.get(), "size");
  const auto url = stringField(root.get(), "url");
  if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) r.url = url;
  else if (url.rfind("/", 0) == 0 && url.rfind("//", 0) != 0 && !base.empty()) r.url = base + url;
  constexpr int64_t MaxSize = 256LL << 20;
  if (r.version.empty() || !std::isdigit(static_cast<unsigned char>(r.version[0])) || r.url.empty() ||
      r.sha256.size() != 64 || r.sha256.find_first_not_of("0123456789abcdef") != std::string::npos || size <= 0 ||
      size > MaxSize)
    return std::nullopt;
  r.size = uint64_t(size);
  return r;
}
std::string sha256Hex(const std::string &bytes) {
  static const char *hex = "0123456789abcdef";
  std::string out;
  for (const unsigned char c : net::dropbox::sha256(bytes)) {
    out += hex[c >> 4];
    out += hex[c & 15];
  }
  return out;
}
} // namespace foil::update
