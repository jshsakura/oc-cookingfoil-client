#include <chrono>
#include <algorithm>
#include "Test.hpp"
#include "core/State.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include <filesystem>
#include "core/Log.hpp"
#include <fstream>
#include <unistd.h>
int runConfigTests() {
    int failures = 0;
    {
        // A plain http address on the internet is warned about; https, and
        // http at home (LAN, loopback, CGNAT/Tailscale, .local, one word), not.
        using foil::plainHttpOverInternet;
        CHECK(plainHttpOverInternet("http://server.example.com"));
        CHECK(plainHttpOverInternet("http://203.0.113.5:8000"));
        CHECK(plainHttpOverInternet("http://[2001:db8::1]:8000"));
        CHECK(!plainHttpOverInternet("https://server.example.com"));
        CHECK(!plainHttpOverInternet("https://203.0.113.5"));
        for (const char* home : {"http://192.168.1.10:8000", "http://10.0.0.2", "http://172.20.0.3", // privacy-ok: private ranges
                                 "http://127.0.0.1:8000", "http://100.100.1.1", "http://169.254.1.1", // privacy-ok: private ranges
                                 "http://nas.local", "http://nas", "http://localhost:8000", "http://[::1]:8000",
                                 "http://[fd00::5]", "http://box.home.arpa"})
            CHECK(!plainHttpOverInternet(home));
        CHECK(plainHttpOverInternet("http://172.32.0.1")); // privacy-ok: just past 172.16/12
        CHECK(plainHttpOverInternet("http://100.128.0.1")); // privacy-ok: just past 100.64/10
        CHECK(!plainHttpOverInternet("not a url"));
    }
    CHECK(foil::normalizeUrl("server.example.com//") == "https://server.example.com");
    CHECK(foil::normalizeUrl("http://host:9080/prefix/") == "http://host:9080/prefix");
    CHECK(foil::normalizeUrl("ftp://host").empty());
    CHECK(foil::normalizeUrl("https://host\r\nX-Key: secret").empty());
    CHECK(foil::normalizeUrl("https://user:pass@host").empty());
    auto valid = foil::parseConfig(R"({"servers":[{"url":"host/","unknown":42}],"future":true})");
    CHECK(!valid.error);
    CHECK(valid.value.servers.size() == 1);
    CHECK(valid.value.servers[0].url == "https://host");
    CHECK(valid.value.servers[0].enabled);
    CHECK(valid.value.language == "ko" && valid.value.installTarget == "sd");
    CHECK(foil::parseConfig(R"({"servers":[{}]})").error);
    CHECK(foil::parseConfig(R"({"servers":[{"url":42}]})").error);
    CHECK(!foil::parseConfig(R"({"language":"ja"})").error);
    CHECK(!foil::parseConfig(R"({"language":"fr"})").error);
    CHECK(!foil::parseConfig(R"({"language":"zh-Hant"})").error);
    CHECK(foil::parseConfig(R"({"language":"xx"})").error);
    CHECK(foil::parseConfig(R"({"installTarget":"system"})").error);
    CHECK(foil::parseConfig(R"({"servers":[{"url":"host","enabled":"false"}]})").error);
    CHECK(foil::parseConfig("{\n  \"servers\": [\n  invalid\n]} ").error.line == 3);
    CHECK(foil::parseConfig("{} trailing").error);
    CHECK(!foil::parseConfig(R"({"servers":[{"url":"host","title":"원격 서버 日本 中文"}]})").error);
    CHECK(foil::parseConfig(std::string("{\"language\":\"") + char(0xc0) + char(0xaf) + "\"}").error);
    {
        // What the app writes reads back the same: every field, a title in
        // three scripts, empty optional fields and a server switched off.
        foil::Config config;
        config.language = "ja";
        config.installTarget = "nand";
        config.servers.push_back({"원격 서버 日本 中文", "https://server.example.com:8443/prefix", "user",
                                  "p\"a\\ss/word", "id.access", "secret", true});
        config.servers.push_back({"빈 칸", "http://192.168.0.2:9080", "", "", "", "", false});
        const auto same = [](const foil::Config &a, const foil::Config &b) {
            if (a.language != b.language || a.installTarget != b.installTarget || a.servers.size() != b.servers.size())
                return false;
            for (size_t i = 0; i < a.servers.size(); ++i) {
                const auto &x = a.servers[i], &y = b.servers[i];
                if (x.title != y.title || x.url != y.url || x.username != y.username || x.password != y.password ||
                    x.cfClientId != y.cfClientId || x.cfClientSecret != y.cfClientSecret || x.enabled != y.enabled)
                    return false;
            }
            return true;
        };
        const auto back = foil::parseConfig(foil::serializeConfig(config));
        CHECK(!back.error);
        CHECK(same(back.value, config));
        CHECK(same(foil::parseConfig(foil::serializeConfig(foil::Config{})).value, foil::Config{}));
        // An empty title reads back as the address's host.
        config.servers[1].title.clear();
        CHECK(foil::parseConfig(foil::serializeConfig(config)).value.servers[1].title == "192.168.0.2:9080");
    }
    char temporary[] = "/tmp/cookingfoil-config-XXXXXX";
    const char* root = mkdtemp(temporary);
    CHECK(root);
    if (root) {
        const auto path = std::string(root) + "/nested/config.json";
        auto created = foil::loadConfig(path);
        CHECK(!created.error && created.created);
        CHECK(created.value.servers.empty());
        const auto original = foil::readFile(path);
        CHECK(!foil::loadConfig(path).created);
        CHECK(foil::readFile(path) == original);
        { std::ofstream f(path); f << "{\n broken"; }
        CHECK(foil::loadConfig(path).error.line == 2);
        CHECK(foil::readFile(path) == "{\n broken");
        // saveConfig replaces the file with what parseConfig reads back.
        foil::Config saved;
        saved.language = "en";
        saved.servers.push_back({"내 서버", "https://server.example.com", "player", "pass word", "", "", true});
        std::string error;
        CHECK(foil::saveConfig(path, saved, error));
        const auto loaded = foil::loadConfig(path);
        CHECK(!loaded.error && loaded.value.servers.size() == 1);
        CHECK(loaded.value.servers[0].title == "내 서버" && loaded.value.servers[0].password == "pass word");
        CHECK(loaded.value.language == "en");
        // A save cut after the old file went aside: config.json is missing
        // and the save goes on from what atomicWrite left beside it.
        std::filesystem::rename(path, path + ".old");
        auto restored = foil::loadConfig(path);
        CHECK(!restored.error && !restored.created && restored.value.servers.size() == 1);
        CHECK(!std::filesystem::exists(path + ".old"));
        // The new copy, complete before any rename, wins over the older one.
        foil::Config newer = saved;
        newer.servers[0].title = "새 이름";
        { std::ofstream f(path + ".tmp"); f << foil::serializeConfig(newer); }
        std::filesystem::rename(path, path + ".old");
        CHECK(foil::loadConfig(path).value.servers[0].title == "새 이름");
        // What is on the card is what counts: servers taken out by hand, or
        // config.json deleted, stay gone; a .bak an older build kept goes.
        { std::ofstream f(path + ".bak"); f << foil::serializeConfig(saved); }
        { std::ofstream f(path); f << foil::serializeConfig(foil::Config{}); }
        CHECK(foil::loadConfig(path).value.servers.empty());
        CHECK(!std::filesystem::exists(path + ".bak"));
        CHECK(foil::saveConfig(path, saved, error));
        CHECK(!std::filesystem::exists(path + ".bak"));
        std::filesystem::remove(path);
        auto deleted = foil::loadConfig(path);
        CHECK(!deleted.error && deleted.created && deleted.value.servers.empty());
        // Nothing in it: no servers, not an error, and the file stays as it is.
        { std::ofstream f(path); }
        auto blank = foil::loadConfig(path);
        CHECK(!blank.error && blank.value.servers.empty() && foil::readFile(path).empty());
        // Unreadable: its error and its line, and the file is left alone.
        { std::ofstream f(path); f << "{ \"servers\": [ cut"; }
        CHECK(foil::loadConfig(path).error);
        CHECK(foil::readFile(path) == "{ \"servers\": [ cut");
        // A broken copy beside it is not taken.
        std::filesystem::remove(path);
        std::filesystem::remove(path + ".bak");
        { std::ofstream f(path + ".old"); f << "{ broken"; }
        auto fresh = foil::loadConfig(path);
        CHECK(!fresh.error && fresh.created && fresh.value.servers.empty());
        std::filesystem::remove_all(root);
    }
    {
    // A file saved with what it already holds is left alone (nothing new
    // to wear the card with); anything else is written.
    char folder[] = "/tmp/cookingfoil-same-XXXXXX";
    if (const char* dir = mkdtemp(folder)) {
      const std::string path = std::string(dir) + "/ledger.json";
      std::string error;
      CHECK(foil::atomicWrite(path, "{\"a\":1}", error));
      const auto earlier = std::filesystem::last_write_time(path) - std::chrono::hours(24);
      std::filesystem::last_write_time(path, earlier);
      CHECK(foil::atomicWrite(path, "{\"a\":1}", error) && std::filesystem::last_write_time(path) == earlier);
      CHECK(foil::atomicWrite(path, "{\"a\":2}", error) && foil::readFile(path) == "{\"a\":2}");
      CHECK(foil::atomicWrite(path, "{\"a\":20}", error) && foil::readFile(path) == "{\"a\":20}");
      std::filesystem::remove_all(dir);
    }
    }
    {
    // A long session's log is cut back as it goes, not only at the next
    // start: it never grows past twice its cap, and the newest lines stay.
    char folder[] = "/tmp/cookingfoil-log-XXXXXX";
    if (const char* dir = mkdtemp(folder)) {
      const std::string path = std::string(dir) + "/log.txt";
      foil::openLog(path);
      const std::string filler(200, 'x');
      for (int i = 0; i < 8000; ++i) foil::logLine("line " + std::to_string(i) + " " + filler);
      foil::flushLog();
      CHECK(std::filesystem::file_size(path) <= 2 * 256 * 1024 + 4096);
      CHECK(foil::readFile(path).find("line 7999 ") != std::string::npos);
      // A line repeated (a poll failing every few seconds) is written once,
      // then how many more times when another line comes.
      std::filesystem::remove(path);
      foil::openLog(path);
      for (int i = 0; i < 5; ++i) foil::logLine("GET /api/pair/status status=404");
      CHECK(foil::logTail(4096).find("4 more times so far") != std::string::npos);
      foil::logLine("paired");
      foil::flushLog();
      const auto text = foil::readFile(path);
      CHECK(std::count(text.begin(), text.end(), '\n') == 3);
      CHECK(text.find("(the line before 4 more times)") != std::string::npos && text.find("paired") != std::string::npos);
      foil::openLog(""); // later tests log nowhere, as before
      std::filesystem::remove_all(dir);
    }
  }
    {
    // An unfinished install survives a round trip through state.json's queue.
    foil::PendingInstall pending{"Paper Moon", "https://x/icon", "nand", {}};
    foil::Item file;
    file.url = "https://x/a.nsp"; file.name = "Paper Moon"; file.type = "update"; file.size = 1234; file.version = 65536;
    pending.files.push_back(file);
    const auto back = foil::parsePending(foil::serializePending(pending));
    CHECK(back && back->name == "Paper Moon" && back->target == "nand" && back->files.size() == 1);
    CHECK(back->files[0].url == "https://x/a.nsp" && back->files[0].type == "update" && back->files[0].size == 1234 &&
          back->files[0].version == 65536);
    CHECK(!foil::parsePending("[]") && !foil::parsePending("garbage"));
    // A homebrew title keeps the format its address does not say.
    file.url = "https://x/api/homebrew/tv/download";
    file.format = "nsp";
    pending.files = {file};
    const auto title = foil::parsePending(foil::serializePending(pending));
    CHECK(title && title->files.size() == 1 && title->files[0].format == "nsp");
  }
  {
    // A server's dot colour: one of the eight, kept through a save; any
    // other number is no choice (its place in the list decides).
    auto parsed = foil::parseConfig(R"({"servers":[{"url":"https://a.example.com","color":3},)"
                                     R"({"url":"https://b.example.com","color":99},{"url":"https://c.example.com"}]})");
    CHECK(!parsed.error && parsed.value.servers.size() == 3);
    CHECK(parsed.value.servers[0].color == 3);
    CHECK(parsed.value.servers[1].color == -1 && parsed.value.servers[2].color == -1);
    const auto again = foil::parseConfig(foil::serializeConfig(parsed.value));
    CHECK(again.value.servers[0].color == 3 && again.value.servers[2].color == -1);
  }
  return failures;
}
