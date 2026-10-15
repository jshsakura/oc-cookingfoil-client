#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
// The time from the internet (SNTP, RFC 4330), to tell a console clock that
// is off from a certificate that is bad, and to set the clock when the
// player agrees (platform/Clock).
namespace foil::net {
using NtpPacket = std::array<unsigned char, 48>;
// A client request carrying `cookie` as its transmit time; the server sends
// it back as the origin time, which ties the answer to this request.
NtpPacket ntpRequest(uint64_t cookie);
// The server's transmit time as Unix seconds, when `reply` answers the
// request made with `cookie` and the server is synchronized.
std::optional<int64_t> ntpTime(const unsigned char *reply, size_t size, uint64_t cookie);
// Asks each host in turn over UDP; the first good answer, or nothing when
// none came within `timeoutMs` each.
std::optional<int64_t> networkTime(const std::vector<std::string> &hosts, int port = 123, int timeoutMs = 2000);
// The servers asked: anycast, so the nearest answers.
const std::vector<std::string> &ntpServers();
} // namespace foil::net
