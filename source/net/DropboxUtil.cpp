// Ported from oc-save-keeper (MIT), include/network/DropboxUtil.hpp and the
// response parsing in source/network/Dropbox.cpp. Copyright (c) 2026 jshsakura.
#include "net/DropboxUtil.hpp"
#include "core/Json.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::net::dropbox {
namespace {
const char *const Hex = "0123456789ABCDEF";
bool isHexDigit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
unsigned char hexValue(char c) {
  if (c >= '0' && c <= '9')
    return static_cast<unsigned char>(c - '0');
  return static_cast<unsigned char>(10 + (std::tolower(static_cast<unsigned char>(c)) - 'a'));
}
std::string pathRequest(const std::string &path) { return "{\"path\":\"" + escapeJsonString(path) + "\"}"; }
// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
template <class T> Parsed<T> failed(const std::string &message) {
  Parsed<T> result;
  result.error.message = message;
  return result;
}
} // namespace

bool isSuccessfulHttpStatus(long status) { return status >= 200 && status < 300; }

std::string trimCopy(const std::string &value) {
  size_t begin = 0, end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])))
    ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
    --end;
  return value.substr(begin, end - begin);
}

std::string percentEncode(const std::string &value) {
  std::string out;
  out.reserve(value.size() * 3);
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
      out += static_cast<char>(c);
      continue;
    }
    out += '%';
    out += Hex[c >> 4];
    out += Hex[c & 0x0F];
  }
  return out;
}

std::string percentDecode(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '%' && i + 2 < value.size() && isHexDigit(value[i + 1]) && isHexDigit(value[i + 2])) {
      out += static_cast<char>((hexValue(value[i + 1]) << 4) | hexValue(value[i + 2]));
      i += 2;
    } else {
      out += c == '+' ? ' ' : c;
    }
  }
  return out;
}

bool randomBytes(unsigned char *buffer, size_t size) {
#ifdef __SWITCH__
  return R_SUCCEEDED(csrngGetRandomBytes(buffer, size));
#else
  std::random_device device;
  for (size_t i = 0; i < size; ++i)
    buffer[i] = static_cast<unsigned char>(device() & 0xFFu);
  return true;
#endif
}

std::string generateRandomHex(size_t bytes) {
  std::vector<unsigned char> buffer(bytes);
  if (!randomBytes(buffer.data(), bytes))
    return "";
  static const char *lower = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (unsigned char b : buffer) {
    out += lower[b >> 4];
    out += lower[b & 0x0F];
  }
  return out;
}

std::string generateCodeVerifier() {
  std::vector<unsigned char> bytes(32);
  if (!randomBytes(bytes.data(), bytes.size()))
    return "";
  return base64UrlEncode(bytes);
}

std::string base64UrlEncode(const std::vector<unsigned char> &bytes) {
  static const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const unsigned chunk = (unsigned(bytes[i]) << 16) | (unsigned(bytes[i + 1]) << 8) | bytes[i + 2];
    out += table[(chunk >> 18) & 0x3F];
    out += table[(chunk >> 12) & 0x3F];
    out += table[(chunk >> 6) & 0x3F];
    out += table[chunk & 0x3F];
  }
  const size_t rest = bytes.size() - i;
  if (rest > 0) {
    const unsigned b0 = bytes[i], b1 = rest > 1 ? bytes[i + 1] : 0;
    out += table[b0 >> 2];
    out += table[((b0 & 0x03) << 4) | (b1 >> 4)];
    if (rest > 1)
      out += table[(b1 & 0x0F) << 2];
  }
  return out;
}

std::vector<unsigned char> sha256(const std::string &input) {
  static const uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
      0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
      0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
      0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  std::vector<unsigned char> msg(input.begin(), input.end());
  const uint64_t bits = static_cast<uint64_t>(msg.size()) * 8u;
  msg.push_back(0x80u);
  while (msg.size() % 64 != 56)
    msg.push_back(0);
  for (int i = 7; i >= 0; --i)
    msg.push_back(static_cast<unsigned char>(bits >> (i * 8)));
  auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); };
  for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const size_t j = chunk + static_cast<size_t>(i) * 4;
      w[i] = (uint32_t(msg[j]) << 24) | (uint32_t(msg[j + 1]) << 16) | (uint32_t(msg[j + 2]) << 8) | msg[j + 3];
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  std::vector<unsigned char> digest;
  digest.reserve(32);
  for (uint32_t word : h)
    for (int shift = 24; shift >= 0; shift -= 8)
      digest.push_back(static_cast<unsigned char>(word >> shift));
  return digest;
}

std::string buildCodeChallengeS256(const std::string &verifier) { return base64UrlEncode(sha256(verifier)); }

std::time_t parseDropboxTime(const char *value) {
  if (!value || !*value)
    return 0;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  char zone = 0;
  if (std::sscanf(value, "%d-%d-%dT%d:%d:%d%c", &year, &month, &day, &hour, &minute, &second, &zone) != 7 ||
      zone != 'Z')
    return 0;
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
      minute > 59 || second < 0 || second > 60)
    return 0;
  const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  return static_cast<std::time_t>(days * 86400 + hour * 3600 + minute * 60 + second);
}

std::string buildAuthorizeUrl(const std::string &baseUrl, const std::string &clientId,
                              const std::string &redirectUri, const std::string &csrfToken,
                              const std::string &codeChallenge) {
  std::string url = baseUrl + "?client_id=" + percentEncode(clientId) +
                    "&response_type=code&token_access_type=offline&code_challenge_method=S256" +
                    "&code_challenge=" + percentEncode(codeChallenge) + "&state=" + percentEncode(csrfToken);
  if (!redirectUri.empty())
    url += "&redirect_uri=" + percentEncode(redirectUri);
  return url;
}

std::string extractQueryParam(const std::string &input, const std::string &key) {
  const std::string trimmed = trimCopy(input);
  if (trimmed.empty())
    return "";
  const size_t question = trimmed.find('?');
  for (size_t pos = question == std::string::npos ? 0 : question + 1; pos <= trimmed.size();) {
    const size_t end = trimmed.find_first_of("&#", pos);
    const std::string token = trimmed.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    const size_t equals = token.find('=');
    if (equals != std::string::npos && percentDecode(token.substr(0, equals)) == key)
      return percentDecode(token.substr(equals + 1));
    if (end == std::string::npos)
      break;
    pos = end + 1;
  }
  return "";
}

std::string extractAuthorizationCode(const std::string &input) {
  const std::string trimmed = trimCopy(input);
  if (trimmed.empty())
    return "";
  const std::string fromQuery = extractQueryParam(trimmed, "code");
  if (!fromQuery.empty())
    return fromQuery;
  if (trimmed.find('=') != std::string::npos || trimmed.find('&') != std::string::npos)
    return "";
  return percentDecode(trimmed);
}

std::string clientIdFromAuthorizeUrl(const std::string &authorizeUrl) {
  if (authorizeUrl.find('?') == std::string::npos)
    return "";
  return extractQueryParam(authorizeUrl, "client_id");
}

bool isAllowedTokenEndpoint(const std::string &url) { return url == TokenEndpoint; }

std::string escapeJsonString(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
        out += buffer;
      } else {
        out += c;
      }
    }
  }
  return out;
}

std::string buildListFolderRequest(const std::string &path, bool recursive) {
  return "{\"path\":\"" + escapeJsonString(path) + "\",\"recursive\":" + (recursive ? "true" : "false") +
         ",\"include_deleted\":false}";
}
std::string buildUploadArg(const std::string &path) {
  return "{\"path\":\"" + escapeJsonString(path) + "\",\"mode\":\"overwrite\",\"autorename\":false}";
}
std::string buildDownloadArg(const std::string &path) { return pathRequest(path); }
std::string buildCreateFolderRequest(const std::string &path) { return pathRequest(path); }
std::string buildDeleteRequest(const std::string &path) { return pathRequest(path); }

std::string parentPath(const std::string &dropboxPath) {
  const size_t slash = dropboxPath.rfind('/');
  if (slash == std::string::npos)
    return "";
  return slash == 0 ? "/" : dropboxPath.substr(0, slash);
}
std::string fileName(const std::string &dropboxPath) {
  const size_t slash = dropboxPath.rfind('/');
  return slash == std::string::npos ? dropboxPath : dropboxPath.substr(slash + 1);
}
bool hasFileComponent(const std::string &dropboxPath) { return !fileName(dropboxPath).empty(); }

bool constantTimeEqual(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  unsigned diff = 0;
  for (size_t i = 0; i < a.size(); ++i)
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  return diff == 0;
}

bool isUrlPrefixValid(const std::string &url, const std::string &expectedPrefix) {
  if (url.empty() || expectedPrefix.empty() || url.compare(0, expectedPrefix.size(), expectedPrefix) != 0)
    return false;
  if (url.size() == expectedPrefix.size())
    return true;
  const char next = url[expectedPrefix.size()];
  return next == '/' || next == '?' || next == '#';
}

std::string buildSafePollUrl(const std::string &pollUrl, const std::string &bridgeBase,
                             const std::string &sessionId) {
  const std::string prefix = bridgeBase + "/";
  if (!pollUrl.empty() && isUrlPrefixValid(pollUrl, prefix))
    return pollUrl;
  return prefix + "v1/sessions/" + sessionId + "/status";
}

std::string titleFolder(uint64_t titleId) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "/titles/%016llX", static_cast<unsigned long long>(titleId));
  return buffer;
}
std::string latestZipPath(uint64_t titleId) { return titleFolder(titleId) + "/latest.zip"; }
std::string latestMetaPath(uint64_t titleId) { return titleFolder(titleId) + "/latest.meta"; }
std::string revisionsFolder(uint64_t titleId) { return titleFolder(titleId) + "/revisions"; }
bool isSafeRevision(const std::string &revision) {
  return !revision.empty() && revision.size() <= 128 && revision != "." &&
         revision.find("..") == std::string::npos && revision.find_first_of("/\\:") == std::string::npos &&
         std::none_of(revision.begin(), revision.end(),
                      [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}
std::string revisionZipPath(uint64_t titleId, const std::string &revision) {
  return isSafeRevision(revision) ? revisionsFolder(titleId) + "/" + revision + ".zip" : "";
}
std::string revisionMetaPath(uint64_t titleId, const std::string &revision) {
  return isSafeRevision(revision) ? revisionsFolder(titleId) + "/" + revision + ".meta" : "";
}

Parsed<BridgeSession> parseBridgeSession(const std::string &json) {
  Parsed<BridgeSession> result;
  const auto root = parseJson(json, result.error);
  if (result.error)
    return result;
  auto &s = result.value;
  s.sessionId = stringField(root.get(), "session_id");
  s.pollToken = stringField(root.get(), "poll_token");
  s.authorizeUrl = stringField(root.get(), "authorize_url");
  s.pollUrl = stringField(root.get(), "poll_url");
  const auto retry = integerField(root.get(), "retry_after");
  s.retryAfter = retry > 0 && retry < 3600 ? static_cast<int>(retry) : 0;
  if (s.sessionId.empty() || s.pollToken.empty())
    result.error.message = "bridge session without session_id or poll_token";
  return result;
}

std::string startBody(const std::string &codeVerifier) {
  return "{\"code_challenge\":\"" + buildCodeChallengeS256(codeVerifier) + "\",\"code_challenge_method\":\"S256\"}";
}

std::string parseSessionStatus(const std::string &json) {
  Error error;
  const auto root = parseJson(json, error);
  return error ? "failed" : stringField(root.get(), "status", "failed");
}

Parsed<BridgeGrant> parseBridgeGrant(const std::string &json) {
  Parsed<BridgeGrant> result;
  const auto root = parseJson(json, result.error);
  if (result.error)
    return result;
  auto &g = result.value;
  g.authorizationCode = stringField(root.get(), "authorization_code");
  g.state = stringField(root.get(), "state");
  g.redirectUri = stringField(root.get(), "redirect_uri");
  g.tokenEndpoint = stringField(root.get(), "token_endpoint");
  if (g.authorizationCode.empty() || !field(root.get(), "state") ||
      !field(root.get(), "redirect_uri") || g.tokenEndpoint.empty())
    result.error.message = "bridge grant is missing fields";
  else if (!isAllowedTokenEndpoint(g.tokenEndpoint))
    result.error.message = "bridge sent an unexpected token endpoint";
  return result;
}

Parsed<Token> parseToken(const std::string &json, bool requireRefresh) {
  Parsed<Token> result;
  const auto root = parseJson(json, result.error);
  if (result.error)
    return result;
  result.value.accessToken = stringField(root.get(), "access_token");
  result.value.refreshToken = stringField(root.get(), "refresh_token");
  const auto expires = integerField(root.get(), "expires_in");
  result.value.expiresIn = expires > 0 ? expires : 0;
  if (result.value.accessToken.empty())
    result.error.message = "token response without access_token";
  else if (requireRefresh && result.value.refreshToken.empty())
    result.error.message = "token response without refresh_token";
  return result;
}

Parsed<ListPage> parseListFolder(const std::string &json) {
  Parsed<ListPage> result;
  const auto root = parseJson(json, result.error);
  if (result.error)
    return result;
  auto *entries = field(root.get(), "entries");
  if (!json_object_is_type(entries, json_type_array))
    return failed<ListPage>("list_folder response without entries");
  auto &page = result.value;
  page.cursor = stringField(root.get(), "cursor");
  auto *more = field(root.get(), "has_more");
  page.hasMore = json_object_is_type(more, json_type_boolean) && json_object_get_boolean(more);
  const size_t count = json_object_array_length(entries);
  for (size_t i = 0; i < count; ++i) {
    if (page.entries.size() >= MaxListEntries) {
      page.hasMore = false;
      break;
    }
    auto *item = json_object_array_get_idx(entries, i);
    if (!json_object_is_type(item, json_type_object))
      continue;
    Entry entry;
    entry.isFolder = stringField(item, ".tag") == "folder";
    entry.path = stringField(item, "path_display");
    entry.name = stringField(item, "name");
    entry.rev = stringField(item, "rev");
    entry.modified = parseDropboxTime(stringField(item, "server_modified").c_str());
    const auto size = integerField(item, "size");
    entry.size = size > 0 ? static_cast<uint64_t>(size) : 0;
    page.entries.push_back(std::move(entry));
  }
  return result;
}

std::string bridgeUrl(const std::string &server, const std::string &sessionId, const std::string &op) {
  auto base = server;
  while (!base.empty() && base.back() == '/') base.pop_back();
  base += "/api/dropbox/v1/sessions/";
  return sessionId.empty() ? base + op : base + percentEncode(sessionId) + "/" + op;
}

std::string serializeCredentials(const Credentials &c) {
  Json root(json_object_new_object());
  json_object_object_add(root.get(), "access_token", json_object_new_string(c.accessToken.c_str()));
  json_object_object_add(root.get(), "refresh_token", json_object_new_string(c.refreshToken.c_str()));
  json_object_object_add(root.get(), "expires_at", json_object_new_int64(c.expiresAt));
  json_object_object_add(root.get(), "client_id", json_object_new_string(c.clientId.c_str()));
  json_object_object_add(root.get(), "account", json_object_new_string(c.account.c_str()));
  return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
}
Parsed<Credentials> parseCredentials(const std::string &json) {
  Parsed<Credentials> result;
  const auto root = parseJson(json, result.error);
  if (result.error)
    return result;
  auto &c = result.value;
  c.accessToken = stringField(root.get(), "access_token");
  c.refreshToken = stringField(root.get(), "refresh_token");
  c.clientId = stringField(root.get(), "client_id");
  c.account = stringField(root.get(), "account");
  c.expiresAt = std::max<int64_t>(0, integerField(root.get(), "expires_at"));
  if (!c.linked())
    result.error.message = "dropbox.json holds no usable token";
  return result;
}
bool needsRefresh(const Credentials &c, std::time_t now) {
  if (c.accessToken.empty())
    return true;
  return c.expiresAt != 0 && c.expiresAt - int64_t(now) <= 60;
}
Credentials withToken(Credentials c, const Token &token, std::time_t now) {
  c.accessToken = token.accessToken;
  if (!token.refreshToken.empty())
    c.refreshToken = token.refreshToken;
  c.expiresAt = token.expiresIn > 0 ? int64_t(now) + token.expiresIn : 0;
  return c;
}
std::string authorizationCodeBody(const BridgeGrant &grant, const std::string &codeVerifier, const std::string &clientId) {
  return "grant_type=authorization_code&code=" + percentEncode(grant.authorizationCode) +
         "&code_verifier=" + percentEncode(codeVerifier) + "&client_id=" + percentEncode(clientId) +
         "&redirect_uri=" + percentEncode(grant.redirectUri);
}
std::string refreshBody(const std::string &refreshToken, const std::string &clientId) {
  return "grant_type=refresh_token&refresh_token=" + percentEncode(refreshToken) +
         "&client_id=" + percentEncode(clientId);
}
bool isRevoked(long status, const std::string &body) {
  // save-keeper's test: a 400 from the token endpoint naming the grant.
  if (status == 401)
    return true;
  return status == 400 && (body.find("invalid_grant") != std::string::npos ||
                           body.find("invalid or revoked") != std::string::npos ||
                           body.find("expired") != std::string::npos);
}
bool isNotFound(long status, const std::string &body) {
  return status == 409 && body.find("not_found") != std::string::npos;
}
std::string parseAccount(const std::string &json) {
  Error error;
  const auto root = parseJson(json, error);
  if (error)
    return "";
  const auto email = stringField(root.get(), "email");
  return email.empty() ? stringField(field(root.get(), "name"), "display_name") : email;
}

std::map<uint64_t, std::vector<Revision>> revisionsByTitle(const std::vector<Entry> &entries) {
  std::map<uint64_t, std::vector<Revision>> out;
  for (const auto &entry : entries) {
    if (entry.isFolder)
      continue;
    // /titles/<16 hex>/revisions/<name>.zip, matched without regard to case.
    std::string lower = entry.path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    constexpr size_t Prefix = 8, Tid = 16; // "/titles/", the title id
    const std::string middle = "/revisions/", suffix = ".zip";
    if (lower.compare(0, Prefix, "/titles/") != 0 || lower.size() <= Prefix + Tid + middle.size() + suffix.size() ||
        lower.compare(Prefix + Tid, middle.size(), middle) != 0 ||
        lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) != 0)
      continue;
    const auto tid = entry.path.substr(Prefix, Tid);
    if (!std::all_of(tid.begin(), tid.end(), isHexDigit))
      continue;
    const auto start = Prefix + Tid + middle.size();
    const auto name = entry.path.substr(start, entry.path.size() - start - suffix.size());
    if (!isSafeRevision(name) ||
        !std::all_of(name.begin(), name.end(), [](char c) { return c > 0x20 && c < 0x7f; }))
      continue;
    out[std::strtoull(tid.c_str(), nullptr, 16)].push_back({name, entry.modified, entry.size});
  }
  for (auto &[tid, revisions] : out)
    std::sort(revisions.begin(), revisions.end(), [](const Revision &a, const Revision &b) {
      if (a.modified != b.modified) return a.modified > b.modified;
      return a.name > b.name;
    });
  return out;
}
} // namespace foil::net::dropbox
