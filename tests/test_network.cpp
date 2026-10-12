#include "Test.hpp"
#include "core/State.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "net/Api.hpp"
#include <filesystem>
#include <algorithm>
#include <unistd.h>
int runNetworkTests() {
    int failures = 0;
    {
        const auto resolve = foil::net::connectionError(6, "https://shop.example:8443/api");
        CHECK(resolve.find("shop.example:8443") == 0);
        CHECK(resolve.find("DNS") != std::string::npos);
        CHECK(!foil::net::connectionError(7, "http://host").empty());
    }
    auto sections = foil::net::parseSections(R"({"sections":[{"items":[{"url":"https://host/game.nsz","name":"한글 日本 中文","title_id":"0100000000010000","app_version":131072,"size":4400000000,"languages":["ko","en"],"added_at":1728000000}]}]})");
    CHECK(!sections.error && sections.value.size() == 1);
    if (!sections.value.empty()) {
        CHECK(sections.value[0].name == "한글 日本 中文");
        CHECK(sections.value[0].size == 4400000000ULL);
        CHECK(sections.value[0].version == 131072);
        CHECK(sections.value[0].addedAt == 1728000000);
        CHECK(sections.value[0].languages.size() == 2);
    }
    CHECK(foil::net::parseSections("{}").error);
    {
        // Contract with the server: optional price summary, extra fields,
        // featured shelves, and a file repeated across sections kept once.
        const std::string body = R"({"sections":[{"items":[
            {"url":"https://h/a.nsp","name":"A","base_title_id":"0100000000010000","app_type":"base",
             "price_regular":"64,800원","price_discount":"45,360원","price_discount_ends":"2026-10-20T14:59:59Z",
             "price_country":"KR","install_size":123,"required_firmware":"16.0.0","latest_version":131072,"dlc_total":3}]},
          {"items":[{"url":"https://h/a.nsp","name":"A"}]}],
          "featured":[{"id":"pick","title":"이번 주 추천","title_ids":["0100000000010000","bad"]},{"id":"x","title":"","title_ids":[]}]})";
        const auto parsed = foil::net::parseSections(body);
        CHECK(parsed.value.size() == 1);
        if (!parsed.value.empty()) {
            const auto &a = parsed.value[0];
            CHECK(a.priceRegular == "64,800원" && a.priceDiscount == "45,360원" && a.priceCountry == "KR");
            CHECK(a.installSize == 123 && a.requiredFirmware == "16.0.0" && a.latestVersion == 131072 && a.dlcTotal == 3);
        }
        const auto featured = foil::net::parseFeatured(body);
        CHECK(featured.size() == 1 && featured[0].titleIds.size() == 1 && featured[0].title == "이번 주 추천");
        CHECK(foil::net::parseFeatured(R"({"sections":[]})").empty());
        const auto genres = foil::net::parseGenres(
            R"({"sections":[],"genres":{"Party":{"en":"Party","ko":"파티"},"Odd":{"en":"Odd"}}})");
        CHECK(genres.size() == 1 && genres.at("Party") == "파티");
        CHECK(foil::net::parseGenres(R"({"sections":[]})").empty());
        const auto labels = foil::net::parseNames(
            R"({"sections":[],"score_labels":{"Very Positive":{"en":"Very Positive","ko":"매우 긍정적"}}})", "score_labels");
        CHECK(labels.size() == 1 && labels.at("Very Positive") == "매우 긍정적");
        const auto rated = foil::net::parseSections(
            R"({"sections":[{"id":"all","items":[{"url":"https://h/a.nsp","name":"A","score":92,"score_count":1234,"score_label":"매우 긍정적","score_source":"steam"}]}]})");
        CHECK(!rated.error && rated.value.size() == 1 && rated.value[0].score == 92 &&
              rated.value[0].scoreCount == 1234 && rated.value[0].scoreLabel == "매우 긍정적");
        const auto ratedDetail = foil::net::parseDetail(R"({"name":"A","score":101,"scoreCount":5,"scoreLabel":"x"})");
        CHECK(ratedDetail.value.score == 100 && ratedDetail.value.scoreCount == 5);
        const auto detail = foil::net::parseDetail(R"({"requiredFirmware":"16.0.0","dlcTotal":2,"ratingContent":["폭력","공포"]})").value;
        CHECK(detail.requiredFirmware == "16.0.0" && detail.dlcTotal == 2 && detail.ratingContent == "폭력, 공포");
    }
    {
        auto ranked = foil::net::parseSections(R"({"sections":[{"items":[{"url":"https://h/a.nsp","name":"A","rank":12,"players":4}]}]})");
        CHECK(!ranked.value.empty() && ranked.value[0].rank == 12 && ranked.value[0].players == 4);
    }
    // Titles the server could not identify fall back to the file name.
    CHECK(foil::net::fileTitle("https://h/files/Hollow%20Knight%20%5B0100633007D48000%5D%5Bv0%5D.nsp") == "Hollow Knight");
    CHECK(foil::net::fileTitle("https://h/a/Paper Moon (KR) [v196608].xci?x=1") == "Paper Moon (KR)");
    CHECK(foil::net::fileTitle("https://h/a/[0100ABCD][v0].nsz").empty());
    {
        auto unknown = foil::net::parseSections(R"({"sections":[{"items":[{"url":"https://h/Tunic%20%5B0100F1000F000000%5D.nsp","name":"Unknown"}]}]})");
        CHECK(!unknown.value.empty() && unknown.value[0].name == "Tunic");
        auto keep = foil::net::parseSections(R"({"sections":[{"items":[{"url":"https://h/x.nsp","name":"Unknown"}]}]})");
        CHECK(!keep.value.empty() && keep.value[0].name == "x");
    }
    CHECK(foil::net::parseSections("{bad").error);
    CHECK(foil::net::parseSections(R"({"sections":[{"items":[{"url":"javascript:bad"},{"url":"https://host/a","size":-1}]}]})").value.size() == 1);
    CHECK(foil::net::parseDetail(R"({"name":"게임","description":null,"screenshots":[]})").value.name == "게임");
    {
        const auto priced = foil::net::parseDetail(R"({"price":{"country":"KR","regular":"64,800원","discount":"45,360원"}})").value;
        CHECK(priced.priceRegular == "64,800원" && priced.priceDiscount == "45,360원" && priced.priceCountry == "KR");
        CHECK(foil::net::parseDetail(R"({"price":null})").value.priceRegular.empty());
    }
    // Runs of blank lines from the store collapse to one paragraph break.
    CHECK(foil::net::tidyDescription("\n \n첫 줄   \r\n\n\n\n  \n둘째 줄\n\n") == "첫 줄\n\n둘째 줄");
    CHECK(foil::net::tidyDescription("한 줄").find('\n') == std::string::npos);
    foil::Server s; s.url = "https://host"; s.username = "user"; s.password = "pass"; s.cfClientId = "client"; s.cfClientSecret = "secret";
    foil::State state; state.deviceKey = std::string(64, 'a'); state.accessKeys[s.url] = "issued-key";
    auto headers = foil::net::authHeaders(s, state, "https://host/api/shop/sections");
    CHECK(std::find(headers.begin(), headers.end(), "Authorization: Basic dXNlcjpwYXNz") != headers.end());
    CHECK(std::find(headers.begin(), headers.end(), "CF-Access-Client-Secret: secret") != headers.end());
    CHECK(std::find(headers.begin(), headers.end(), "X-Access-Key: issued-key") != headers.end());
    // The server answers in the app's language (game names, descriptions).
    CHECK(std::any_of(headers.begin(), headers.end(), [](const std::string& h) { return h.rfind("Accept-Language: ", 0) == 0; }));
    auto foreign = foil::net::authHeaders(s, state, "https://other.example.com/icon.jpg");
    CHECK(foreign.size() == 1 && foreign[0].find("Version:") == 0);
    char temporary[] = "/tmp/cookingfoil-state-XXXXXX";
    const char* root = mkdtemp(temporary);
    CHECK(root);
    if (root) {
        const auto path = std::string(root) + "/state.json";
        const auto fresh = foil::loadState(path);
        CHECK(!fresh.error && fresh.value.deviceKey.size() == 64);
        CHECK(fresh.value.deviceKey != std::string(64, '0'));
        CHECK(foil::loadState(path).value.deviceKey == fresh.value.deviceKey);
        auto changed = fresh.value; changed.accessKeys[s.url] = "issued-key";
        foil::Error error; CHECK(foil::saveState(path, changed, error));
        CHECK(foil::loadState(path).value.accessKeys.at(s.url) == "issued-key");
        // Error reports go to the server unless turned off in 설정.
        CHECK(fresh.value.reportErrors);
        changed.reportErrors = false;
        CHECK(foil::saveState(path, changed, error));
        CHECK(!foil::loadState(path).value.reportErrors);
        // HOME Menu shortcuts for retro games and homebrew: on unless turned off.
        CHECK(fresh.value.makeShortcuts);
        changed.makeShortcuts = false;
        CHECK(foil::saveState(path, changed, error));
        CHECK(!foil::loadState(path).value.makeShortcuts);
        // Where installs go, chosen in 설정; empty follows config.json.
        CHECK(fresh.value.installTarget.empty());
        changed.installTarget = "nand";
        CHECK(foil::saveState(path, changed, error));
        CHECK(foil::loadState(path).value.installTarget == "nand");
        std::filesystem::remove_all(root);
    }
    return failures;
}
