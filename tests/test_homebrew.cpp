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
  // Folder installs: the server's install_path and files, each checked to
  // stay inside the app's folder.
  {
    const std::string sha(64, 'c');
    auto e = entry;
    e.insert(e.size() - 1, R"(,"install_path":"switch/ytb/ytb-player.nro","install_dir":"switch/ytb","files":[)"
                           R"({"path":"switch/ytb/ytb-player.nro","size":10,"sha256":")" + sha + R"(","url":"/f/1"},)"
                           R"({"path":"switch/ytb/data/a.bin","size":5,"sha256":")" + sha + R"(","url":"/f/2"}])");
    const auto folder = parse(R"({"homebrew":[)" + e + "]}", "https://shop.example");
    CHECK(folder.size() == 1);
    if (!folder.empty()) {
      CHECK(folder.front().installPath == "switch/ytb/ytb-player.nro");
      CHECK(folder.front().files.size() == 2 && folder.front().files[1].url == "https://shop.example/f/2" &&
            folder.front().files[1].size == 5 && folder.front().files[1].sha256 == sha);
      const auto again = parse(R"({"homebrew":[)" + serialize(folder.front()) + "]}", "");
      CHECK(again.size() == 1 && again.front().files.size() == 2 && again.front().installPath == "switch/ytb/ytb-player.nro");
    }
    const auto rejected = [&](const std::string &from, const std::string &to) {
      auto bad = e;
      bad.replace(bad.find(from), from.size(), to);
      return parse(R"({"homebrew":[)" + bad + "]}", "https://shop.example").empty();
    };
    CHECK(rejected("switch/ytb/data/a.bin", "switch/ytb/../../a.bin"));
    CHECK(rejected("switch/ytb/data/a.bin", "/switch/ytb/a.bin"));
    CHECK(rejected("switch/ytb/data/a.bin", "switch/other/a.bin"));
    CHECK(rejected(R"("install_path":"switch/ytb/ytb-player.nro")", R"("install_path":"atmosphere/x.nro")"));
  }
  // Only install_dir: the nro goes inside that folder; one outside switch/ is refused.
  {
    auto e = entry;
    e.insert(e.size() - 1, R"(,"install_dir":"switch/ytb")");
    const auto dir = parse(R"({"homebrew":[)" + e + "]}", "https://shop.example");
    CHECK(dir.size() == 1 && !dir.empty() && dir.front().installPath == "switch/ytb/ytb-player.nro");
    auto bad = entry;
    bad.insert(bad.size() - 1, R"(,"install_dir":"atmosphere")");
    CHECK(parse(R"({"homebrew":[)" + bad + "]}", "https://shop.example").empty());
  }
  // Without install_path the app goes to switch/<stem>/<file>.
  if (!apps.empty()) CHECK(apps.front().installPath == "switch/ytb-player/ytb-player.nro" && apps.front().files.empty());
  // kind splits tools from ports (tool when missing or unknown); app is the
  // folder under switch/, else the install folder's name.
  {
    const auto one = [&](const std::string &extra) {
      auto e = entry;
      e.insert(e.size() - 1, extra);
      const auto list = parse(R"({"homebrew":[)" + e + "]}", "https://shop.example");
      return list.empty() ? App{} : list.front();
    };
    CHECK(one("").kind == Kind::Tool && one("").app == "ytb-player");
    CHECK(one(R"(,"kind":"port","app":"sm64")").kind == Kind::Port);
    CHECK(one(R"(,"kind":"port","app":"sm64")").app == "sm64");
    CHECK(one(R"(,"kind":"tool")").kind == Kind::Tool);
    CHECK(one(R"(,"kind":"game")").kind == Kind::Tool);
    CHECK(one(R"(,"kind":7)").kind == Kind::Tool);
    CHECK(one(R"(,"app":"../x")").app == "ytb-player");
    CHECK(one(R"(,"app":"a/b")").app == "ytb-player");
    CHECK(one(R"(,"install_dir":"switch/ytb")").app == "ytb");
    const auto port = one(R"(,"kind":"port","app":"Port 64")");
    const auto back = parse(R"({"homebrew":[)" + serialize(port) + "]}", "");
    CHECK(back.size() == 1 && back.front().kind == Kind::Port && back.front().app == "Port 64");
  }
  CHECK(appFolderAllowed("sm64") && appFolderAllowed("Port 64") && appFolderAllowed("a.b_c-d"));
  CHECK(!appFolderAllowed("") && !appFolderAllowed(".") && !appFolderAllowed(".."));
  CHECK(!appFolderAllowed("a..b") && !appFolderAllowed("a/b") && !appFolderAllowed("a\\b"));
  CHECK(!appFolderAllowed("a:b") && !appFolderAllowed(" a") && !appFolderAllowed("a ") && !appFolderAllowed("a."));
  CHECK(!appFolderAllowed(std::string(65, 'a')));
  // A path counts as the app's only below switch/<app>/, every part safe.
  CHECK(insideApp("sm64", "switch/sm64/mods/a.bin"));
  CHECK(insideApp("sm64", "switch/sm64/save.dat"));
  CHECK(!insideApp("sm64", "switch/sm64"));
  CHECK(!insideApp("sm64", "switch/sm64/"));
  CHECK(!insideApp("sm64", "switch/sm64/../other/a"));
  CHECK(!insideApp("sm64", "switch/sm64/./a"));
  CHECK(!insideApp("sm64", "switch/sm64//a"));
  CHECK(!insideApp("sm64", "switch/sm64x/a"));
  CHECK(!insideApp("sm64", "switch/other/a"));
  CHECK(!insideApp("sm64", "/switch/sm64/a"));
  CHECK(!insideApp("sm64", "switch/sm64/a\\..\\b"));
  CHECK(!insideApp("sm64", "switch/sm64/c:a"));
  CHECK(!insideApp("sm64", "atmosphere/contents/a"));
  CHECK(!insideApp("..", "switch/../a"));
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
  Ledger ledger{{"gh-9c9ef5794087", {"sha256:fb7d", "1.1.0", "switch/ytb-player/ytb-player.nro", {}}}};
  const auto read = parseLedger(serializeLedger(ledger));
  CHECK(read.size() == 1 && read.at("gh-9c9ef5794087").file == "switch/ytb-player/ytb-player.nro");
  CHECK(parseLedger("garbage").empty());
  {
    Ledger folder{{"gh-1", {"h", "1", "switch/ytb/ytb.nro", {"switch/ytb/data/a.bin", "../escape"}}}};
    const auto back = parseLedger(serializeLedger(folder));
    CHECK(back.at("gh-1").files == std::vector<std::string>{"switch/ytb/data/a.bin"});
  }
  if (!apps.empty()) {
    const auto there = [](const std::string &) { return true; };
    const auto gone = [](const std::string &) { return false; };
    CHECK(status(apps.front(), {}, there) == Status::Available);
    CHECK(status(apps.front(), ledger, there) == Status::Installed);
    CHECK(status(apps.front(), ledger, gone) == Status::Available);
    auto newer = apps.front();
    newer.hash = "sha256:0000";
    CHECK(status(newer, ledger, there) == Status::Update);
    auto moved = apps.front();
    moved.installPath = "switch/ytb/ytb-player.nro";
    CHECK(status(moved, ledger, there) == Status::Update);
  }
  {
    // The card is the truth, the ledger a cache: marks round trip, the same
    // size and time are taken unhashed, anything else is hashed and followed.
    Record seen{"sha256:fb7d", "1.1.0", "switch/a/a.nro", {}, {100, 5}, "aa"};
    const auto back = parseLedger(serializeLedger({{"a", seen}}));
    CHECK(back.at("a") == seen);
    std::map<std::string, CardFile> card{{"switch/a/a.nro", {100, 5}}};
    std::map<std::string, std::string> bytes{{"switch/a/a.nro", "aa"}};
    int hashes = 0;
    const auto look = [&](const std::string &f) -> std::optional<CardFile> {
      const auto at = card.find(f);
      return at == card.end() ? std::nullopt : std::optional<CardFile>(at->second);
    };
    const auto sha = [&](const std::string &f) { ++hashes; return bytes.count(f) ? bytes.at(f) : std::string(); };
    const auto version = [](const std::string &) { return std::string("2.0.0"); };
    const Ledger ledger{{"a", seen}};
    // Same: no hash, nothing to fix.
    auto same = checkLedger(ledger, look, sha, version);
    CHECK(hashes == 0 && same.hashed == 0 && same.fixed.empty() && same.onCard.at("switch/a/a.nro"));
    // Only touched: hashed once, the same bytes, the record kept with new marks.
    card["switch/a/a.nro"] = {100, 9};
    auto touched = checkLedger(ledger, look, sha, version);
    CHECK(hashes == 1 && touched.followed.empty() && touched.fixed.at("a").hash == "sha256:fb7d" &&
          touched.fixed.at("a").seen == (CardFile{100, 9}));
    // Other bytes: the record follows the card, so it no longer reads as the
    // server's build (an update) and shows the card's version.
    card["switch/a/a.nro"] = {120, 9};
    bytes["switch/a/a.nro"] = "bb";
    auto other = checkLedger(ledger, look, sha, version);
    CHECK(other.followed == std::vector<std::string>{"a"});
    CHECK(other.fixed.at("a").hash.empty() && other.fixed.at("a").version == "2.0.0" && other.fixed.at("a").fileSha == "bb");
    App listed;
    listed.id = "a";
    listed.hash = "sha256:fb7d";
    listed.installPath = "switch/a/a.nro";
    const auto there = [](const std::string &) { return true; };
    CHECK(status(listed, ledger, there) == Status::Installed);
    CHECK(status(listed, other.fixed, there) == Status::Update);
    // Gone: reported, not hashed.
    hashes = 0;
    card.clear();
    auto gone = checkLedger(ledger, look, sha, version);
    CHECK(hashes == 0 && !gone.onCard.at("switch/a/a.nro") && gone.fixed.empty());
    // A ledger from before marks: hashed once and taken as it is.
    card["switch/a/a.nro"] = {120, 9};
    const Ledger older{{"a", {"sha256:fb7d", "1.1.0", "switch/a/a.nro", {}}}};
    auto adopted = checkLedger(older, look, sha, version);
    CHECK(hashes == 1 && adopted.followed.empty() && adopted.fixed.at("a").fileSha == "bb" &&
          adopted.fixed.at("a").hash == "sha256:fb7d");
    // Unreadable: left for the next look.
    bytes.clear();
    CHECK(checkLedger(older, look, sha, version).fixed.empty());
  }
  {
    // Patches and saves name an app by its folder; the list gives its name.
    std::vector<App> listed(2);
    listed[0].app = "ytb";
    listed[0].name = "YTB Player";
    listed[1].app = "nameless";
    CHECK(findApp(listed, "ytb") == &listed[0]);
    CHECK(findApp(listed, "YTB Player") == nullptr);
    CHECK(findApp(listed, "") == nullptr);
    CHECK(findApp({}, "ytb") == nullptr);
    CHECK(appTitle(listed, "ytb") == "YTB Player");
    CHECK(appTitle(listed, "nameless") == "nameless");
    CHECK(appTitle(listed, "gone") == "gone");
    // A game groups by Title ID, an app by its folder, never with each other.
    CHECK(ownerKey("0100ABCDEF012000", "") == "0100ABCDEF012000");
    CHECK(ownerKey("", "ytb") == "app:ytb");
    CHECK(ownerKey("", "ytb") != ownerKey("ytb", ""));
    CHECK(ownerKey("0100ABCDEF012000", "ytb") == "0100ABCDEF012000");
  }
  {
    // A zip the server keeps (archive): read, kept through serialize, and
    // left out (the app stays) when an entry's path could leave switch/.
    const std::string zip =
        R"(,"archive":{"format":"zip","url":"/api/homebrew/gh-9c9ef5794087/archive","size":4096,"sha256":"ab",)"
        R"("entries":[{"entry":"ytb/ytb-player.nro","path":"switch/ytb-player/ytb-player.nro","size":100}]}})";
    auto e = entry;
    e.replace(e.size() - 1, 1, zip);
    const auto withZip = parse(R"({"homebrew":[)" + e + "]}", "https://shop.example");
    CHECK(withZip.size() == 1 && withZip[0].archive.usable() && withZip[0].archive.size == 4096 &&
          withZip[0].archive.url == "https://shop.example/api/homebrew/gh-9c9ef5794087/archive" &&
          withZip[0].archive.entries.size() == 1 && withZip[0].archive.entries[0].size == 100);
    if (!withZip.empty()) {
      const auto again = parse(R"({"homebrew":[)" + serialize(withZip[0]) + "]}", "https://shop.example");
      CHECK(again.size() == 1 && again[0].archive.usable() && again[0].archive.entries[0].path == "switch/ytb-player/ytb-player.nro");
    }
    auto outside = e;
    outside.replace(outside.find("switch/ytb-player/ytb"), 21, "atmosphere/../../ytb");
    const auto noZip = parse(R"({"homebrew":[)" + outside + "]}", "https://shop.example");
    CHECK(noZip.size() == 1 && !noZip[0].archive.usable());
  }
  return failures;
}
