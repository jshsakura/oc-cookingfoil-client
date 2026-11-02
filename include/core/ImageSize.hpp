#pragma once
// A picture's width and height read from its header alone (PNG, JPEG, WebP,
// GIF), so its size is known before anything is decoded: a small download
// can be a picture that unpacks to tens of megabytes.
#include <cstdint>
#include <string>
namespace foil {
// False when the bytes are none of these, or their header is cut or broken.
bool imageSize(const std::string &bytes, uint32_t &width, uint32_t &height);
} // namespace foil
