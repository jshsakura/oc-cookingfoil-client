#include "core/NczPoint.hpp"
#include <algorithm>
#include <cstring>
#include <utility>
namespace foil {
namespace {
constexpr const char *Prefix = "ncz1:";
constexpr size_t PrefixSize = 5;
constexpr const char *Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
// Bounds no real point reaches: a saved decoder is about 27 KB, a hash
// midstate well under 256 bytes, and NSZ's window 8 MB (the decoder's own
// limit is 128 MB).
constexpr size_t MaxDecoder = 1u << 16;
constexpr size_t MaxSha = 256;
constexpr uint64_t MaxWindow = 1ull << 27;
constexpr const char *WindowExt = ".win";

void putLe(std::string &out, uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i)
    out.push_back(char((value >> (8 * i)) & 0xFF));
}
bool getLe(const std::string &in, size_t &at, int bytes, uint64_t &value) {
  if (in.size() - at < size_t(bytes))
    return false;
  value = 0;
  for (int i = 0; i < bytes; ++i)
    value |= uint64_t(uint8_t(in[at + size_t(i)])) << (8 * i);
  at += size_t(bytes);
  return true;
}
// A length-prefixed run of bytes, at most `limit` long.
bool getBytes(const std::string &in, size_t &at, size_t limit, std::string &out) {
  uint64_t size = 0;
  if (!getLe(in, at, 4, size) || size > limit || in.size() - at < size)
    return false;
  out.assign(in, at, size_t(size));
  at += size_t(size);
  return true;
}

std::string toBase64(const std::string &in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    const size_t n = std::min<size_t>(3, in.size() - i);
    uint32_t v = uint32_t(uint8_t(in[i])) << 16;
    if (n > 1)
      v |= uint32_t(uint8_t(in[i + 1])) << 8;
    if (n > 2)
      v |= uint8_t(in[i + 2]);
    out.push_back(Alphabet[(v >> 18) & 63]);
    out.push_back(Alphabet[(v >> 12) & 63]);
    out.push_back(n > 1 ? Alphabet[(v >> 6) & 63] : '=');
    out.push_back(n > 2 ? Alphabet[v & 63] : '=');
  }
  return out;
}
int sextet(char c) {
  const char *at = c ? std::strchr(Alphabet, c) : nullptr;
  return at ? int(at - Alphabet) : -1;
}
// Strict: whole groups of four, padding only at the very end, and no stray
// bits under it (so one text means one byte string).
bool fromBase64(const std::string &in, size_t from, std::string &out) {
  const size_t size = in.size() - from;
  if (size % 4)
    return false;
  out.clear();
  out.reserve(size / 4 * 3);
  for (size_t i = from; i < in.size(); i += 4) {
    const bool last = i + 4 == in.size();
    const size_t pad = last ? (in[i + 3] == '=') + (in[i + 2] == '=' && in[i + 3] == '=') : 0;
    uint32_t v = 0;
    for (size_t j = 0; j < 4 - pad; ++j) {
      const int s = sextet(in[i + j]);
      if (s < 0)
        return false;
      v |= uint32_t(s) << (18 - 6 * j);
    }
    if ((pad == 1 && (v & 0xFF)) || (pad == 2 && (v & 0xFFFF)))
      return false;
    out.push_back(char(v >> 16));
    if (pad < 2)
      out.push_back(char((v >> 8) & 0xFF));
    if (pad < 1)
      out.push_back(char(v & 0xFF));
  }
  return true;
}

bool plain(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
int hexDigit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}
} // namespace

std::string NczPoint::encode() const {
  std::string bin;
  putLe(bin, version, 4);
  putLe(bin, zstdStart, 8);
  putLe(bin, consumed, 8);
  putLe(bin, produced, 8);
  putLe(bin, windowSize, 8);
  putLe(bin, decoder.size(), 4);
  bin += decoder;
  putLe(bin, sha.size(), 4);
  bin += sha;
  return Prefix + toBase64(bin);
}
bool NczPoint::isNcz(const std::string &plan) { return plan.compare(0, PrefixSize, Prefix) == 0; }
bool NczPoint::decode(const std::string &text, NczPoint &point) {
  std::string bin;
  if (!isNcz(text) || !fromBase64(text, PrefixSize, bin))
    return false;
  NczPoint p;
  size_t at = 0;
  uint64_t version = 0;
  if (!getLe(bin, at, 4, version) || version != CurrentVersion || !getLe(bin, at, 8, p.zstdStart) ||
      !getLe(bin, at, 8, p.consumed) || !getLe(bin, at, 8, p.produced) || !getLe(bin, at, 8, p.windowSize) ||
      !getBytes(bin, at, MaxDecoder, p.decoder) || !getBytes(bin, at, MaxSha, p.sha) || at != bin.size())
    return false;
  if (p.windowSize == 0 || p.windowSize > MaxWindow)
    return false;
  p.version = uint32_t(version);
  point = std::move(p);
  return true;
}

// Letters and digits stay; anything else becomes "_" and two hex digits, so
// '-' (which ends the key) and '.' never appear in the escaped key.
std::string windowFileName(const std::string &key, uint64_t produced) {
  static const char *Hex = "0123456789abcdef";
  std::string name;
  for (const char c : key) {
    if (plain(c)) {
      name.push_back(c);
    } else {
      name.push_back('_');
      name.push_back(Hex[uint8_t(c) >> 4]);
      name.push_back(Hex[uint8_t(c) & 15]);
    }
  }
  return name + "-" + std::to_string(produced) + WindowExt;
}
bool windowFileOf(const std::string &name, std::string &key, uint64_t &produced) {
  const size_t ext = std::strlen(WindowExt);
  if (name.size() <= ext || name.compare(name.size() - ext, ext, WindowExt) != 0)
    return false;
  const std::string stem = name.substr(0, name.size() - ext);
  const size_t dash = stem.rfind('-');
  if (dash == std::string::npos || dash == 0)
    return false;
  const std::string digits = stem.substr(dash + 1);
  if (digits.empty() || digits.size() > 20 || digits.find_first_not_of("0123456789") != std::string::npos)
    return false;
  uint64_t n = 0;
  for (const char c : digits) {
    const uint64_t d = uint64_t(c - '0');
    if (n > (~uint64_t(0) - d) / 10)
      return false;
    n = n * 10 + d;
  }
  std::string k;
  for (size_t i = 0; i < dash; ++i) {
    if (plain(stem[i])) {
      k.push_back(stem[i]);
      continue;
    }
    if (stem[i] != '_' || i + 2 >= dash)
      return false;
    const int hi = hexDigit(stem[i + 1]), lo = hexDigit(stem[i + 2]);
    if (hi < 0 || lo < 0 || plain(char(hi * 16 + lo)))
      return false;
    k.push_back(char(hi * 16 + lo));
    i += 2;
  }
  key = std::move(k);
  produced = n;
  return true;
}
} // namespace foil
