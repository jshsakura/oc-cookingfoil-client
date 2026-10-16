#pragma once
#include <string>
namespace foil::ui {
// A HOME Menu shortcut's icon from any picture the server gives (JPEG, PNG,
// WebP): its middle square, scaled to side x side, as a JPEG small enough
// for the control data. Empty when it cannot be read or made.
std::string shortcutIconJpeg(const std::string &image, int side = 256);
} // namespace foil::ui
