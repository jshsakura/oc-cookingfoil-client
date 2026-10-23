#pragma once
#include "core/I18n.hpp"
#include "core/Types.hpp"
#include <json-c/json.h>
#include <cstdio>
#include <memory>
namespace foil {
struct JsonDelete { void operator()(json_object* p) const { if (p) json_object_put(p); } };
using Json = std::unique_ptr<json_object, JsonDelete>;
Json parseJson(const std::string& text, Error& error);
json_object* field(json_object* obj, const char* key);
std::string stringField(json_object* obj, const char* key, const std::string& fallback = "");
int64_t integerField(json_object* obj, const char* key, int64_t fallback = 0);
// A server path ("/api/...") joined to `base`, or an absolute http(s) URL;
// anything else reads as "".
std::string urlField(json_object* obj, const char* key, const std::string& base);
// An array of addresses (shop paths made full with `base`), at most `most`;
// anything else in it is left out.
std::vector<std::string> urlList(json_object* obj, const char* key, const std::string& base, size_t most);
// An object of language code to text ({"ko": "...", "zh-Hant": "..."}),
// keeping the interface's languages; `keep` may drop a text (a bad URL).
LanguageTexts languageTexts(json_object* obj, const char* key, bool (*keep)(const std::string&) = nullptr);
json_object* languageTextsJson(const LanguageTexts& texts);
std::string readFile(const std::string& path);
// What a Mac (or Windows) leaves on a card it touched: "._name" AppleDouble
// files, .DS_Store, .Spotlight-V100, .Trashes, .fseventsd, Thumbs.db. Never
// an app, a patch or a ROM; scans of the card pass over them.
bool systemJunk(const std::string& name);
bool atomicWrite(const std::string& path, const std::string& contents, std::string& error);
// The console refuses rename over an existing file, which must be removed first.
bool replaceFile(const std::string& from, const std::string& to, std::string& error);
// mkdir -p that stops at a device root ("sdmc:"): std::filesystem::create_directories
// takes "sdmc:" for a folder on the console and fails before making any.
bool makeDirectories(const std::string& path);
// A file being written gets a large buffer: the SD card is slow with the C
// library's small default, which turns a download into 1KB writes.
void writeBuffer(std::FILE* file);
}
