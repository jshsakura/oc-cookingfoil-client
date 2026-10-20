// Two servers' shop lists as one: each item names its server, a file both
// hold is listed once from the first and names the other, and different
// versions or kinds of a title stay apart.
#include "Test.hpp"
#include "core/Catalog.hpp"
#include <algorithm>
namespace {
foil::Item item(const std::string &titleId, const std::string &type, uint32_t version, const std::string &url) {
  foil::Item i;
  i.titleId = titleId;
  i.baseId = titleId.substr(0, 13) + "000";
  i.type = type;
  i.version = version;
  i.url = url;
  i.name = titleId;
  return i;
}
} // namespace
int runMergeCatalogTests() {
  int failures = 0;
  const std::string a = "https://a.example", b = "https://b.example";
  const std::string base = "0100AAAA00010000", update = "0100AAAA00010800";
  std::vector<foil::ServerItems> lists{
      {a, {item(base, "base", 0, a + "/g1"), item(update, "update", 65536, a + "/u1")}},
      {b,
       {item(base, "base", 0, b + "/g1"), item(update, "update", 131072, b + "/u2"),
        item("0100BBBB00020000", "base", 0, b + "/g2"), item("", "base", 0, b + "/odd")}}};
  const auto merged = foil::mergeServers(lists);
  CHECK(merged.size() == 5);
  const auto find = [&](const std::string &url) {
    return std::find_if(merged.begin(), merged.end(), [&](const foil::Item &i) { return i.url == url; });
  };
  // The base both hold: once, from the first, naming the second.
  CHECK(find(a + "/g1") != merged.end() && find(b + "/g1") == merged.end());
  CHECK(find(a + "/g1")->server == a && find(a + "/g1")->alsoOn == std::vector<std::string>{b});
  // Updates of other versions are other files.
  CHECK(find(a + "/u1") != merged.end() && find(b + "/u2") != merged.end());
  CHECK(find(b + "/u2")->server == b && find(b + "/u2")->alsoOn.empty());
  CHECK(find(b + "/g2")->server == b);
  // No Title ID: kept as it is.
  CHECK(find(b + "/odd") != merged.end());
  // One server: every item names it, nothing else changes.
  const auto one = foil::mergeServers({{a, {item(base, "base", 0, a + "/g1")}}});
  CHECK(one.size() == 1 && one[0].server == a && one[0].alsoOn.empty());
  // Grouped, the merged list makes one game with its two updates.
  const auto groups = foil::groupCatalog(merged);
  const auto game = std::find_if(groups.begin(), groups.end(), [&](const foil::Group &g) { return g.id == base; });
  CHECK(game != groups.end());
  return failures;
}
