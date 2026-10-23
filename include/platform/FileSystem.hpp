#pragma once
#include <string>
#include <system_error>

namespace foil::platform {
// Remove a file (including a Switch concatenation file) or a whole directory.
// Missing is success; a successful delete is checked before returning.
bool removePath(const std::string &path, std::error_code &error);
// Rename without changing the platform's overwrite rules. Native FS errors
// retain their Result code and compare to the matching std::errc, if known.
bool renamePath(const std::string &from, const std::string &to, std::error_code &error);
}
