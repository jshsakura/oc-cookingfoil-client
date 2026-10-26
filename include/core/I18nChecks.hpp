#pragma once
// Screen text that comes from a server's dictionary rather than the tables in
// source/i18n/: what it may not reword, and the checks a text passes before
// it stands for a key. tools/strings-check runs the same checks on a
// dictionary file, so the server's CI refuses what the console would.
#include <cstdint>
#include <set>
#include <string>
#include <vector>
namespace foil::i18n {
// Keys a server may not reword in a language the app ships: confirmations
// that delete or replace, sign-in and password text, device approval and QR
// steps, and the security switches.
const std::vector<const char *> &lockedKeys();
bool locked(const std::string &key);
// What a key shows in English ("Name|sort" -> "Name"), for any string:
// english() takes only the app's literal keys.
std::string englishOf(const std::string &key);
// The {0} {1} ... a text fills in.
std::set<std::string> placeholders(const std::string &text);
enum class Problem { None, Empty, Control, Placeholders, TooLong };
// Whether `text` can stand for `key`: not empty, no control characters but
// line breaks (nor the ones that turn text right to left), the key's
// placeholders exactly, and at most four times the English length (at least
// 16 characters), so a stray paragraph does not take a button's place.
Problem checkText(const std::string &key, const std::string &text);
const char *problemName(Problem problem);
std::vector<uint32_t> codePoints(const std::string &utf8);
// Scripts the console cannot draw whatever the font: right-to-left ones
// (there is no bidi) and those whose letters join or reorder (no shaping).
bool undrawable(uint32_t point);
} // namespace foil::i18n
