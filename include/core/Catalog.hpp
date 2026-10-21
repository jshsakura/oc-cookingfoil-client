#pragma once
#include "core/Types.hpp"
#include <map>
#include <initializer_list>
#include <optional>
#include <string_view>
namespace foil {
struct Group {
  std::string id;
  Item representative;
  std::optional<Item> base, update;
  std::vector<Item> dlc, alternatives;
  // Every file's name, title id and publisher, lowercased, one per line:
  // what search looks at. Kept instead of a copy of every file (that copy
  // was most of the catalog's memory).
  std::string search;
  int64_t addedAt = 0;
};
// The worst file check among a game's files (Item::integrity): 0, 1 or 2.
int groupIntegrity(const Group &g);
// A card's labels, most wanted first. The file's state is one label:
// 손상 의심 or 수정됨 over 최신 버전 (its update is the newest the server
// knows) or 업데이트.
enum class CardLabel { KoreanPatch, RequiredPatch, Damaged, Modified, Latest, Update, Sale, Dlc };
std::vector<CardLabel> cardLabels(const Group &g);
// Genre key (the server's English name) -> name shown on screen.
using GenreNames = std::map<std::string, std::string>;
std::string genreLabel(const GenreNames &names, const std::string &key);
// 20240715 -> "2024.07.15"; only a year or a month known (2024, 202407,
// 20240000, 20240700) -> "2024" or "2024.07"; anything else as it is.
std::string dateText(int date);
enum class Filter { All, Korean, Update, Dlc, Custom };
std::vector<Group> groupCatalog(std::vector<Item> items);
// Several servers' lists as one: each item names its server (`lists` in the
// servers' order, by url), and a file more than one holds (same title,
// kind and version) is listed once, from the first, naming the others.
struct ServerItems {
    std::string server;
    std::vector<Item> items;
};
std::vector<Item> mergeServers(std::vector<ServerItems> lists);
std::vector<size_t> selectCatalog(const std::vector<Group> &groups,
                                  const std::string &query = "",
                                  Filter filter = Filter::All);
enum class SortMode { Default, Recent, Release, Name, Popular, Largest, Updated, Score };
const char *sortLabel(SortMode sort);
// A home row and, through R, the same view in the full list.
struct Theme {
  std::string title;
  SortMode sort = SortMode::Default;
  Filter filter = Filter::All;
  std::string category;
  int minPlayers = 0;
  uint64_t maxSize = 0; // base size cap, 0 for none
  int maxPlayers = 0;   // 0 for no upper bound
  uint64_t minSize = 0; // base size floor, 0 for none
  bool notInstalled = false;
  bool onSale = false; // eShop discount running (feature item-price)
  std::string withoutCategory; // genre to leave out, empty for none
  int minScore = 0;            // review score floor, 0 for none
};
struct HomeTheme {
  Theme theme;
  std::vector<size_t> items;
};
// A shelf holds this many, then a 모두 보기 card opens the full list.
constexpr size_t HomeRowLimit = 30, HomeRowVisible = 6;
// Shop lists show only games that have a base; update or DLC-only rows are
// leftovers, not something to install on their own.
std::vector<size_t> browseCatalog(const std::vector<Group> &groups,
                                  const std::string &query, const Theme &theme);
// Games passing every filter (AND), sorted; the multi-select list view.
// `installed` marks groups already on the console (same order as groups);
// empty means unknown, and nothing is treated as installed.
using InstalledMask = std::vector<bool>;
// Whether any of `texts` holds `query` (ASCII letters ignore case); an
// empty query matches everything. 세이브 and 유저 패치's search.
bool textMatches(std::initializer_list<std::string_view> texts, const std::string &query);
std::vector<size_t> browseFiltered(const std::vector<Group> &groups, const std::string &query,
                                   const std::vector<Theme> &filters, SortMode sort,
                                   const InstalledMask &installed = {});
// Home skips installed games; the shop is for what is not on the console yet.
size_t homeHero(const std::vector<Group> &groups, const InstalledMask &installed = {});
// The rotating banner at the top of home: one game per reason, no repeats.
struct HeroSlide {
  size_t index;
  std::string label;
};
constexpr size_t HeroSlides = 5;
std::vector<HeroSlide> homeHeroes(const std::vector<Group> &groups,
                                  const InstalledMask &installed = {}, unsigned seed = 0);
// Filter chips for the full list; chips with no matching game are left out.
std::vector<Theme> libraryChips(const std::vector<Group> &groups, const GenreNames &names = {});
SortMode nextSort(SortMode sort);
// `pinned` rows (admin picks) come first and count toward "already shown".
// A nonzero `seed` shuffles the rows with no natural order (big games,
// genres) so they change between launches; 0 keeps them sorted.
std::vector<HomeTheme> homeThemes(const std::vector<Group> &groups, size_t hero,
                                  const InstalledMask &installed = {},
                                  std::vector<HomeTheme> pinned = {}, unsigned seed = 0,
                                  const GenreNames &names = {});
// The server's 256px cover (`?size=sm`), the same one its web pages use.
// sm is 256px, md 512px (feature image-sizes); covers use sm, large art md.
std::string thumbnailUrl(const std::string &url, const char *size = "sm");
std::string versionLabel(const Item &item);
// The store writes free titles as "0원" or "$0.00"; those read "무료".
std::string priceText(const std::string &amount);
std::string formatBytes(uint64_t bytes);
// A speed in the unit its size calls for, in bytes as formatBytes counts
// them (1024) and in bits as speed test apps do (1000):
// "850 KB/s · 6.8 Mbps", "13.0 MB/s · 109 Mbps".
std::string formatSpeed(double bytesPerSecond);
// The file's kind from its address, upper case (NSP, NSZ, XCI, XCZ); empty
// when the address has no short extension.
std::string fileFormat(const Item &item);
} // namespace foil
