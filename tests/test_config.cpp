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
