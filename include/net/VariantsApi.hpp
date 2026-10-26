#pragma once
// Install options a server's plugins offer for this console (feature
// "file-variants"): POST /api/library/variants with the files about to be
// installed; the console's firmware goes in X-CookingFoil-Console. The words
// are the server's, in the language asked.
#include "core/State.hpp"
#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace foil::net {
// A "#RRGGBB" the server gave; unset when it gave none or a wrong one, and
// the app's own color is used.
struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
  bool set = false;
};
Rgb parseRgb(const std::string &text);
struct FileOption {
  std::string id, label, note;
  // The confirm before it installs; an option that gives a plain string has
  // it as the body.
  std::string confirmTitle, confirmBody;
  // What the install says while the server prepares the file, and when it
  // refuses (409) without words of its own.
  std::string progress, failed;
  // Not installable this way: gray, and A shows why.
  bool blocked = false;
  std::string blockedTitle, blockedBody;
  // The server changes the file as it prepares it: its NCAs no longer hash to their ids.
  bool hashesChange = false;
  // How its button and its confirm's card look: the button's fill, stripes
  // (noStripes: plain) and text; the card's fill, text and icon ("warning"
  // or "info"). Unset parts keep the app's look.
  Rgb buttonColor, buttonStripe, buttonText, cardColor, cardText;
  bool noStripes = false;
  std::string cardIcon;
  // Put on the file's address as it is downloaded.
  std::vector<std::pair<std::string, std::string>> query;
};
// {"variants": {"<file as sent>": [option, ...]}}. Options without an id or
// a label are left out; null when the reply is not that shape.
std::optional<std::map<std::string, std::vector<FileOption>>> parseVariants(const std::string &json);
// `url` with the option's query after it ("?" or "&"), each part encoded.
std::string withQuery(const std::string &url, const std::vector<std::pair<std::string, std::string>> &query);
// The options for `files` (download addresses, at most 200), by file; empty
// when the server has none or could not answer.
std::map<std::string, std::vector<FileOption>> fileOptions(const Server &server, const State &state,
                                                           const std::vector<std::string> &files,
                                                           const std::atomic<bool> *cancel);
} // namespace foil::net
