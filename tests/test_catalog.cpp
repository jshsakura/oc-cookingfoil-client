#include "Test.hpp"
#include "core/I18n.hpp"
#include "core/Catalog.hpp"
#include <algorithm>
int runCatalogTests() {
  int failures = 0;
  CHECK(foil::thumbnailUrl("https://shop/api/icon/1") == "https://shop/api/icon/1?size=sm");
  CHECK(foil::thumbnailUrl("https://shop/i?id=1") == "https://shop/i?id=1&size=sm");
  CHECK(foil::thumbnailUrl("https://shop/i?size=sm") == "https://shop/i?size=sm");
  CHECK(foil::thumbnailUrl("installed:0100") == "installed:0100");
  CHECK(foil::thumbnailUrl("").empty());
  {
    std::vector<foil::Group> groups(16);
    for (size_t i = 0; i < groups.size(); ++i) {
      groups[i].addedAt = int64_t(i + 1);
      groups[i].representative.name = "g" + std::to_string(100 + i);
      groups[i].representative.languages = {"ko"};
      groups[i].representative.releaseDate = int(20200000 + i % 3);
      groups[i].representative.categories = {i % 2 ? "액션" : "퍼즐"};
      groups[i].base = groups[i].representative;
    }
    groups[7].base.reset(); // update-only leftover
    const auto hero = foil::homeHero(groups);
    CHECK(hero == 15);
    const auto rows = foil::homeThemes(groups, hero);
    CHECK(!rows.empty() && rows[0].theme.title == "최근 추가");
    // 16 games less the hero and the update-only one, under the shelf limit.
    CHECK(rows[0].items.size() == std::min<size_t>(foil::HomeRowLimit, 14));
    CHECK(rows[0].items.front() == 14);
    for (const auto &row : rows)
      for (const auto i : row.items) {
        CHECK(i != hero);
        CHECK(i != 7);
      }
    const auto genre = std::find_if(rows.begin(), rows.end(),
                                    [](const auto &r) { return r.theme.category == "액션"; });
    if (genre != rows.end())
      for (const auto i : genre->items) CHECK(i % 2 == 1);
    // No game shows up in two rows' visible part.
    std::vector<size_t> seen;
    for (const auto &row : rows)
      for (size_t n = 0; n < row.items.size() && n < foil::HomeRowVisible; ++n) {
        CHECK(std::find(seen.begin(), seen.end(), row.items[n]) == seen.end());
        seen.push_back(row.items[n]);
      }
    CHECK(rows.size() >= 2);
    const auto byName = foil::browseCatalog(groups, "", {"", foil::SortMode::Name, foil::Filter::All, "", 0, 0, 0, 0, false, false, "", 0});
    // Popular: lower rank first, unranked left out. Multiplayer: 2+ players.
    groups[3].representative.rank = 5;
    groups[9].representative.rank = 2;
    groups[9].representative.players = 4;
    groups[3].representative.players = 1;
    const auto popular = foil::browseCatalog(groups, "", {"", foil::SortMode::Popular, foil::Filter::All, "", 0, 0, 0, 0, false, false, "", 0});
    CHECK(popular.size() >= 2 && popular[0] == 9 && popular[1] == 3);
    const auto party = foil::browseCatalog(groups, "", {"", foil::SortMode::Popular, foil::Filter::All, "", 2, 0, 0, 0, false, false, "", 0});
    CHECK(!party.empty() && party[0] == 9);
    for (auto &group : groups) {
      group.representative.titleId = "0100000000010000";
      group.base->titleId = group.representative.titleId;
      group.base->size = 2ULL << 30;
    }
    groups[2].base->size = 512ULL << 20;
    const auto chips = foil::libraryChips(groups);
    CHECK(chips.front().title == "전체");
    const auto light = std::find_if(chips.begin(), chips.end(), [](const auto &c) { return c.maxSize; });
    CHECK(light != chips.end());
    if (light != chips.end()) CHECK(foil::browseCatalog(groups, "", *light).size() == 1);
    CHECK(std::none_of(chips.begin(), chips.end(), [](const auto &c) { return c.title == "커스텀"; }));
    CHECK(foil::nextSort(foil::SortMode::Updated) == foil::SortMode::Score &&
        foil::nextSort(foil::SortMode::Score) == foil::SortMode::Default);
    // Filters combine: party games (4+) that are also light.
    groups[2].representative.players = 4;
    groups[4].representative.players = 4;
    const foil::Theme crowd{"", foil::SortMode::Default, foil::Filter::All, "", 4, 0, 0, 0, false, false, "", 0};
    const foil::Theme under1g{"", foil::SortMode::Default, foil::Filter::All, "", 0, 1ULL << 30, 0, 0, false, false, "", 0};
    const std::vector<foil::Theme> crowdOnly{crowd}, crowdLight{crowd, under1g};
    CHECK(foil::browseFiltered(groups, "", crowdOnly, foil::SortMode::Default).size() == 3);
    const auto both = foil::browseFiltered(groups, "", crowdLight, foil::SortMode::Default);
    CHECK(both.size() == 1 && both[0] == 2);
    groups[5].base->size = 30ULL << 30;
    groups[6].base->size = 12ULL << 30;
    const foil::Theme big{"", foil::SortMode::Largest, foil::Filter::All, "", 0, 0, 0, 10ULL << 30, false, false, "", 0};
    const auto large = foil::browseCatalog(groups, "", big);
    CHECK(large.size() == 2 && large[0] == 5 && large[1] == 6);
    // Installed games leave home and the "not installed" filter.
    foil::InstalledMask installed(groups.size(), false);
    installed[14] = true;
    installed[13] = true;
    CHECK(foil::homeHero(groups, installed) == 15);
    installed[15] = true;
    CHECK(foil::homeHero(groups, installed) == 12);
    for (const auto &row : foil::homeThemes(groups, 12, installed))
      for (const auto i : row.items) CHECK(!installed[i]);
    const foil::Theme fresh{"", foil::SortMode::Default, foil::Filter::All, "", 0, 0, 0, 0, true, false, "", 0};
    const std::vector<foil::Theme> freshOnly{fresh};
    const auto notOnConsole = foil::browseFiltered(groups, "", freshOnly, foil::SortMode::Default, installed);
    CHECK(std::none_of(notOnConsole.begin(), notOnConsole.end(), [&](size_t i) { return installed[i]; }));
    CHECK(byName.size() == 15 && byName.front() == 0);
  }
  {
    // Big games rotate with the seed so the row is not the same every launch,
    // but it still only holds games of 10GB or more.
    std::vector<foil::Group> groups(60);
    for (size_t i = 0; i < groups.size(); ++i) {
      groups[i].representative.name = "big" + std::to_string(i);
      groups[i].representative.size = (i < 40 ? 12ULL : 2ULL) << 30;
      groups[i].base = groups[i].representative;
    }
    auto bigRow = [&](unsigned seed) {
      for (const auto &row : foil::homeThemes(groups, 59, {}, {}, seed))
        if (row.theme.title == "대용량 게임")
          return row.items;
      return std::vector<size_t>{};
    };
    const auto fixed = bigRow(0), a = bigRow(1), b = bigRow(2);
    CHECK(fixed == bigRow(0));
    CHECK(a == bigRow(1));
    CHECK(!a.empty() && a != b);
    for (const auto i : a) CHECK(i < 40);
  }
  {
    // Party games are a genre; "2인 이상 플레이" is every other game that
    // takes two or more players on one console (beat 'em ups, co-op).
    // Enough games that the rows above (최근 추가, 인기) leave some over.
    std::vector<foil::Group> groups(60);
    for (size_t i = 0; i < groups.size(); ++i) {
      auto &r = groups[i].representative;
      r.name = "p" + std::to_string(i);
      r.players = i % 2 ? 2 : i % 4 ? 4 : 1;
      r.categories = {i % 3 == 0 ? "Party" : "Action"};
      groups[i].base = r;
    }
    auto row = [&](const std::string &title) {
      for (const auto &r : foil::homeThemes(groups, 59))
        if (r.theme.title == title)
          return r.items;
      return std::vector<size_t>{};
    };
    const auto party = row("파티 게임"), together = row("2인 이상 플레이");
    CHECK(!party.empty() && !together.empty());
    for (const auto i : party) CHECK(groups[i].representative.categories.front() == "Party");
    for (const auto i : together) {
      CHECK(groups[i].representative.players >= 2);
      CHECK(groups[i].representative.categories.front() != "Party");
    }
    for (const auto &r : foil::homeThemes(groups, 59)) CHECK(r.theme.category != "Party" || r.theme.title == "파티 게임");
    // Genre rows keep the key for filtering and show the translated name.
    const foil::GenreNames names{{"Action", "액션"}};
    bool labeled = false;
    for (const auto &r : foil::homeThemes(groups, 59, {}, {}, 0, names))
      if (r.theme.category == "Action") labeled = r.theme.title == "액션";
    CHECK(labeled);
    CHECK(foil::genreLabel(names, "Racing") == "Racing");
  }
  {
    // The hero rotates through several reasons, each a different game.
    std::vector<foil::Group> groups(30);
    for (size_t i = 0; i < groups.size(); ++i) {
      auto &r = groups[i].representative;
      r.name = "h" + std::to_string(i);
      r.rank = int(i + 1);
      r.players = i % 2 ? 2 : 1;
      r.languages = {"ko"};
      groups[i].addedAt = int64_t(i);
      groups[i].base = r;
    }
    groups[3].base->priceDiscount = "1,000원";
    groups[3].representative.priceDiscount = "1,000원";
    const auto slides = foil::homeHeroes(groups, {}, 7);
    CHECK(slides.size() >= 3 && slides.size() <= foil::HeroSlides);
    CHECK(slides.front().index == 29 && slides.front().label == "새로 들어온 게임");
    for (size_t a = 0; a < slides.size(); ++a)
      for (size_t b = a + 1; b < slides.size(); ++b) CHECK(slides[a].index != slides[b].index);
    foil::InstalledMask installed(groups.size(), false);
    installed[29] = true;
    CHECK(foil::homeHeroes(groups, installed).front().index == 28);
  }
  {
    // Review scores: 명작 holds 85+, the score sort puts unrated games last.
    std::vector<foil::Group> groups(40);
    for (size_t i = 0; i < groups.size(); ++i) {
      auto &r = groups[i].representative;
      r.name = "s" + std::to_string(i);
      r.score = i < 10 ? 0 : int(60 + i);
      groups[i].base = r;
    }
    const foil::Theme byScore{"", foil::SortMode::Score, foil::Filter::All, "", 0, 0, 0, 0, false, false, "", 0};
    const auto sorted = foil::browseCatalog(groups, "", byScore);
    CHECK(sorted.size() == 40 && sorted.front() == 39 && groups[sorted.back()].representative.score == 0);
    for (const auto &row : foil::homeThemes(groups, 0))
      if (row.theme.title == "명작")
        for (const auto i : row.items) CHECK(groups[i].representative.score >= 85);
    CHECK(std::string(foil::sortLabel(foil::SortMode::Score)) == "평점순");
  }
  foil::Item base;
  base.url = "https://host/base.nsz";
  base.titleId = "0100E5E01C098000";
  base.baseId = base.titleId;
  base.name = "Lantern Road";
  base.icon = "https://host/icon";
  base.publisher = "Butter Works";
  base.size = 4400000000ULL;
  base.languages = {"ko", "en"};
  auto alternate = base;
  alternate.url = "https://host/base.nsp";
  alternate.size = 6600000000ULL;
  auto unknown = base;
  unknown.url = "https://host/base-unknown.xci";
  unknown.size = 0;
  auto old = base;
  old.url = "https://host/update-v1.nsz";
  old.type = "update";
  old.version = 65536;
  auto update = old;
  update.url = "https://host/update-v2.nsz";
  update.version = 131072;
  update.versionName = "1.6.0";
  auto dlc = base;
  dlc.url = "https://host/dlc.nsp";
  dlc.titleId = "0100E5E01C099001";
  dlc.type = "dlc";
  dlc.name = "Ember Pass";
  dlc.version = 1;
  auto newer = dlc;
  newer.url = "https://host/dlc-new.nsp";
  newer.version = 2;
  auto second = dlc;
  second.url = "https://host/dlc-second.nsp";
  second.titleId = "0100E5E01C099002";
  const std::vector<foil::Item> input{dlc,     old,   alternate, second, update,
                                      unknown, newer, base,      base};
  const auto groups = foil::groupCatalog(input);
  CHECK(groups.size() == 1);
  if (!groups.empty()) {
    const auto &g = groups.front();
    CHECK(g.representative.name == base.name);
    CHECK(g.representative.icon == base.icon);
    CHECK(g.base && g.base->url == base.url);
    CHECK(g.alternatives.size() == 2);
    CHECK(g.update && g.update->version == 131072);
    CHECK(g.dlc.size() == 2);
    CHECK(g.dlc.front().version == 2);
    CHECK(!g.search.empty()); // search covers every file (checked by selectCatalog below)
    CHECK(foil::versionLabel(*g.update) == "v1.6.0");
    CHECK(foil::selectCatalog(groups, "Ember", foil::Filter::All).size() ==
          1);
    CHECK(foil::selectCatalog(groups, "", foil::Filter::Korean).size() == 1);
    CHECK(foil::selectCatalog(groups, "absent", foil::Filter::All).empty());
  }
  CHECK(input.front().url == dlc.url && input.size() == 9);
  CHECK(foil::groupCatalog({}).empty());
  const auto missing = foil::groupCatalog({old, dlc});
  CHECK(missing.size() == 1 && !missing.front().base &&
        missing.front().dlc.size() == 1);
  auto custom = base;
  custom.titleId = "";
  custom.baseId = "";
  custom.url = "https://host/custom.nro";
  auto other = custom;
  other.url = "https://host/other.nro";
  CHECK(foil::groupCatalog({custom, other}).size() == 2);
  auto byTitle = base;
  byTitle.baseId = "";
  CHECK(foil::groupCatalog({byTitle}).front().id == base.titleId);
  CHECK(foil::versionLabel(old) == "v65536");
  auto languageUnknown = base;
  languageUnknown.languages.clear();
  languageUnknown.name = "한국어 이름";
  languageUnknown.region = "KR";
  CHECK(foil::selectCatalog(foil::groupCatalog({languageUnknown}), "",
                            foil::Filter::Korean)
            .empty());
  CHECK(foil::formatBytes(4402341888ULL) == "4.1 GB");
  {
    // Won and yen follow the interface language.
    foil::setLanguage(foil::Language::English);
    CHECK(foil::priceText("42,000\xEC\x9B\x90") == "\xE2\x82\xA9" "42,000");
    CHECK(foil::priceText("4,950\xE5\x86\x86") == "\xC2\xA5" "4,950");
    CHECK(foil::priceText("$24.99") == "$24.99");
    foil::setLanguage(foil::Language::Korean);
    CHECK(foil::priceText("42,000\xEC\x9B\x90") == "42,000\xEC\x9B\x90");
    CHECK(foil::priceText("4,950\xE5\x86\x86") == "4,950\xEC\x97\x94");
  }
  return failures;
}
