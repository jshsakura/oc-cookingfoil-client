#include "Test.hpp"
#include "core/State.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
int runConfigTests() {
    int failures = 0;
    CHECK(foil::normalizeUrl("shop.example.com/SHOP.TFL/") == "https://shop.example.com");
    CHECK(foil::normalizeUrl("http://host:9080/prefix/shop.json") == "http://host:9080/prefix");
    CHECK(foil::normalizeUrl("ftp://host").empty());
    CHECK(foil::normalizeUrl("https://host\r\nX-Key: secret").empty());
    CHECK(foil::normalizeUrl("https://user:pass@host").empty());
    auto valid = foil::parseConfig(R"({"servers":[{"url":"host/shop.tfl","unknown":42}],"future":true})");
    CHECK(!valid.error);
    CHECK(valid.value.servers.size() == 1);
    CHECK(valid.value.servers[0].url == "https://host");
    CHECK(valid.value.servers[0].enabled);
    CHECK(valid.value.language == "ko" && valid.value.installTarget == "sd");
    CHECK(foil::parseConfig(R"({"servers":[{}]})").error);
    CHECK(foil::parseConfig(R"({"servers":[{"url":42}]})").error);
    CHECK(!foil::parseConfig(R"({"language":"ja"})").error);
    CHECK(foil::parseConfig(R"({"language":"fr"})").error);
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
        config.servers.push_back({"원격 서버 日本 中文", "https://shop.example.com:8443/prefix", "user",
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
        saved.servers.push_back({"내 서버", "https://shop.example.com", "player", "pass word", "", "", true});
        std::string error;
        CHECK(foil::saveConfig(path, saved, error));
        const auto loaded = foil::loadConfig(path);
        CHECK(!loaded.error && loaded.value.servers.size() == 1);
        CHECK(loaded.value.servers[0].title == "내 서버" && loaded.value.servers[0].password == "pass word");
        CHECK(loaded.value.language == "en");
        // A save cut after the old file went aside: config.json is missing and
        // its servers come back from what is beside it, not an empty file.
        std::filesystem::rename(path, path + ".old");
        std::filesystem::remove(path + ".bak");
        auto restored = foil::loadConfig(path);
        CHECK(!restored.error && !restored.created && restored.value.servers.size() == 1);
        CHECK(!std::filesystem::exists(path + ".old"));
        // An older build already made an empty one over it: healed the same way.
        CHECK(foil::saveConfig(path, saved, error)); // .bak has the server again
        { std::ofstream f(path); f << foil::serializeConfig(foil::Config{}); }
        CHECK(foil::loadConfig(path).value.servers.size() == 1);
        // The new copy, complete before any rename, wins over the older one.
        foil::Config newer = saved;
        newer.servers[0].title = "새 이름";
        { std::ofstream f(path + ".tmp"); f << foil::serializeConfig(newer); }
        std::filesystem::remove(path);
        CHECK(foil::loadConfig(path).value.servers[0].title == "새 이름");
        // Servers removed on purpose stay removed: the save writes .bak alike.
        CHECK(foil::saveConfig(path, foil::Config{}, error));
        CHECK(foil::loadConfig(path).value.servers.empty());
        // A config.json left empty (cut while being made) reads as missing.
        CHECK(foil::saveConfig(path, saved, error));
        { std::ofstream f(path); }
        CHECK(foil::loadConfig(path).value.servers.size() == 1);
        CHECK(foil::saveConfig(path, foil::Config{}, error));
        // A broken copy beside it is not taken.
        std::filesystem::remove(path);
        std::filesystem::remove(path + ".bak");
        { std::ofstream f(path + ".old"); f << "{ broken"; }
        auto fresh = foil::loadConfig(path);
        CHECK(!fresh.error && fresh.created && fresh.value.servers.empty());
        std::filesystem::remove_all(root);
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
  }
  return failures;
}
