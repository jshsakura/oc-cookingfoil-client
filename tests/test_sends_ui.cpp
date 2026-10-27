// 내 기기로 보내기 end to end: the console reads the server's queue, puts the
// sent game in the download queue and answers "accepted" on the next round;
// the list is asked again with its ETag.
#include "Test.hpp"
#include "core/State.hpp"
#include "ui/Application.hpp"
#include "MockServer.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
namespace {
size_t countOf(const std::vector<std::string> &requests, const std::string &start) {
  return size_t(std::count_if(requests.begin(), requests.end(), [&](const std::string &r) { return r.rfind(start, 0) == 0; }));
}
} // namespace
int runSendsUiTests(SDL_Renderer *renderer) {
  int failures = 0;
  Mock mock;
  foil::Server server;
  server.url = "http://127.0.0.1:" + std::to_string(mock.port);
  bool answered = false;
  std::vector<std::string> sent;
  mock.answer = [&](const std::string &request) -> Reply {
    if (request.rfind("GET /api/pair/status", 0) == 0) return {404, "", "{}"}; // no approval on this server
    if (request.rfind("GET /api/library/sections", 0) == 0) return {200, "", R"({"sections":[]})"};
    if (request.rfind("GET /api/device/sends", 0) == 0) {
      if (answered) return {200, "ETag: \"q1\"\r\n", R"({"serverId":"s1","items":[]})"};
      if (request.find("If-None-Match: \"q0\"") != std::string::npos) return {304, "", ""};
      return {200, "ETag: \"q0\"\r\n",
              R"({"serverId":"s1","items":[{"id":"a1b2c3d4e5f6","path":"Sent Game.nsp","name":"Sent Game",)"
              R"("titleId":"0100000000aa0000","size":1024,"sentAt":"2026-10-27T10:00:00Z","sentBy":"web",)"
              R"("url":")" + server.url + R"(/file/sent.nsp"}]})"};
    }
    if (request.rfind("POST /api/device/sends/ack", 0) == 0) {
      answered = true;
      return {200, "", R"({"ok":true,"removed":1})"};
    }
    if (request.rfind("GET /file", 0) == 0) return {404, "", ""};
    return {200, "", R"({"features":[]})"};
  };
  char temporary[] = "/tmp/cookingfoil-sends-XXXXXX";
  const char *root = mkdtemp(temporary);
  CHECK(root);
  if (!root) return failures;
  const std::string statePath = std::string(root) + "/cookingfoil.db";
  {
    foil::ui::Application app(renderer, "", statePath);
    CHECK(app.open());
    app.previewStorage(64ull << 30, 16ull << 30); // the host reads no free space
    const auto state = foil::loadState(statePath).value;
    app.setSession(std::make_unique<foil::net::Session>(server, state, statePath));
    app.refresh();
    for (const auto until = SDL_GetTicks() + 8000; !app.outcomeOf("Sent Game") && SDL_GetTicks() < until;) {
      app.poll();
      SDL_Delay(10);
    }
    CHECK(app.outcomeOf("Sent Game").has_value());
    for (const auto until = SDL_GetTicks() + 5000; !answered && SDL_GetTicks() < until;) {
      app.poll();
      SDL_Delay(10);
    }
    CHECK(answered);
    for (const auto &r : mock.take()) sent.push_back(r);
    const auto ack = std::find_if(sent.begin(), sent.end(),
                                  [](const std::string &r) { return r.rfind("POST /api/device/sends/ack", 0) == 0; });
    CHECK(ack != sent.end());
    if (ack != sent.end())
      CHECK(ack->find(R"({"ids":["a1b2c3d4e5f6"],"result":"accepted"})") != std::string::npos);
    // The answer goes before the list is read again, with the list's ETag.
    CHECK(countOf(sent, "GET /api/device/sends") >= 2);
    const auto again = std::find_if(ack == sent.end() ? sent.end() : ack + 1, sent.end(),
                                    [](const std::string &r) { return r.rfind("GET /api/device/sends", 0) == 0; });
    CHECK(again != sent.end() && again->find("If-None-Match: \"q0\"") != std::string::npos);
    // Taken once: still listed until answered, it never went in twice.
    CHECK(std::count(app.queuedNames().begin(), app.queuedNames().end(), "Sent Game") <= 1);
  }
  std::filesystem::remove_all(root);
  return failures;
}
