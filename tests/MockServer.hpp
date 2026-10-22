#pragma once
// A server on the loopback for tests: each request is answered by `answer`,
// one connection at a time, and kept for take().
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace {
struct Reply {
  int status = 200;
  std::string headers, body;
};
struct Mock {
  int listener = -1;
  int port = 0;
  std::atomic<bool> stop{false};
  std::mutex lock;
  std::vector<std::string> requests;
  std::function<Reply(const std::string &)> answer;
  std::thread thread;
  Mock() {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, reinterpret_cast<sockaddr *>(&at), sizeof(at));
    listen(listener, 8);
    socklen_t size = sizeof(at);
    getsockname(listener, reinterpret_cast<sockaddr *>(&at), &size);
    port = ntohs(at.sin_port);
    thread = std::thread([this] { serve(); });
  }
  ~Mock() {
    stop = true;
    thread.join();
    close(listener);
  }
  void serve() {
    while (!stop) {
      pollfd ready{listener, POLLIN, 0};
      if (poll(&ready, 1, 50) <= 0) continue;
      const int client = accept(listener, nullptr, nullptr);
      if (client < 0) continue;
      std::string request;
      char buffer[4096];
      while (request.find("\r\n\r\n") == std::string::npos) {
        const auto got = recv(client, buffer, sizeof buffer, 0);
        if (got <= 0) break;
        request.append(buffer, size_t(got));
      }
      const auto end = request.find("\r\n\r\n");
      const auto length = request.find("Content-Length: ");
      const size_t want = length != std::string::npos && length < end ? std::stoul(request.substr(length + 16)) : 0;
      while (end != std::string::npos && request.size() < end + 4 + want) {
        const auto got = recv(client, buffer, sizeof buffer, 0);
        if (got <= 0) break;
        request.append(buffer, size_t(got));
      }
      Reply reply;
      {
        std::lock_guard<std::mutex> held(lock);
        requests.push_back(request);
        reply = answer(request);
      }
      const auto text = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\nConnection: close\r\n" + reply.headers +
                        "Content-Length: " + std::to_string(reply.body.size()) + "\r\n\r\n" + reply.body;
      send(client, text.data(), text.size(), MSG_NOSIGNAL);
      close(client);
    }
  }
  std::vector<std::string> take() {
    std::lock_guard<std::mutex> held(lock);
    return std::exchange(requests, {});
  }
};
} // namespace
