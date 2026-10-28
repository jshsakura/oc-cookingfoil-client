#include "Test.hpp"
#include "core/State.hpp"
#include "net/Api.hpp"
#include "ui/Application.hpp"
#include "MockServer.hpp"
#include <SDL.h>
#include <atomic>
#include <filesystem>
#include <string>
// A console showing its list loses its key on the server (an admin took it
// back), shows the QR code, and once it is approved shows the list again on
// its own: the next approval poll brings the key and the list with it.
int runRepairUiTests(SDL_Renderer *renderer) {
  int failures = 0;
  Mock mock;
  std::atomic<int> phase{0}; // 0 approved, 1 taken back, 2 approved again
  const std::string list = R"({"sections":[{"items":[{"url":"https://example.com/game.nsz","name":"Game","title_id":"0100000000000000"}]}]})";
  mock.answer = [&](const std::string &request) -> Reply {
    const auto starts = [&](const char *text) { return request.rfind(text, 0) == 0; };
    if (starts("GET /api/library/info")) return {200, "", R"({"features":["session-token"]})"};
    // A session token for each key; the first key's dies with it.
    if (starts("GET /api/session")) {
      if (request.find("X-Access-Key: second-key") != std::string::npos) return {200, "", R"({"token":"cfs_second","expiresAt":0})"};
      if (phase == 0) return {200, "", R"({"token":"cfs_first","expiresAt":0})"};
      return {401, "X-CookingFoil-Auth: device-not-approved\r\n", "{}"};
    }
    if (starts("POST /api/pair/request")) return {200, "", "{}"};
    if (starts("GET /api/pair/status")) {
      if (phase == 0) return {200, "", R"({"status":"approved"})"};
      if (phase == 1) return {200, "", R"({"status":"pending","pairUrl":"https://example.com/admin/pair/1","pairCode":"1234ABCD","expiresAt":2000000000000})"};
      return {200, "", R"({"status":"approved","accessKey":"second-key"})"};
    }
    if (starts("GET /api/library/sections")) {
      const bool second = request.find("X-Access-Key: second-key") != std::string::npos ||
                          request.find("Bearer cfs_second") != std::string::npos;
      if (phase >= 1 && request.find("Bearer cfs_first") != std::string::npos)
        return {401, "X-CookingFoil-Session: revoked\r\nX-CookingFoil-Auth: session-revoked\r\n", "{}"};
      if (phase == 1 || (phase == 2 && !second))
        return {401, "X-CookingFoil-Auth: device-not-approved\r\n", R"({"error":"not approved"})"};
      // The list did not change while the console waited: a 304 to its ETag.
      if (request.find("If-None-Match: \"one\"") != std::string::npos) return {304, "ETag: \"one\"\r\n", ""};
      return {200, "ETag: \"one\"\r\n", list};
    }
    return {404, "", "{}"};
  };
  char temporary[] = "/tmp/cookingfoil-repair-ui-XXXXXX";
  const char *root = mkdtemp(temporary);
  CHECK(root);
  if (!root) return failures;
  const std::string statePath = std::string(root) + "/cookingfoil.db";
  foil::Server server;
  server.url = "http://127.0.0.1:" + std::to_string(mock.port);
  {
    auto state = foil::loadState(statePath).value;
    state.accessKeys[server.url] = "first-key";
    foil::Error error;
    CHECK(foil::saveState(statePath, state, error));
  }
  {
    foil::ui::Application app(renderer, "", statePath);
    CHECK(app.open());
    app.setSession(std::make_unique<foil::net::Session>(server, foil::loadState(statePath).value, statePath));
    const auto until = [&](auto done, uint32_t ms) {
      const auto deadline = SDL_GetTicks() + ms;
      while (!done() && SDL_GetTicks() < deadline) {
        app.poll();
        app.render();
        SDL_Delay(10);
      }
    };
    app.refresh();
    until([&] { return app.groupCount() == 1; }, 3000);
    CHECK(app.groupCount() == 1);
    phase = 1;
    app.refresh();
    until([&] { return app.pairingPending(); }, 3000);
    CHECK(app.pairingPending());
    phase = 2;
    // The first approval poll comes 5 seconds after the QR code shows.
    until([&] { return !app.pairingPending(); }, 9000);
    CHECK(!app.pairingPending());
    CHECK(app.groupCount() == 1);
    CHECK(foil::loadState(statePath).value.accessKeys.at(server.url) == "second-key");
  }
  std::filesystem::remove_all(root);
  return failures;
}
