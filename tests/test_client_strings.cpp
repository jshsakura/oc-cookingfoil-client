// A server's screen text: what tr() takes from it, what it never takes, and
// how the answer is kept on the card and asked again.
#include "MockServer.hpp"
#include "Test.hpp"
#include "core/I18n.hpp"
#include "core/I18nChecks.hpp"
#include "core/Version.hpp"
#include "net/ClientStringsApi.hpp"
#include <filesystem>
using namespace foil;
int runClientStringsTests() {
  int failures = 0;
  {
    const auto reply = net::parseStringsReply(R"({"lang":"ko","base":null,"strings":{"Language":"언어 설정","n":3}})");
    CHECK(reply && reply->lang == "ko" && reply->base.empty() && reply->strings.size() == 1 &&
          reply->strings.at("Language") == "언어 설정");
    CHECK(!net::parseStringsReply(R"({"lang":"ko"})"));
    CHECK(!net::parseStringsReply(R"({"strings":{}})"));
    CHECK(!net::parseStringsReply("<html>"));
  }
  {
    const std::string builtIn = (setLanguage(Language::Korean), tr("Language"));
    const char *locked = i18n::lockedKeys().front();
    const std::string lockedText = tr(locked);
    const char *http = "Could not reach the server. HTTP {0}";
    const auto taken = setServerTexts(Language::Korean, {{"Language", "언어 설정"},
                                                         {locked, "바꾼 문구"},
                                                         {"No such key anywhere", "없음"},
                                                         {http, "서버 오류 {1}"},
                                                         {"Settings", ""}});
    CHECK(taken == 1);
    CHECK(std::string(tr("Language")) == "언어 설정");
    CHECK(std::string(tr(locked)) == lockedText); // locked keys keep the built-in text
    CHECK(trf(http, {"500"}).find("500") != std::string::npos); // wrong placeholders: built-in
    setLanguage(Language::Japanese); // another language's text is its own
    CHECK(std::string(tr("Language")) != "언어 설정");
    setLanguage(Language::English);
    CHECK(setServerTexts(Language::English, {{"Language", "Tongue"}}) == 0);
    CHECK(std::string(tr("Language")) == "Language");
    setLanguage(Language::Korean);
    const char *held = tr("Language");
    setServerTexts(Language::Korean, {}); // a later answer without the key: back to built-in
    CHECK(std::string(tr("Language")) == builtIn);
    CHECK(std::string(held) == "언어 설정"); // what was handed out stays readable
  }
  {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "cookingfoil-strings-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    DownloadStore store;
    std::string error;
    CHECK(store.open((dir / "cookingfoil.db").string(), error));
    Mock mock;
    int status = 200;
    std::string lang = "ko";
    mock.answer = [&](const std::string &) {
      return Reply{status, status == 200 ? "ETag: \"one\"\r\n" : "",
                   status == 200 ? R"({"lang":")" + lang + R"(","base":null,"strings":{"Language":"언어 설정"}})" : ""};
    };
    Server server;
    server.url = "http://127.0.0.1:" + std::to_string(mock.port);
    State state;
    // Nothing kept, no server: nothing.
    CHECK(net::serverStrings(store, nullptr, state, "ko", nullptr).empty());
    // A 200 is used and kept with its ETag for this version.
    auto texts = net::serverStrings(store, &server, state, "ko", nullptr);
    CHECK(texts.size() == 1 && texts["Language"] == "언어 설정");
    auto requests = mock.take();
    CHECK(requests.size() == 1 && requests[0].find("GET /api/client/strings?lang=ko&v=") == 0 &&
          requests[0].find("If-None-Match") == std::string::npos);
    const auto row = store.clientStrings("ko");
    CHECK(row && row->etag == "\"one\"" && row->version == appVersion() && row->server == server.url);
    // Kept text needs no server; a 304 keeps it and was asked with the ETag.
    CHECK(net::serverStrings(store, nullptr, state, "ko", nullptr).size() == 1);
    status = 304;
    CHECK(net::serverStrings(store, &server, state, "ko", nullptr).size() == 1);
    requests = mock.take();
    CHECK(requests.size() == 1 && requests[0].find("If-None-Match: \"one\"") != std::string::npos);
    // Another server is asked without that ETag; a failure keeps the row.
    Server other = server;
    other.url = "http://localhost:" + std::to_string(mock.port);
    status = 500;
    CHECK(net::serverStrings(store, &other, state, "ko", nullptr).size() == 1);
    requests = mock.take();
    CHECK(requests.size() == 1 && requests[0].find("If-None-Match") == std::string::npos);
    // An answer for another language is not taken.
    status = 200;
    lang = "ja";
    CHECK(net::serverStrings(store, &server, state, "fr", nullptr).empty() && !store.clientStrings("fr"));
    // Every kept language comes back in one read.
    CHECK(store.saveClientStrings("ja", {server.url, appVersion(), "", R"({"lang":"ja","strings":{"Language":"言語の設定"}})"}, error));
    auto kept = net::keptStrings(store);
    CHECK(kept.size() == 2 && kept["ko"]["Language"] == "언어 설정" && kept["ja"]["Language"] == "言語の設定");
    // A row from another version of the app is not used.
    CHECK(store.saveClientStrings("ko", {server.url, "0.0.1-old", "\"one\"", row->body}, error));
    CHECK(net::serverStrings(store, nullptr, state, "ko", nullptr).empty());
    CHECK(net::keptStrings(store).size() == 1);
    store.close();
    fs::remove_all(dir);
  }
  return failures;
}
