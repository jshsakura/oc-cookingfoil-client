#include "Test.hpp"
#include "core/State.hpp"
#include "net/Api.hpp"
#include "core/Log.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <thread>
#include <filesystem>
#include <vector>
// A real loopback HTTP connection covers approval, one-time key persistence,
// conditional requests and retention of the previous immutable catalog.
int runSessionTests() {
    int failures = 0;
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0); if (listener < 0) return failures;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(listen(listener, 5) == 0);
    socklen_t length = sizeof(address); getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    std::vector<std::string> requests;
    std::thread mock([&] {
        // /api/shop/info comes before each catalog request; it is answered
        // on the side and not counted in the sequence below.
        for (int i = 0, accepted = 0; i < 7 && accepted < 20; ++accepted) {
            pollfd ready{listener, POLLIN, 0}; if (poll(&ready, 1, 5000) <= 0) break;
            const int client = accept(listener, nullptr, nullptr); if (client < 0) break;
            char buffer[4096]; std::string request;
            while (request.find("\r\n\r\n") == std::string::npos) {
                const auto size = recv(client, buffer, sizeof(buffer), 0); if (size <= 0) break; request.append(buffer, size);
            }
            if (request.rfind("GET /api/shop/info ", 0) == 0) {
                const std::string info = R"({"features":["retro"]})";
                const auto reply = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(info.size()) + "\r\n\r\n" + info;
                send(client, reply.data(), reply.size(), MSG_NOSIGNAL); close(client);
                continue;
            }
            requests.push_back(request);
            const auto body = i == 0 ? R"({"status":"pending"})" : i == 1 ? R"({"status":"approved","accessKey":"issued-once"})" : i == 2 ? R"({"sections":[{"items":[{"url":"https://example.com/game.nsz","name":"한글","size":4400000000}]}]})" : i == 6 ? "{bad-json" : i == 4 ? "" : R"({"status":"approved"})";
            std::string response = std::string("HTTP/1.1 ") + (i == 4 ? "304 Not Modified" : "200 OK") + "\r\nConnection: close\r\nETag: \"catalog-v1\"\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(std::string(body).size()) + "\r\n\r\n" + body;
            send(client, response.data(), response.size(), MSG_NOSIGNAL); close(client);
            ++i;
        }
    });
    char temporary[] = "/tmp/cookingfoil-session-XXXXXX"; const char* root = mkdtemp(temporary);
    CHECK(root);
    if (root) {
        const std::string path = std::string(root) + "/state.json";
        foil::Server server; server.url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        const auto initial = foil::loadState(path);
        foil::net::Session session(server, initial.value, path); foil::Error error;
        CHECK(!session.refresh(error)); CHECK(session.pairingPending()); CHECK(session.items().empty());
        CHECK(session.refresh(error)); CHECK(!session.pairingPending()); CHECK(!error);
        CHECK(session.items().size() == 1); CHECK(session.items().front().name == "한글");
        CHECK(session.features().count("retro"));
        CHECK(foil::loadState(path).value.accessKeys.at(server.url) == "issued-once");
        CHECK(session.refresh(error)); CHECK(session.items().size() == 1);
        CHECK(!session.refresh(error)); CHECK(error); CHECK(session.items().size() == 1);
        std::filesystem::remove_all(root);
    }
    mock.join(); close(listener);
    CHECK(requests.size() == 7);
    if (requests.size() == 7) {
        CHECK(requests[2].find("X-Access-Key: issued-once") != std::string::npos);
        CHECK(requests[4].find("If-None-Match: \"catalog-v1\"") != std::string::npos);
    }
    // A patch list still being scanned: what is there so far, when to ask
    // again, and no ETag kept, so the next ask gets the full list.
    {
        const int scan = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in at{}; at.sin_family = AF_INET; at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(scan >= 0 && bind(scan, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0 && listen(scan, 2) == 0);
        socklen_t size = sizeof(at); getsockname(scan, reinterpret_cast<sockaddr*>(&at), &size);
        std::vector<std::string> asked;
        std::thread server([&] {
            for (int i = 0; i < 2; ++i) {
                pollfd ready{scan, POLLIN, 0}; if (poll(&ready, 1, 5000) <= 0) break;
                const int client = accept(scan, nullptr, nullptr); if (client < 0) break;
                char buffer[4096]; std::string request;
                while (request.find("\r\n\r\n") == std::string::npos) {
                    const auto got = recv(client, buffer, sizeof(buffer), 0); if (got <= 0) break; request.append(buffer, got);
                }
                asked.push_back(request);
                const std::string body = i == 0 ? R"({"patches":[],"scanning":true})" : R"({"patches":[]})";
                const auto reply = std::string("HTTP/1.1 200 OK\r\nConnection: close\r\n") + (i == 0 ? "Retry-After: 7\r\n" : "") + "ETag: \"p" + std::to_string(i) +
                                   "\"\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
                send(client, reply.data(), reply.size(), MSG_NOSIGNAL); close(client);
            }
        });
        foil::Server server2; server2.url = "http://127.0.0.1:" + std::to_string(ntohs(at.sin_port));
        foil::net::Session patches(server2, {}, "");
        CHECK(!patches.patches().error); CHECK(patches.patchesRetryAfter() == 7);
        CHECK(!patches.patches().error); CHECK(patches.patchesRetryAfter() == 0);
        server.join(); close(scan);
        CHECK(asked.size() == 2 && asked[1].find("If-None-Match") == std::string::npos);
    }
    {
        // The log (and the bug report made from it) keeps no server address:
        // host and query go, the path tells what it was.
        using foil::withoutAddresses;
        CHECK(withoutAddresses("STREAM https://dl.example.kr:38443/Game%20[0100].nsz?exp=1&sig=abc status=206 curl=0") ==
              "STREAM /Game%20[0100].nsz status=206 curl=0");
        CHECK(withoutAddresses("GET http://127.0.0.1:38000/api/retro/games/x/icon status=404") ==
              "GET /api/retro/games/x/icon status=404");
        CHECK(withoutAddresses("connect to https://shop.example.com failed") == "connect to / failed");
        CHECK(withoutAddresses("Could not resolve host: dl.example.kr") == "Could not resolve host: (server)");
        CHECK(withoutAddresses("httpd ok, nothing to hide") == "httpd ok, nothing to hide");
        // Credentials never reach a bug report.
        CHECK(withoutAddresses("sent Authorization: Bearer abc.DEF-1 then") == "sent Authorization: Bearer *** then");
        CHECK(withoutAddresses("{\"accessKey\":\"0123abcd\",\"x\":1}") == "{\"accessKey\":\"***\",\"x\":1}");
        CHECK(withoutAddresses("token eyJhbGc.eyJzdWI.sig_1 ok") == "token *** ok");
        CHECK(withoutAddresses("password=hunter2 & more") == "password=*** & more");
        CHECK(withoutAddresses("hash 3f2a ok, eyJust a word") == "hash 3f2a ok, eyJust a word");
    }
    return failures;
}
