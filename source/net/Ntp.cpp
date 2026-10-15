#include "net/Ntp.hpp"
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
namespace foil::net {
namespace {
// Seconds from 1900 (NTP's start) to 1970.
constexpr int64_t NtpToUnix = 2208988800LL;
uint64_t readBig(const unsigned char *at, int bytes) {
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) value = value << 8 | at[i];
  return value;
}
} // namespace
NtpPacket ntpRequest(uint64_t cookie) {
  NtpPacket packet{};
  packet[0] = 0 << 6 | 4 << 3 | 3; // no leap warning, version 4, client
  for (int i = 0; i < 8; ++i) packet[40 + i] = static_cast<unsigned char>(cookie >> (56 - 8 * i));
  return packet;
}
std::optional<int64_t> ntpTime(const unsigned char *reply, size_t size, uint64_t cookie) {
  if (size < 48) return std::nullopt;
  const int leap = reply[0] >> 6, mode = reply[0] & 7, stratum = reply[1];
  // A server's answer (mode 4), synchronized (leap 3 is not), not a
  // kiss-of-death (stratum 0), to this request.
  if (mode != 4 || leap == 3 || stratum == 0 || stratum > 15 || readBig(reply + 24, 8) != cookie)
    return std::nullopt;
  const auto seconds = static_cast<uint32_t>(readBig(reply + 40, 4));
  if (!seconds) return std::nullopt;
  // From 2036 the 32-bit seconds wrap: a small value is the next era.
  int64_t unix = int64_t(seconds) - NtpToUnix;
  if (seconds < 0x80000000u) unix += int64_t(1) << 32;
  return unix;
}
std::optional<int64_t> networkTime(const std::vector<std::string> &hosts, int port, int timeoutMs) {
  for (const auto &host : hosts) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) || !found) continue;
    const int fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    const bool linked = fd >= 0 && connect(fd, found->ai_addr, found->ai_addrlen) == 0;
    freeaddrinfo(found);
    if (!linked) {
      if (fd >= 0) close(fd);
      continue;
    }
    const auto cookie = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) | 1;
    const auto request = ntpRequest(cookie);
    std::optional<int64_t> time;
    if (send(fd, request.data(), request.size(), 0) == ssize_t(request.size())) {
      pollfd ready{fd, POLLIN, 0};
      unsigned char reply[128];
      if (poll(&ready, 1, timeoutMs) > 0) {
        const auto got = recv(fd, reply, sizeof reply, 0);
        if (got > 0) time = ntpTime(reply, size_t(got), cookie);
      }
    }
    close(fd);
    if (time) return time;
  }
  return std::nullopt;
}
const std::vector<std::string> &ntpServers() {
  static const std::vector<std::string> servers{"time.cloudflare.com", "time.google.com", "pool.ntp.org"};
  return servers;
}
} // namespace foil::net
