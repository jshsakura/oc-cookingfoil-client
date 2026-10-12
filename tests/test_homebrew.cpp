#include "Test.hpp"
#include "core/ClientUpdate.hpp"
#include "core/Homebrew.hpp"
#include "core/Sha256.hpp"
using namespace foil::homebrew;
int runHomebrewTests() {
  int failures = 0;
  const std::string entry =
      R"({"id":"gh-9c9ef5794087","name":"YTB Player","version":"1.1.0","author":"muratgokce","description":null,)"
      R"("size":33584861,"hash":"sha256:fb7d","file_name":"ytb-player.nro","icon_url":"/api/homebrew/gh-9c9ef5794087/icon",)"
      R"("download_url":"/api/homebrew/gh-9c9ef5794087/download","source":"github","released_at":"2026-09-26T20:06:01Z",)"
      R"("repo_url":"https://github.com/murat303/ytb-player"})";
  const auto apps = parse(R"({"homebrew":[)" + entry + "]}", "https://shop.example");
  CHECK(apps.size() == 1);
  if (!apps.empty()) {
    const auto &a = apps.front();
    CHECK(a.id == "gh-9c9ef5794087" && a.name == "YTB Player" && a.version == "1.1.0" && a.author == "muratgokce");
    CHECK(a.description.empty() && a.size == 33584861 && a.hash == "sha256:fb7d");
    CHECK(a.iconUrl == "https://shop.example/api/homebrew/gh-9c9ef5794087/icon");
    CHECK(a.downloadUrl == "https://shop.example/api/homebrew/gh-9c9ef5794087/download");
    CHECK(a.source == "github" && a.repoUrl == "https://github.com/murat303/ytb-player");
  }
  // An entry that could write outside its folder, or lacks what is needed, is left out.
  const auto bad = [&](const std::string &from, const std::string &to) {
    auto e = entry;
    e.replace(e.find(from), from.size(), to);
    return parse(R"({"homebrew":[)" + e + "]}", "https://shop.example").empty();
  };
  CHECK(bad(R"("ytb-player.nro")", R"("../ytb-player.nro")"));
  CHECK(bad(R"("ytb-player.nro")", R"("a/ytb-player.nro")"));
  CHECK(bad(R"("ytb-player.nro")", R"("ytb-player.exe")"));
  CHECK(bad(R"("ytb-player.nro")", R"(".nro")"));
  CHECK(bad(R"("hash":"sha256:fb7d")", R"("hash":"")"));
  CHECK(bad(R"("id":"gh-9c9ef5794087")", R"("id":"a/b")"));
  CHECK(bad(R"("download_url":"/api)", R"("download_url":"ftp://x/api)"));
  CHECK(parse("nope", "https://s").empty());
  // A HOME Menu shortcut comes along when the server built one; a title ID
  // that is not 16 hex digits means none.
  {
    auto e = entry;
    e.insert(e.size() - 1, R"(,"forwarder_url":"/api/homebrew/gh-9c9ef5794087/forwarder","forwarder_title_id":"0100ABCDEF012000","forwarder_size":524288)");
    const auto withShortcut = parse(R"({"homebrew":[)" + e + "]}", "https://shop.example");
    CHECK(withShortcut.size() == 1 && withShortcut.front().forwarderTitleId == "0100ABCDEF012000" &&
          withShortcut.front().forwarderSize == 524288 &&
          withShortcut.front().forwarderUrl == "https://shop.example/api/homebrew/gh-9c9ef5794087/forwarder");
    if (!withShortcut.empty()) {
      const auto again = parse(R"({"homebrew":[)" + serialize(withShortcut.front()) + "]}", "");
      CHECK(again.size() == 1 && again.front().forwarderTitleId == "0100ABCDEF012000");
    }
    auto broken = e;
    broken.replace(broken.find("0100ABCDEF012000"), 16, "nope");
    const auto without = parse(R"({"homebrew":[)" + broken + "]}", "https://shop.example");
    CHECK(without.size() == 1 && without.front().forwarderUrl.empty());
  }
  if (!apps.empty()) {
    const auto again = parse(R"({"homebrew":[)" + serialize(apps.front()) + "]}", "");
    CHECK(again.size() == 1 && again.front().downloadUrl == apps.front().downloadUrl &&
          again.front().hash == apps.front().hash && again.front().size == apps.front().size);
  }
  CHECK(sdPath("ytb-player.nro") == "switch/ytb-player/ytb-player.nro");
  // The download's ETag: the file's sha256, quoted, maybe prefixed.
  const std::string sha(64, 'a');
  CHECK(etagSha256("\"sha256:" + std::string(64, 'A') + "\"") == sha);
  CHECK(etagSha256("\"" + sha + "\"") == sha);
  CHECK(etagSha256("W/\"abc\"").empty());
  CHECK(etagSha256("").empty());
  // A streamed hash equals the one-shot hash.
  foil::Sha256 hasher;
  hasher.update("a", 1);
  hasher.update("bc", 2);
  CHECK(hasher.hex() == foil::update::sha256Hex("abc"));
  // The ledger round trips; states follow the hash and the file.
  Ledger ledger{{"gh-9c9ef5794087", {"sha256:fb7d", "1.1.0", "switch/ytb-player/ytb-player.nro"}}};
  const auto read = parseLedger(serializeLedger(ledger));
  CHECK(read.size() == 1 && read.at("gh-9c9ef5794087").file == "switch/ytb-player/ytb-player.nro");
  CHECK(parseLedger("garbage").empty());
  if (!apps.empty()) {
    const auto there = [](const std::string &) { return true; };
    const auto gone = [](const std::string &) { return false; };
    CHECK(status(apps.front(), {}, there) == Status::Available);
    CHECK(status(apps.front(), ledger, there) == Status::Installed);
    CHECK(status(apps.front(), ledger, gone) == Status::Available);
    auto newer = apps.front();
    newer.hash = "sha256:0000";
    CHECK(status(newer, ledger, there) == Status::Update);
  }
  return failures;
}
