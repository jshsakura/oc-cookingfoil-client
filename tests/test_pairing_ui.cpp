#include "Test.hpp"
#include "core/State.hpp"
#include "ui/Application.hpp"
#include <arpa/inet.h>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <thread>
// A note typed on the approval screen is kept per server in state.json and
// goes out with a new approval request; with none, nothing is posted.
int runPairMessageUiTests(SDL_Renderer *renderer) {
  int failures = 0;
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(listener >= 0);
  if (listener < 0)
    return failures;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
  CHECK(listen(listener, 5) == 0);
  socklen_t length = sizeof(address);
  getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length);
  foil::Server server;
  server.url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
  const std::string pending = "{\"status\":\"pending\",\"pairUrl\":\"" + server.url + "/pair/" + std::string(32, 'a') +
                              "\",\"pairCode\":\"1234ABCD\",\"expiresAt\":2000000000000}";
  std::mutex lock;
  std::vector<std::string> posts;
  std::atomic<bool> stop{false};
  std::thread mock([&] {
    while (!stop) {
      pollfd ready{listener, POLLIN, 0};
      if (::poll(&ready, 1, 50) <= 0)
        continue;
      const int client = accept(listener, nullptr, nullptr);
      if (client < 0)
        break;
      char buffer[4096];
      std::string request;
      size_t headerEnd = std::string::npos, want = 0;
      while (true) {
        if (headerEnd == std::string::npos && (headerEnd = request.find("\r\n\r\n")) != std::string::npos) {
          const auto field = request.find("Content-Length: ");
          want = field != std::string::npos && field < headerEnd ? std::stoul(request.substr(field + 16)) : 0;
        }
        if (headerEnd != std::string::npos && request.size() >= headerEnd + 4 + want)
          break;
        const auto size = recv(client, buffer, sizeof(buffer), 0);
        if (size <= 0)
          break;
        request.append(buffer, size);
      }
      std::string body = R"({"features":[]})";
      if (request.rfind("POST /api/pair/request ", 0) == 0) {
        std::lock_guard<std::mutex> guard(lock);
        posts.push_back(request);
        body = pending;
      } else if (request.rfind("GET /api/pair/status ", 0) == 0) {
        body = pending;
      }
      const std::string reply = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
                                std::to_string(body.size()) + "\r\n\r\n" + body;
      send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
      close(client);
    }
  });
  const auto postCount = [&] {
    std::lock_guard<std::mutex> guard(lock);
    return posts.size();
  };
  const auto pollFor = [](foil::ui::Application &app, uint32_t ms) {
    const auto until = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < until) {
      app.poll();
      SDL_Delay(10);
    }
  };
  char temporary[] = "/tmp/cookingfoil-pair-note-XXXXXX";
  const char *root = mkdtemp(temporary);
  CHECK(root);
  if (root) {
    const std::string statePath = std::string(root) + "/state.json";
    {
      foil::ui::Application app(renderer, "", statePath);
      CHECK(app.open());
      const auto state = foil::loadState(statePath).value;
      app.setSession(std::make_unique<foil::net::Session>(server, state, statePath));
      app.refresh();
      auto deadline = SDL_GetTicks() + 3000;
      while (!app.pairingPending() && SDL_GetTicks() < deadline) {
        app.poll();
        SDL_Delay(10);
      }
      CHECK(app.pairingPending());
      app.render();
      CHECK(postCount() == 0); // no note, no request of its own
      // Y opens the keyboard for the note, empty the first time.
      app.action(foil::ui::Action::Queue);
      auto request = app.takeTextRequest();
      CHECK(request.has_value());
      if (request) {
        CHECK(request->initial.empty());
        CHECK(request->maxLength == 100);
        CHECK(!request->password);
      }
      app.enterText("  철수 거실 기기\n");
      const auto saved = foil::loadState(statePath).value.pairMessages;
      CHECK(saved.count(server.url) && saved.at(server.url) == "철수 거실 기기");
      deadline = SDL_GetTicks() + 5000;
      while (postCount() == 0 && SDL_GetTicks() < deadline) {
        app.poll();
        SDL_Delay(10);
      }
      CHECK(postCount() == 1);
      {
        std::lock_guard<std::mutex> guard(lock);
        if (!posts.empty()) {
          CHECK(posts[0].find("X-Device-Key: " + state.deviceKey) != std::string::npos);
          CHECK(posts[0].find("Content-Type: application/json") != std::string::npos);
          CHECK(posts[0].find("\r\n\r\n{\"deviceKey\":\"" + state.deviceKey + "\",\"message\":\"철수 거실 기기\"}") !=
                std::string::npos);
        }
      }
      // Once that refresh is done, the keyboard opens on the saved note.
      pollFor(app, 500);
      CHECK(app.pairingPending());
      app.render();
      app.action(foil::ui::Action::Queue);
      request = app.takeTextRequest();
      CHECK(request && request->initial == "철수 거실 기기");
      // The same text again changes nothing and sends nothing.
      app.enterText("철수 거실 기기");
      pollFor(app, 300);
      CHECK(postCount() == 1);
      // Text no longer goes to the note once it was handed over.
      app.enterText("다른 기기");
      CHECK(foil::loadState(statePath).value.pairMessages.at(server.url) == "철수 거실 기기");
      pollFor(app, 300);
    }
    std::filesystem::remove_all(root);
  }
  stop = true;
  mock.join();
  close(listener);
  return failures;
}
int runPairingUiTests(SDL_Renderer *renderer) {
  int failures = 0;
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(listener >= 0);
  if (listener < 0)
    return failures;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
  CHECK(listen(listener, 5) == 0);
  socklen_t length = sizeof(address);
  getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length);
  foil::Server server;
  server.url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
  const std::string pairUrl = server.url + "/admin/pair/" + std::string(32, 'a');
  std::vector<std::string> requests;
  std::thread mock([&] {
    // /api/shop/info is answered on the side, outside the sequence.
    for (int i = 0, accepted = 0; i < 3 && accepted < 10; ++accepted) {
      pollfd ready{listener, POLLIN, 0};
      if (::poll(&ready, 1, 10000) <= 0)
        break;
      const int client = accept(listener, nullptr, nullptr);
      if (client < 0)
        break;
      char buffer[4096];
      std::string request;
      while (request.find("\r\n\r\n") == std::string::npos) {
        const auto size = recv(client, buffer, sizeof(buffer), 0);
        if (size <= 0)
          break;
        request.append(buffer, size);
      }
      if (request.rfind("GET /api/shop/info ", 0) == 0) {
        const std::string info = R"({"features":[]})";
        const std::string reply = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(info.size()) + "\r\n\r\n" + info;
        send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
        close(client);
        continue;
      }
      requests.push_back(request);
      const std::string body = i == 0
          ? "{\"status\":\"pending\",\"pairUrl\":\"" + pairUrl + "\",\"pairCode\":\"1234ABCD\",\"expiresAt\":2000000000000}"
          : i == 1 ? R"({"status":"approved","accessKey":"issued-once"})"
                   : R"({"sections":[{"items":[{"url":"https://example.com/game.nsz","name":"Game","title_id":"0100000000000000"}]}]})";
      const std::string response = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
      send(client, response.data(), response.size(), MSG_NOSIGNAL);
      close(client);
      ++i;
    }
  });
  char temporary[] = "/tmp/cookingfoil-pair-ui-XXXXXX";
  const char *root = mkdtemp(temporary);
  CHECK(root);
  if (root) {
    const std::string statePath = std::string(root) + "/state.json";
    {
      foil::ui::Application app(renderer, "", statePath);
      CHECK(app.open());
      app.setSession(std::make_unique<foil::net::Session>(server, foil::loadState(statePath).value, statePath));
      app.refresh();
      auto deadline = SDL_GetTicks() + 2000;
      while (!app.pairingPending() && SDL_GetTicks() < deadline) {
        app.poll();
        SDL_Delay(10);
      }
      CHECK(app.pairingPending());
      CHECK(app.pairingInfo().url == pairUrl);
      CHECK(app.pairingInfo().code == "1234ABCD");
      CHECK(app.groupCount() == 0);
      app.render();
      deadline = SDL_GetTicks() + 8000;
      while (app.groupCount() == 0 && SDL_GetTicks() < deadline) {
        app.poll();
        SDL_Delay(10);
      }
      CHECK(app.groupCount() == 1);
      CHECK(!app.pairingPending());
      CHECK(app.pairingInfo().url.empty());
      CHECK(foil::loadState(statePath).value.accessKeys.at(server.url) == "issued-once");
    }
    std::filesystem::remove_all(root);
  }
  mock.join();
  close(listener);
  CHECK(requests.size() == 3);
  if (requests.size() == 3) {
    CHECK(requests[0].find("GET /api/pair/status HTTP") != std::string::npos);
    CHECK(requests[0].find("X-Device-Key:") != std::string::npos);
    CHECK(requests[2].find("X-Access-Key: issued-once") != std::string::npos);
  }
  failures += runPairMessageUiTests(renderer);
  return failures;
}
