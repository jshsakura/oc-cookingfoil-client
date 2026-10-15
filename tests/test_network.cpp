#include "net/Segments.hpp"
#include "Test.hpp"
#include <fstream>
#include "core/State.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "net/Api.hpp"
#include "net/Socket.hpp"
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
    {
        // Game by game: strings holding braces, brackets and escaped quotes,
        // nested objects, a file repeated in another section, and the small
        // tables beside the sections all read as from one tree.
        const auto catalog = foil::net::parseCatalog(R"( {
          "genres": {"Action": {"en": "Action", "ko": "액션"}},
          "sections": [
            {"id": "new", "items": [
              {"url": "https://h/a.nsp", "name": "A \"quoted\" {brace} [x]", "icons": {"ko": "https://h/i.jpg"},
               "categories": ["Action"], "size": 10},
              {"url": "https://h/b.nsp", "name": "B\\path", "size": 20}
            ]},
            {"id": "all", "items": [{"url": "https://h/a.nsp", "name": "again"}, {"url": "ftp://bad"}]}
          ],
          "featured": [],
          "score_labels": {}
        } )");
        CHECK(!catalog.error);
        CHECK(catalog.value.items.size() == 2);
        CHECK(catalog.value.items[0].name == "A \"quoted\" {brace} [x]");
        CHECK(catalog.value.items[0].categories.size() == 1);
        CHECK(catalog.value.items[1].name == "B\\path");
        CHECK(catalog.value.items[1].size == 20);
        CHECK(catalog.value.genres.count("Action") && catalog.value.genres.at("Action").at("ko") == "액션");
        // Cut short, or no sections at all: the list's format error.
        CHECK(foil::net::parseCatalog(R"({"sections":[{"items":[{"url":"https://h/a.nsp")").error);
        CHECK(foil::net::parseCatalog(R"({"genres":{}})").error);
        CHECK(foil::net::parseCatalog("[]").error);
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
    // The operator's notes come apart from the description; null is none.
    CHECK(foil::net::parseDetail(R"({"notes":"데이터 49GB 필요  \n"})").value.notes == "데이터 49GB 필요");
    CHECK(foil::net::parseDetail(R"({"notes":null})").value.notes.empty());
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
        const auto path = std::string(root) + "/cookingfoil.db";
        const auto fresh = foil::loadState(path);
        CHECK(!fresh.error && fresh.value.deviceKey.size() == 64);
        CHECK(fresh.value.deviceKey != std::string(64, '0'));
        CHECK(foil::loadState(path).value.deviceKey == fresh.value.deviceKey);
        auto changed = fresh.value; changed.accessKeys[s.url] = "issued-key";
        foil::Error error; CHECK(foil::saveState(path, changed, error));
        CHECK(foil::loadState(path).value.accessKeys.at(s.url) == "issued-key");
        // Pair secrets per server: kept when 64 hex characters, dropped otherwise.
        changed.pairSecrets[s.url] = std::string(64, 'b');
        changed.pairSecrets["https://other.example.com"] = "short";
        CHECK(foil::saveState(path, changed, error));
        {
            const auto secrets = foil::loadState(path).value.pairSecrets;
            CHECK(secrets.size() == 1 && secrets.count(s.url) && secrets.at(s.url) == std::string(64, 'b'));
        }
        changed.pairSecrets.clear();
        // A key that is not one: a new key, the old one's server keys gone,
        // the player's settings kept; no error.
        {
            auto broken = changed; broken.deviceKey = "not-a-key"; broken.language = "ja";
            CHECK(foil::saveState(path, broken, error));
            const auto rekeyed = foil::loadState(path);
            CHECK(foil::takeRebound() == foil::Rebound::Changed);
            CHECK(!rekeyed.error && rekeyed.value.deviceKey.size() == 64);
            CHECK(rekeyed.value.accessKeys.empty() && rekeyed.value.language == "ja");
            CHECK(foil::loadState(path).value.deviceKey == rekeyed.value.deviceKey); // saved
        }
        // Another file in its place (not a database, or empty): started over
        // with a new key, no error, and readable next time.
        for (const std::string& other : {std::string("\x89PNG garbage"), std::string("[1,2,3]"), std::string()}) {
            std::ofstream(path, std::ios::binary | std::ios::trunc) << other;
            const auto over = foil::loadState(path);
            foil::takeRebound(); // a file that is not a database: Recreated
            CHECK(!over.error && over.created && over.value.deviceKey.size() == 64);
            CHECK(foil::loadState(path).value.deviceKey == over.value.deviceKey);
        }
        // A save cut off between its two renames: the copy it left beside is
        // read instead.
        CHECK(foil::saveState(path, changed, error));
        std::filesystem::rename(path, path + ".old");
        CHECK(foil::loadState(path).value.deviceKey == changed.deviceKey);
        std::filesystem::remove(path + ".old");
        CHECK(foil::saveState(path, changed, error));
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
        // The approval note, per server; none until one is typed.
        CHECK(fresh.value.pairMessages.empty());
        changed.pairMessages[s.url] = "철수 거실 기기";
        CHECK(foil::saveState(path, changed, error));
        const auto noted = foil::loadState(path).value;
        CHECK(noted.pairMessages.size() == 1 && noted.pairMessages.at(s.url) == "철수 거실 기기");
        CHECK(noted.accessKeys.at(s.url) == "issued-key");
        changed.pairMessages.clear();
        CHECK(foil::saveState(path, changed, error));
        CHECK(foil::loadState(path).value.pairMessages.empty());
        std::filesystem::remove_all(root);
    }
    {
        using foil::net::SegmentPlan;
        constexpr uint64_t MB = 1ull << 20;
        // Small files: one part whatever the setting.
        CHECK(SegmentPlan(0, 63 * MB, 8).segments().size() == 1);
        // Eight parts that cover the file exactly, 4 KB aligned inside.
        SegmentPlan plan(0, 1000 * MB + 123, 8);
        CHECK(plan.segments().size() == 8);
        uint64_t at = 0;
        for (const auto &s : plan.segments()) {
            CHECK(s.start == at && s.next == s.start && s.end > s.start);
            at = s.end;
        }
        CHECK(at == 1000 * MB + 123);
        CHECK(plan.segments()[1].start % 4096 == 0);
        // Part 0 finishes: it takes the back half of the part with the most left.
        plan.segments()[0].next = plan.segments()[0].end;
        plan.segments()[3].next += 10 * MB; // part 3 has the least left among the busy
        std::vector<bool> busy(8, true);
        busy[0] = false;
        const int taken = plan.take(busy);
        CHECK(taken == 8);
        const auto &back = plan.segments()[8];
        CHECK(back.end == plan.segments()[7].end + 0 || back.end > back.start);
        CHECK(plan.segments()[taken].start == plan.segments()[taken].next);
        // The split part now stops where the new one begins.
        bool joined = false;
        for (int i = 1; i < 8; ++i) joined |= plan.segments()[i].end == back.start;
        CHECK(joined);
        // Nothing is split below 2 x 32 MB left.
        SegmentPlan small(0, 120 * MB, 2);
        small.segments()[0].next = small.segments()[0].end;
        small.segments()[1].next = small.segments()[1].end - 63 * MB;
        CHECK(small.take({false, true}) == -1);
        // An idle part with bytes left is handed out before any split.
        SegmentPlan idle(0, 512 * MB, 4);
        CHECK(idle.take({true, false, true, true}) == 1);
        // Saved and loaded as it was; nonsense is refused.
        std::vector<foil::net::Segment> back2;
        CHECK(SegmentPlan::load(plan.save(), back2) && back2.size() == plan.segments().size());
        CHECK(back2[3].next == plan.segments()[3].next);
        CHECK(!SegmentPlan::load("5-3-4;", back2));
        CHECK(plan.received() > 0 && !plan.done());
        // Connections: automatic by the server's address, or as set.
        CHECK(foil::net::connectionsFor("http://192.0.2.10:38000", 0) == 2);
        CHECK(foil::net::connectionsFor("http://198.51.100.10:38000", 0) == 2);
        CHECK(foil::net::connectionsFor("http://127.0.0.1:38000", 0) == 2);
        CHECK(foil::net::connectionsFor("http://172.20.1.1/", 0) == 2);
        CHECK(foil::net::connectionsFor("http://172.32.1.1/", 0) == 4);
        CHECK(foil::net::connectionsFor("https://tinfoil.example.com", 0) == 4);
        CHECK(foil::net::connectionsFor("https://tinfoil.example.com", 4) == 4);
        CHECK(foil::net::connectionsFor("http://10.0.0.2", 3) == 2); // not a choice: automatic
    }
    {
        // A server restart, heard from any reply's header: downloads treat it
        // as the way being down (they wait), it counts once per id, and a
        // success without the header ends it.
        const auto hear = [](const std::string &line) { foil::net::heardHeader(line.data(), line.size()); };
        const auto before = foil::net::restartSerial();
        CHECK(!foil::net::serverRestarting());
        hear("HTTP/1.1 503 Service Unavailable\r\n");
        hear("X-CookingFoil-Restarting: 20; id=r1\r\n");
        hear("\r\n");
        CHECK(foil::net::serverRestarting() && foil::net::linkDown());
        CHECK(foil::net::restartSerial() == before + 1);
        // The same restart on a piece still coming (206 with the header): still on, not counted again.
        hear("HTTP/1.1 206 Partial Content\r\n");
        hear("x-cookingfoil-restarting: 12; id=r1\r\n");
        hear("\r\n");
        CHECK(foil::net::serverRestarting() && foil::net::restartSerial() == before + 1);
        // Back: a success with no header.
        hear("HTTP/1.1 200 OK\r\n");
        hear("Content-Length: 2\r\n");
        hear("\r\n");
        CHECK(!foil::net::serverRestarting() && !foil::net::linkDown());
        // A later restart is a new one.
        hear("HTTP/1.1 503 Service Unavailable\r\n");
        hear("X-CookingFoil-Restarting: 20; id=r2\r\n");
        hear("\r\n");
        CHECK(foil::net::restartSerial() == before + 2);
        hear("HTTP/1.1 204 No Content\r\n");
        hear("\r\n");
        CHECK(!foil::net::serverRestarting());
    }
    {
        // The download token comes in any reply's header; it goes as a bearer
        // token to the download host (https, another host) and nowhere else,
        // and the server's own keys never go there. A refusal is noted until
        // a new token comes. Every request to the server says what the client
        // can do.
        const auto hear = [](const std::string &line) { foil::net::heardHeader(line.data(), line.size()); };
        hear("HTTP/1.1 200 OK\r\n");
        hear("X-CookingFoil-Download-Token: abc.def.ghi\r\n");
        hear("\r\n");
        CHECK(foil::net::downloadToken() == "abc.def.ghi" && !foil::net::downloadTokenRejected());
        foil::Server shop;
        shop.url = "https://shop.example";
        shop.username = "user";
        shop.password = "pass";
        foil::State state;
        state.deviceKey = std::string(64, 'a');
        const auto has = [](const std::vector<std::string> &headers, const std::string &text) {
            return std::any_of(headers.begin(), headers.end(), [&](const std::string &h) { return h.find(text) != std::string::npos; });
        };
        const auto dl = foil::net::downloadHeaders(shop, state, "https://dl.example:38443/games/x.nsz");
        CHECK(has(dl, "Authorization: Bearer abc.def.ghi") && !has(dl, "Basic") && !has(dl, "X-Device-Key"));
        CHECK(!has(foil::net::downloadHeaders(shop, state, "http://dl.example/games/x.nsz"), "Bearer"));
        // A picture on another host gets nothing: not the token, not the keys.
        const auto picture = foil::net::authHeaders(shop, state, "https://images.example/icon.jpg");
        CHECK(!has(picture, "Authorization") && !has(picture, "X-Device-Key") && !has(picture, "X-Access-Key"));
        CHECK(!has(foil::net::downloadHeaders(shop, state, "https://shop.example/api/x"), "Bearer"));
        const auto own = foil::net::authHeaders(shop, state, "https://shop.example/api/shop/sections");
        CHECK(has(own, "X-CookingFoil-Features: direct-token") && has(own, "Basic") && !has(own, "Bearer"));
        hear("HTTP/1.1 401 Unauthorized\r\n");
        hear("X-CookingFoil-Token: expired\r\n");
        hear("\r\n");
        CHECK(foil::net::downloadTokenRejected());
        hear("HTTP/1.1 304 Not Modified\r\n");
        hear("X-CookingFoil-Download-Token: new.token.x\r\n");
        hear("\r\n");
        CHECK(foil::net::downloadToken() == "new.token.x" && !foil::net::downloadTokenRejected());
    }
    return failures;
}
