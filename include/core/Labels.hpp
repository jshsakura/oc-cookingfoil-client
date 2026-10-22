#pragma once
// A card's labels as the server words them: a dictionary sent once with a
// list (its "labels" table, every language) and, per game, the keys to show
// in order with an optional value ("achievements" 40 -> 업적 40). The
// client only draws them, so a kind it has never seen still shows.
#include "core/Tags.hpp"
#include <map>
#include <string>
#include <vector>
struct json_object;
namespace foil::labels {
enum class Tone { Neutral, Accent, Good, Warn, Bad };
struct Def {
  // Text per language; {0} takes the game's value. `one` is used instead
  // when that value is "1" (English plurals), per language too.
  std::map<std::string, std::string> text, one;
  Tone tone = Tone::Neutral;
  std::string icon; // a name from the client's icon set; empty for none
  int order = 0;
};
using Book = std::map<std::string, Def>;
struct Ref {
  Word key, value;
};
// The "labels" member of a list's root; keys outside [a-z0-9-] and entries
// without any text are dropped.
Book parseBook(json_object *root);
// A game's "labels": ["korean-patch", ["achievements", "40"]]; at most 8.
// `present` says whether the game had the field at all (an old server's
// games have none, and the client words them itself).
std::vector<Ref> parseRefs(json_object *game, bool &present);
// Books from several servers: a key already there stays.
void merge(Book &into, const Book &from);
// The text for a game's label in `lang` (English when it has none); empty
// when the key is not in the book.
std::string text(const Book &book, const Ref &ref, const std::string &lang);
} // namespace foil::labels
