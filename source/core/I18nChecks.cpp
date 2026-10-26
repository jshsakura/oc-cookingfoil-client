#include "core/I18nChecks.hpp"
#include <algorithm>
namespace foil::i18n {
const std::vector<const char *> &lockedKeys() {
  static const std::vector<const char *> keys = {
    // Confirmations that delete or replace, and their buttons.
    "Delete game",
    "Delete game files",
    "Deletes the game, updates and DLC. Saves are kept.",
    "Deletes play data shared by all users.",
    "Deletes this user's play data.",
    "Delete save?",
    "Delete game?",
    "Deletes the save shared by all users. This can't be undone.",
    "Deletes {0}'s save. This can't be undone.",
    "Deletes the game, updates and DLC. User saves are kept.",
    "Delete|confirm",
    "Delete the image cache?",
    "Delete leftover files?",
    "Deletes files no game uses and pieces of cut-off installs. Installed games and saves stay.",
    "Delete this server?",
    "Replaces {0}'s current save with this one. The current save is backed up first and can be restored from Saves.",
    "Removes the Dropbox link from this console. Backups on Dropbox stay there and show again when you reconnect.",
    "Applies to the shared save. The current save is backed up first.",
    "Apply to this account. The current save is backed up first.",
    "Remove",
    "Delete save",
    "Replaces it with the new version. A patch that was on stays on.",
    "Delete the patch",
    "Deletes the downloaded patch files from the SD card.",
    "Replaces {0}'s current save with this backup. The current save is backed up first.",
    "Delete the shortcut?",
    "Delete shortcut",
    "Removes only the HOME Menu shortcut and keeps the app file.",
    "Remove this app?",
    "Removes {0} from the SD card.",
    "Delete folder",
    "Press X to delete the whole {0} folder, settings and saves included.",
    "Removes it from the console and the HOME Menu. User saves are kept.",
    "Delete",
    "Deletes the downloaded files from the SD card.",
    "Delete this patch?",
    // Sign-in fields and what is said about the password.
    "Username",
    "Password",
    "Login password",
    "Enter the username",
    "Enter the password",
    "This address is not encrypted (http), so the password and keys cross the internet as they are. Use the https address if the server has one.",
    "Scan the QR code and tap Allow in Dropbox; this console finishes on its own. Your password is never kept on the console or the server.",
    "2. Scan the QR code with your phone and enter the password.",
    "Enter the password on your phone",
    "The address is http, so someone on the same network could swap the page and see the password.",
    "RetroAchievements refused the password. Enter it again on the phone.",
    "Scan the QR code with your phone's camera and enter the RetroAchievements password there.",
    "The phone locks the password so only this console can open it. The phone has to reach this server's address.",
    "This server cannot take the password from a phone yet.",
    "Enter the id and the password.",
    "Id|account",
    "RetroAchievements password",
    "Sign in",
    "The password signs in once and is kept neither on the console nor on the server.",
    "The server refused the login. Check the server's username and password in Settings.",
    // Device approval, QR codes and phone sign-in steps.
    "For a server that uses device approval, enter only the address and save to go to the QR approval screen; the message goes to the admin with the request.",
    "Scan this QR code from the server website to approve.",
    "Sign in to the shop on your phone, open device approval and scan the code to approve this device for that account.",
    "This download was already used on another console. Scan the QR code to approve this one.",
    "This download has expired. Scan the QR code to approve this console.",
    "This download is no longer valid. Scan the QR code to approve this console.",
    "This account has as many consoles as it can register. Remove one you no longer use, then approve this one.",
    "This account can register up to {0} consoles. Remove one you no longer use, then approve this one.",
    "This account has connected as many consoles by download as it can this hour. Scan the QR code to approve this one.",
    "If you cannot scan it, enter the device code in device approval. Connects automatically once approved.",
    "This shop does not support QR approval. Enter the device code in device approval on the shop website.",
    "Sign in to Dropbox on your phone",
    "2. Scan the QR code with your phone and sign in to Dropbox.",
    "Could not open what the phone sent. Open a new QR code.",
    "The server holds another approval request for this device. Ask the server admin to clear it.",
    "Connects once you approve this device on the server website.",
    "This console is linked to another account. Scan the QR code to approve it.",
    "Approve this console again",
    // Security switches and the installs that rely on them.
    "Made the {0} shortcut. No signature patches were found, so it may fail to open. Install signature patches such as sys-patch first.",
    "No signature patches were found. A shortcut opens only with signature patches installed.",
    "Could not verify the content signature. Turn on Ignore signature check in Settings to install it.",
    "Ignore signature check",
    "Force install|button",
    "Install ignoring firmware version",
    "The server's prod.keys are too old to go on. Replace them with prod.keys for {0} or later.",
    "This game needs firmware {1} or later, but the console has {0}, so it cannot be installed as is.\nThe server has newer prod.keys and can re-encrypt it into a version this console can install.\nThe console must have signature patches: the install relies on them to go ahead.\nIf the game needs features of the newer firmware, it may stop while playing.",
  };
  return keys;
}
bool locked(const std::string &key) {
  const auto &keys = lockedKeys();
  return std::any_of(keys.begin(), keys.end(), [&](const char *k) { return key == k; });
}
std::string englishOf(const std::string &key) { return key.substr(0, key.find('|')); }
std::set<std::string> placeholders(const std::string &text) {
  std::set<std::string> out;
  for (size_t at = text.find('{'); at != std::string::npos; at = text.find('{', at + 1)) {
    const auto end = text.find('}', at);
    if (end != std::string::npos && end - at <= 3) out.insert(text.substr(at, end - at + 1));
  }
  return out;
}
std::vector<uint32_t> codePoints(const std::string &utf8) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < utf8.size();) {
    const unsigned char c = utf8[i];
    const int length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    uint32_t point = length == 1 ? c : c & (0xFF >> (length + 1));
    for (int k = 1; k < length && i + k < utf8.size(); ++k)
      point = point << 6 | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
    i += length;
    out.push_back(point);
  }
  return out;
}
bool undrawable(uint32_t p) {
  return (p >= 0x0590 && p <= 0x08FF) || // Hebrew, Arabic, Syriac, Thaana, N'Ko
         (p >= 0x0900 && p <= 0x0DFF) || // Indic scripts and Sinhala
         (p >= 0x0E00 && p <= 0x0FFF) || // Thai, Lao, Tibetan
         (p >= 0x1000 && p <= 0x109F) || // Myanmar
         (p >= 0x1100 && p <= 0x11FF) || // Hangul jamo that combine
         (p >= 0x1780 && p <= 0x17FF) || // Khmer
         (p >= 0xFB1D && p <= 0xFDFF) || (p >= 0xFE70 && p <= 0xFEFF); // their presentation forms
}
namespace {
bool control(uint32_t p) {
  return (p < 0x20 && p != '\n') || p == 0x7F || (p >= 0x200E && p <= 0x200F) ||
         (p >= 0x202A && p <= 0x202E) || (p >= 0x2066 && p <= 0x2069);
}
} // namespace
Problem checkText(const std::string &key, const std::string &text) {
  constexpr size_t Stretch = 4, Floor = 16;
  if (text.empty()) return Problem::Empty;
  const auto points = codePoints(text);
  if (std::any_of(points.begin(), points.end(), control)) return Problem::Control;
  const auto shown = englishOf(key);
  if (placeholders(text) != placeholders(shown)) return Problem::Placeholders;
  if (points.size() > std::max(Floor, Stretch * codePoints(shown).size())) return Problem::TooLong;
  return Problem::None;
}
const char *problemName(Problem problem) {
  switch (problem) {
  case Problem::None: return "ok";
  case Problem::Empty: return "empty";
  case Problem::Control: return "control-character";
  case Problem::Placeholders: return "placeholders";
  case Problem::TooLong: return "too-long";
  }
  return "?";
}
} // namespace foil::i18n
