#include "core/ImageSize.hpp"
namespace foil {
namespace {
uint32_t be16(const std::string &b, size_t at) { return uint32_t(uint8_t(b[at])) << 8 | uint8_t(b[at + 1]); }
uint32_t be32(const std::string &b, size_t at) { return be16(b, at) << 16 | be16(b, at + 2); }
uint32_t le16(const std::string &b, size_t at) { return uint32_t(uint8_t(b[at])) | uint32_t(uint8_t(b[at + 1])) << 8; }
uint32_t le24(const std::string &b, size_t at) { return le16(b, at) | uint32_t(uint8_t(b[at + 2])) << 16; }
bool jpeg(const std::string &b, uint32_t &w, uint32_t &h) {
  size_t at = 2;
  // Marker by marker to the frame header (SOF0..SOF15 but DHT, JPG, DAC).
  while (at + 4 <= b.size()) {
    if (uint8_t(b[at]) != 0xFF) return false;
    const uint8_t marker = uint8_t(b[at + 1]);
    if (marker == 0xFF) {
      ++at;
      continue;
    }
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      at += 2;
      continue;
    }
    const uint32_t length = be16(b, at + 2);
    if (length < 2) return false;
    if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
      if (length < 7 || b.size() - at < 9) return false;
      h = be16(b, at + 5);
      w = be16(b, at + 7);
      return w && h;
    }
    if (marker == 0xD9 || marker == 0xDA) return false; // no frame header before the image data
    if (b.size() - at - 2 < length) return false;
    at += 2 + length;
  }
  return false;
}
} // namespace
bool imageSize(const std::string &b, uint32_t &w, uint32_t &h) {
  w = h = 0;
  if (b.size() >= 24 && b.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0 && b.compare(12, 4, "IHDR") == 0) {
    w = be32(b, 16);
    h = be32(b, 20);
    return w && h;
  }
  if (b.size() >= 4 && uint8_t(b[0]) == 0xFF && uint8_t(b[1]) == 0xD8) return jpeg(b, w, h);
  if (b.size() >= 10 && (b.compare(0, 6, "GIF87a") == 0 || b.compare(0, 6, "GIF89a") == 0)) {
    w = le16(b, 6);
    h = le16(b, 8);
    return w && h;
  }
  if (b.size() >= 30 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) {
    if (b.compare(12, 4, "VP8 ") == 0) { // lossy: the key frame's 14-bit sizes
      w = le16(b, 26) & 0x3FFF;
      h = le16(b, 28) & 0x3FFF;
    } else if (b.compare(12, 4, "VP8L") == 0) {
      const uint32_t bits = le16(b, 21) | le16(b, 23) << 16;
      w = (bits & 0x3FFF) + 1;
      h = ((bits >> 14) & 0x3FFF) + 1;
    } else if (b.compare(12, 4, "VP8X") == 0) {
      w = le24(b, 24) + 1;
      h = le24(b, 27) + 1;
    }
    return w && h;
  }
  return false;
}
} // namespace foil
