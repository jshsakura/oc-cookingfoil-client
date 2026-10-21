#include "Test.hpp"
#include "core/Json.hpp"
#include "core/Labels.hpp"
#include "core/Retro.hpp"
#include "net/Api.hpp"
int runLabelsTests() {
  int failures = 0;
  using namespace foil;
  {
    // The dictionary: every language, tone, icon and plural; bad keys and
    // textless entries dropped, an unknown tone read as neutral.
    Error error;
    auto root = parseJson(R"({"labels":{
      "korean-patch":{"ko":"한글패치","en":"Korean patch","tone":"accent","icon":"globe","order":1},
      "players":{"ko":"{0}인","en":"{0} players","one":{"en":"{0} player"},"tone":"loud"},
      "Bad Key":{"ko":"x"}, "empty":{"tone":"good"}}})", error);
    const auto book = labels::parseBook(root.get());
    CHECK(book.size() == 2 && book.count("korean-patch") && book.count("players"));
    CHECK(book.at("korean-patch").tone == labels::Tone::Accent && book.at("korean-patch").icon == "globe");
    CHECK(book.at("korean-patch").order == 1 && book.at("players").tone == labels::Tone::Neutral);
    CHECK(labels::text(book, {Word("korean-patch"), Word("")}, "ko") == "한글패치");
    CHECK(labels::text(book, {Word("korean-patch"), Word("")}, "ja") == "Korean patch"); // English when missing
    CHECK(labels::text(book, {Word("players"), Word("2")}, "en") == "2 players");
    CHECK(labels::text(book, {Word("players"), Word("1")}, "en") == "1 player");
    CHECK(labels::text(book, {Word("players"), Word("1")}, "ko") == "1인");
    CHECK(labels::text(book, {Word("unknown"), Word("")}, "ko").empty());
    // Merging: a key already there stays.
    labels::Book into{{"players", {{{"en", "mine"}}, {}, labels::Tone::Good, "", 0}}};
    labels::merge(into, book);
    CHECK(into.size() == 2 && into.at("players").text.at("en") == "mine");
  }
  {
    // A game's labels: plain keys and [key, value], in order; a field that
    // is missing says the server words none.
    Error error;
    auto game = parseJson(R"({"labels":["korean",["achievements",40],["year","1995"],"Not A Key",[]]})", error);
    bool present = false;
    const auto refs = labels::parseRefs(game.get(), present);
    CHECK(present && refs.size() == 3 && refs[0].key == "korean" && refs[1].value == "40" && refs[2].value == "1995");
    auto bare = parseJson("{}", error);
    CHECK(labels::parseRefs(bare.get(), present).empty() && !present);
  }
  {
    // Switch: the dictionary rides on the list; a base item carries its keys.
    const auto catalog = net::parseCatalog(R"({"labels":{"dlc":{"ko":"DLC {0}","en":"DLC {0}"}},
      "sections":[{"items":[{"url":"https://h/a.nsp","name":"A","labels":[["dlc","2"]]},
                            {"url":"https://h/b.nsp","name":"B"}]}]})");
    CHECK(!catalog.error && catalog.value.labels.count("dlc"));
    CHECK(catalog.value.items.size() == 2 && catalog.value.items[0].hasLabels && !catalog.value.items[1].hasLabels);
    CHECK(labels::text(catalog.value.labels, catalog.value.items[0].labels.at(0), "ko") == "DLC 2");
  }
  {
    // Retro: a theme row's icon and the reply's dictionary.
    labels::Book book;
    const auto themes = retro::parseThemes(R"({"labels":{"korean":{"ko":"한글"}},"themes":[{"id":"recent","name":"최근 추가","icon":"clock",
      "games":[{"id":"g1","name":"G","system":"snes","bundleUrl":"/api/retro/bundle/g1","labels":["korean"]}]}]})", "https://h", &book);
    CHECK(themes.size() == 1 && themes[0].icon == "clock" && book.count("korean"));
    CHECK(themes[0].games.size() == 1 && themes[0].games[0].hasLabels && themes[0].games[0].labels[0].key == "korean");
    CHECK(retro::mergeThemes({{"https://h", themes}}).at(0).icon == "clock");
  }
  return failures;
}
