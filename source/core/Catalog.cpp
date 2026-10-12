#include "core/Catalog.hpp"
#include "core/I18n.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <set>
namespace foil {
namespace {
// Party games get their own row and chip. Genres arrive as English keys.
const std::string PartyGenre = "Party";
// Review score floors: the 명작 row and the "평점 80 이상" chip.
constexpr int GreatScore = 85, GoodScore = 80;
bool smaller(const Item &a, const Item &b) {
  const auto as = a.size ? a.size : UINT64_MAX,
             bs = b.size ? b.size : UINT64_MAX;
  return as == bs ? a.url < b.url : as < bs;
}
bool newer(const Item &a, const Item &b) {
  return a.version == b.version ? smaller(a, b) : a.version > b.version;
}
std::string lower(std::string text) {
  for (auto &c : text)
    if (static_cast<unsigned char>(c) < 128)
      c = std::tolower(static_cast<unsigned char>(c));
  return text;
}
} // namespace
std::vector<Group> groupCatalog(const std::vector<Item> &items) {
  std::vector<Group> result;
  std::vector<std::vector<Item>> filesOf; // per group, only while grouping
  std::map<std::string, size_t> byId;
  std::set<std::string> urls;
  for (const auto &item : items) {
    if (item.url.empty() || !urls.insert(item.url).second)
      continue;
    const auto id = !item.baseId.empty()    ? item.baseId
                    : !item.titleId.empty() ? item.titleId
                                            : item.url;
    if (!byId.count(id)) {
      byId[id] = result.size();
      Group group;
      group.id = id;
      result.push_back(std::move(group));
      filesOf.emplace_back();
    }
    const auto at = byId[id];
    filesOf[at].push_back(item);
    result[at].addedAt = std::max(result[at].addedAt, item.addedAt);
  }
  for (size_t n = 0; n < result.size(); ++n) {
    auto &g = result[n];
    const auto &files = filesOf[n];
    std::vector<Item> bases, updates;
    std::map<std::string, Item> dlc;
    for (const auto &file : files) {
      const auto line = lower(file.name + " " + file.titleId + " " + file.publisher);
      if (g.search.find(line) == std::string::npos) g.search += line + "\n";
      if (file.type == "base")
        bases.push_back(file);
      else if (file.type == "update")
        updates.push_back(file);
      else if (file.type == "dlc") {
        const auto key = file.titleId.empty() ? file.url : file.titleId;
        const auto found = dlc.find(key);
        if (found == dlc.end() || newer(file, found->second))
          dlc[key] = file;
      }
    }
    std::sort(bases.begin(), bases.end(), smaller);
    std::sort(updates.begin(), updates.end(), newer);
    if (!bases.empty()) {
      g.base = bases.front();
      g.alternatives.assign(bases.begin() + 1, bases.end());
    }
    if (!updates.empty())
      g.update = updates.front();
    for (const auto &pair : dlc)
      g.dlc.push_back(pair.second);
    g.representative = g.base ? *g.base : files.front();
    std::vector<Item>().swap(filesOf[n]); // free as it goes
  }
  return result;
}
std::string thumbnailUrl(const std::string &url, const char *size) {
  if (url.empty() || url.find("size=") != std::string::npos ||
      url.rfind("http", 0) != 0)
    return url;
  return url + (url.find('?') == std::string::npos ? "?" : "&") + "size=" + size;
}
const char *sortLabel(SortMode sort) {
  return sort == SortMode::Recent    ? tr("최근 추가순")
         : sort == SortMode::Release ? tr("출시일순")
         : sort == SortMode::Name    ? tr("이름순")
         : sort == SortMode::Popular ? tr("인기순")
         : sort == SortMode::Largest ? tr("용량 큰순")
         : sort == SortMode::Updated ? tr("업데이트순")
         : sort == SortMode::Score   ? tr("평점순")
                                     : tr("기본 순서");
}
namespace {
bool passes(const Group &g, const Theme &t) {
  const auto &r = g.representative;
  const auto &lang = r.languages;
  const auto &genres = r.categories;
  if (t.filter == Filter::Korean && std::find(lang.begin(), lang.end(), "ko") == lang.end())
    return false;
  if ((t.filter == Filter::Update && !g.update) || (t.filter == Filter::Dlc && g.dlc.empty()) ||
      (t.filter == Filter::Custom && !r.titleId.empty()))
    return false;
  if (r.players < t.minPlayers || (t.maxPlayers && r.players > t.maxPlayers) ||
      (t.maxSize && g.base && g.base->size > t.maxSize) ||
      (t.minSize && (!g.base || g.base->size < t.minSize)))
    return false;
  if (t.minScore && r.score < t.minScore)
    return false;
  if (t.onSale && (!g.base || g.base->priceDiscount.empty()))
    return false;
  if (!t.withoutCategory.empty() &&
      std::find(genres.begin(), genres.end(), t.withoutCategory) != genres.end())
    return false;
  return t.category.empty() || std::find(genres.begin(), genres.end(), t.category) != genres.end();
}
} // namespace
bool textMatches(const std::vector<std::string> &texts, const std::string &query) {
  if (query.empty()) return true;
  const auto needle = lower(query);
  return std::any_of(texts.begin(), texts.end(),
                     [&](const std::string &text) { return lower(text).find(needle) != std::string::npos; });
}
std::vector<size_t> browseFiltered(const std::vector<Group> &groups, const std::string &query,
                                   const std::vector<Theme> &filters, SortMode sort,
                                   const InstalledMask &installed) {
  std::vector<size_t> list;
  const bool hideInstalled = std::any_of(filters.begin(), filters.end(),
                                         [](const Theme &t) { return t.notInstalled; });
  for (const auto i : selectCatalog(groups, query, Filter::All)) {
    const auto &g = groups[i];
    if (hideInstalled && i < installed.size() && installed[i])
      continue;
    if (!g.base || (sort == SortMode::Release && !g.representative.releaseDate) ||
        (sort == SortMode::Updated && (!g.update || !g.update->addedAt)) ||
        !std::all_of(filters.begin(), filters.end(), [&](const Theme &t) { return passes(g, t); }))
      continue;
    list.push_back(i);
  }
  std::stable_sort(list.begin(), list.end(), [&](size_t a, size_t b) {
    const auto &x = groups[a], &y = groups[b];
    if (sort == SortMode::Recent && x.addedAt != y.addedAt)
      return x.addedAt > y.addedAt;
    if (sort == SortMode::Release && x.representative.releaseDate != y.representative.releaseDate)
      return x.representative.releaseDate > y.representative.releaseDate;
    // Popular: ranked titles first (1 is top), unranked keep their order after.
    if (sort == SortMode::Popular && x.representative.rank != y.representative.rank) {
      if (!x.representative.rank || !y.representative.rank)
        return x.representative.rank != 0;
      return x.representative.rank < y.representative.rank;
    }
    // Score: rated titles first, highest on top; unrated keep their order after.
    if (sort == SortMode::Score && x.representative.score != y.representative.score)
      return x.representative.score > y.representative.score;
    if (sort == SortMode::Updated && x.update->addedAt != y.update->addedAt)
      return x.update->addedAt > y.update->addedAt;
    if (sort == SortMode::Largest && x.base->size != y.base->size)
      return x.base->size > y.base->size;
    if (sort == SortMode::Name)
      return x.representative.name < y.representative.name;
    return false;
  });
  return list;
}
std::vector<size_t> browseCatalog(const std::vector<Group> &groups,
                                  const std::string &query, const Theme &theme) {
  return browseFiltered(groups, query, {theme}, theme.sort);
}
SortMode nextSort(SortMode sort) {
  return sort == SortMode::Default   ? SortMode::Recent
         : sort == SortMode::Recent  ? SortMode::Popular
         : sort == SortMode::Popular ? SortMode::Release
         : sort == SortMode::Release ? SortMode::Name
         : sort == SortMode::Name    ? SortMode::Largest
         : sort == SortMode::Largest ? SortMode::Updated
         : sort == SortMode::Updated ? SortMode::Score
                                     : SortMode::Default;
}
std::string genreLabel(const GenreNames &names, const std::string &key) {
  const auto found = names.find(key);
  return found == names.end() || found->second.empty() ? key : found->second;
}
std::vector<Theme> libraryChips(const std::vector<Group> &groups, const GenreNames &names) {
  constexpr size_t GenreChips = 4;
  constexpr uint64_t LightSize = 1ULL << 30, BigSize = 10ULL << 30;
  std::vector<Theme> candidates = {
      {tr("설치 안 한 게임"), SortMode::Default, Filter::All, "", 0, 0, 0, 0, true, false, "", 0},
      {tr("eShop 할인 중"), SortMode::Default, Filter::All, "", 0, 0, 0, 0, false, true, "", 0},
      {tr("평점 80 이상"), SortMode::Score, Filter::All, "", 0, 0, 0, 0, false, false, "", GoodScore},
      {tr("한국어 지원"), SortMode::Default, Filter::Korean, "", 0, 0, 0, 0, false, false, "", 0},
      {tr("2인 이상 플레이"), SortMode::Default, Filter::All, "", 2, 0, 0, 0, false, false, PartyGenre, 0},
      {tr("파티 게임"), SortMode::Default, Filter::All, PartyGenre, 0, 0, 0, 0, false, false, "", 0},
      {tr("업데이트 있음"), SortMode::Default, Filter::Update, "", 0, 0, 0, 0, false, false, "", 0},
      {tr("DLC 있음"), SortMode::Default, Filter::Dlc, "", 0, 0, 0, 0, false, false, "", 0},
      {tr("1GB 이하"), SortMode::Default, Filter::All, "", 0, LightSize, 0, 0, false, false, "", 0},
      {tr("10GB 이상"), SortMode::Default, Filter::All, "", 0, 0, 0, BigSize, false, false, "", 0}};
  std::map<std::string, size_t> counts;
  for (const auto i : browseCatalog(groups, "", {"", SortMode::Default, Filter::All, "", 0, 0, 0, 0, false, false, "", 0}))
    for (const auto &genre : groups[i].representative.categories)
      ++counts[genre];
  std::vector<std::pair<std::string, size_t>> genres(counts.begin(), counts.end());
  std::stable_sort(genres.begin(), genres.end(),
                   [](const auto &a, const auto &b) { return a.second > b.second; });
  genres.erase(std::remove_if(genres.begin(), genres.end(),
                              [](const auto &g) { return g.first == PartyGenre; }),
               genres.end());
  for (size_t n = 0; n < genres.size() && n < GenreChips; ++n)
    candidates.push_back({genreLabel(names, genres[n].first), SortMode::Default, Filter::All, genres[n].first, 0, 0, 0, 0, false, false, "", 0});
  candidates.push_back({tr("커스텀"), SortMode::Default, Filter::Custom, "", 0, 0, 0, 0, false, false, "", 0});
  std::vector<Theme> chips = {{tr("전체"), SortMode::Default, Filter::All, "", 0, 0, 0, 0, false, false, "", 0}};
  for (auto &chip : candidates)
    if (chip.notInstalled || !browseCatalog(groups, "", chip).empty())
      chips.push_back(std::move(chip));
  return chips;
}
size_t homeHero(const std::vector<Group> &groups, const InstalledMask &installed) {
  for (const auto i : browseCatalog(groups, "", {"", SortMode::Recent, Filter::All, "", 0, 0, 0, 0, false, false, "", 0}))
    if (i >= installed.size() || !installed[i])
      return i;
  return 0;
}
std::vector<HeroSlide> homeHeroes(const std::vector<Group> &groups,
                                  const InstalledMask &installed, unsigned seed) {
  constexpr size_t PickFrom = 40; // a seeded pick comes from this many leaders
  std::vector<HeroSlide> slides;
  auto usable = [&](size_t i) {
    return (i >= installed.size() || !installed[i]) &&
           std::none_of(slides.begin(), slides.end(), [i](const HeroSlide &s) { return s.index == i; });
  };
  // `pick` false takes the first usable game; true draws one of the leaders.
  auto add = [&](const std::string &label, Theme theme, bool pick) {
    std::vector<size_t> pool;
    for (const auto i : browseCatalog(groups, "", theme)) {
      if (!usable(i)) continue;
      pool.push_back(i);
      if (!pick || pool.size() >= PickFrom) break;
    }
    if (pool.empty()) return;
    // Salted by slot, not by label: the same games in every language.
    const size_t at = pick && seed ? std::mt19937(seed ^ (unsigned(slides.size() + 1) * 2654435761u))() % pool.size() : 0;
    slides.push_back({pool[at], label});
  };
  add(tr("새로 들어온 게임"), {"", SortMode::Recent, Filter::All, "", 0, 0, 0, 0, false, false, "", 0}, false);
  add(tr("eShop 할인 중"), {"", SortMode::Popular, Filter::All, "", 0, 0, 0, 0, false, true, "", 0}, true);
  add(tr("인기 게임"), {"", SortMode::Popular, Filter::All, "", 0, 0, 0, 0, false, false, "", 0}, false);
  add(tr("함께 하면 더 재미있는 게임"), {"", SortMode::Popular, Filter::All, "", 2, 0, 0, 0, false, false, PartyGenre, 0}, true);
  add(tr("오늘의 추천"), {"", SortMode::Popular, Filter::Korean, "", 0, 0, 0, 0, false, false, "", 0}, true);
  if (slides.size() > HeroSlides) slides.resize(HeroSlides);
  return slides;
}
std::vector<HomeTheme> homeThemes(const std::vector<Group> &groups, size_t hero,
                                  const InstalledMask &installed,
                                  std::vector<HomeTheme> pinned, unsigned seed,
                                  const GenreNames &names) {
  constexpr size_t MinCategoryGames = 4, CategoryRows = 3;
  // Sample sizes for rotating rows. Time-ordered rows (최근 추가, 신작, 최근
  // 업데이트) change on their own as the shop grows, so they stay sorted.
  constexpr size_t TopPool = 36, WidePool = 80, AnyGame = SIZE_MAX;
  std::vector<HomeTheme> rows;
  std::vector<size_t> shown;
  for (auto &row : pinned) {
    if (row.items.empty())
      continue;
    shown.insert(shown.end(), row.items.begin(),
                 row.items.begin() + std::min(row.items.size(), HomeRowVisible));
    rows.push_back(std::move(row));
  }
  // Each row skips the games an earlier row shows on screen (its first
  // HomeRowVisible), so no two rows look alike without starving small shops.
  // A row with a `pool` draws a seeded sample from the first `pool` games of
  // its sort instead of always showing the same top dozen.
  unsigned rowSalt = 0;
  auto add = [&](Theme theme, size_t pool = 0) {
    HomeTheme row{std::move(theme), {}};
    const bool shuffle = pool && seed;
    const size_t take = shuffle ? pool : HomeRowLimit;
    for (const auto i : browseCatalog(groups, "", row.theme)) {
      if (row.items.size() >= take)
        break;
      if (i != hero && (i >= installed.size() || !installed[i]) &&
          std::find(shown.begin(), shown.end(), i) == shown.end())
        row.items.push_back(i);
    }
    if (shuffle) {
      // Salted by row order, not by title, so a language switch keeps the picks.
      std::mt19937 random(seed ^ (unsigned(++rowSalt) * 2654435761u));
      std::shuffle(row.items.begin(), row.items.end(), random);
      if (row.items.size() > HomeRowLimit)
        row.items.resize(HomeRowLimit);
    }
    if (row.items.empty())
      return;
    shown.insert(shown.end(), row.items.begin(),
                 row.items.begin() + std::min(row.items.size(), HomeRowVisible));
    rows.push_back(std::move(row));
  };
  add({tr("최근 추가"), SortMode::Recent, Filter::All, "", 0, 0, 0, 0, false, false, "", 0});
  add({tr("eShop 할인 중"), SortMode::Default, Filter::All, "", 0, 0, 0, 0, false, true, "", 0}, WidePool);
  add({tr("신작"), SortMode::Release, Filter::All, "", 0, 0, 0, 0, false, false, "", 0});
  add({tr("최근 업데이트"), SortMode::Updated, Filter::All, "", 0, 0, 0, 0, false, false, "", 0});
  add({tr("인기"), SortMode::Popular, Filter::All, "", 0, 0, 0, 0, false, false, "", 0}, TopPool);
  add({tr("명작"), SortMode::Score, Filter::All, "", 0, 0, 0, 0, false, false, "", GreatScore}, WidePool);
  add({tr("2인 이상 플레이"), SortMode::Popular, Filter::All, "", 2, 0, 0, 0, false, false, PartyGenre, 0}, WidePool);
  add({tr("파티 게임"), SortMode::Popular, Filter::All, PartyGenre, 0, 0, 0, 0, false, false, "", 0}, WidePool);
  add({tr("대용량 게임"), SortMode::Largest, Filter::All, "", 0, 0, 0, 10ULL << 30, false, false, "", 0}, AnyGame);
  add({tr("한국어 지원"), SortMode::Release, Filter::Korean, "", 0, 0, 0, 0, false, false, "", 0}, WidePool);
  std::map<std::string, size_t> counts;
  for (const auto i : browseCatalog(groups, "", {"", SortMode::Default, Filter::All, "", 0, 0, 0, 0, false, false, "", 0}))
    for (const auto &genre : groups[i].representative.categories)
      ++counts[genre];
  std::vector<std::pair<std::string, size_t>> genres(counts.begin(), counts.end());
  std::stable_sort(genres.begin(), genres.end(),
                   [](const auto &a, const auto &b) { return a.second > b.second; });
  genres.erase(std::remove_if(genres.begin(), genres.end(),
                              [](const auto &g) { return g.first == PartyGenre; }),
               genres.end());
  for (size_t n = 0; n < genres.size() && n < CategoryRows; ++n)
    if (genres[n].second >= MinCategoryGames)
      add({genreLabel(names, genres[n].first), SortMode::Recent, Filter::All, genres[n].first, 0, 0, 0, 0, false, false, "", 0},
          WidePool);
  return rows;
}
std::vector<size_t> selectCatalog(const std::vector<Group> &groups,
                                  const std::string &query, Filter filter) {
  std::vector<size_t> result;
  const auto needle = lower(query);
  for (size_t i = 0; i < groups.size(); ++i) {
    const auto &g = groups[i];
    const auto &lang = g.representative.languages;
    if (filter == Filter::Korean &&
        std::find(lang.begin(), lang.end(), "ko") == lang.end())
      continue;
    if (filter == Filter::Update && !g.update)
      continue;
    if (filter == Filter::Dlc && g.dlc.empty())
      continue;
    if (filter == Filter::Custom && !g.representative.titleId.empty())
      continue;
    if (needle.empty() || g.search.find(needle) != std::string::npos)
      result.push_back(i);
  }
  return result;
}
std::string versionLabel(const Item &item) {
  return item.versionName.empty()          ? "v" + std::to_string(item.version)
         : item.versionName.front() == 'v' ? item.versionName
                                           : "v" + item.versionName;
}
// The server formats eShop prices the Korean way ("42,000원", "4,950円",
// "$24.99"). Outside a Korean interface the won and yen suffixes become the
// usual leading symbols; a Japanese interface keeps 円.
std::string priceText(const std::string &amount) {
  const bool zero = amount.find_first_of("123456789") == std::string::npos;
  if (zero)
    return tr("무료");
  auto suffix = [&](const std::string &unit) {
    return amount.size() > unit.size() && amount.compare(amount.size() - unit.size(), unit.size(), unit) == 0;
  };
  const auto lang = language();
  const std::string won = "\xEC\x9B\x90", yen = "\xE5\x86\x86"; // 원, 円
  if (suffix(won) && lang != Language::Korean)
    return "\xE2\x82\xA9" + amount.substr(0, amount.size() - won.size()); // ₩
  if (suffix(yen) && lang != Language::Japanese)
    return lang == Language::Korean ? amount.substr(0, amount.size() - yen.size()) + "\xEC\x97\x94" // 엔
                                    : "\xC2\xA5" + amount.substr(0, amount.size() - yen.size());    // ¥
  return amount;
}
std::string formatBytes(uint64_t bytes) {
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  int i = 0;
  double n = bytes;
  while (n >= 1024 && i < 4) {
    n /= 1024;
    ++i;
  }
  char buffer[64];
  // One decimal under 10 (1.4 GB), but never a bare ".0" (2 GB, not 2.0 GB).
  const bool decimal = i > 1 && n < 10 && std::lround(n * 10) % 10 != 0;
  std::snprintf(buffer, sizeof(buffer), decimal ? "%.1f %s" : "%.0f %s", n, units[i]);
  return buffer;
}
} // namespace foil
