#include "Test.hpp"
#include "net/Ntp.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <thread>
namespace {
// A server's answer to `request`: mode 4, stratum 2, the request's transmit
// time as origin, `seconds` (NTP era 0) as transmit time.
foil::net::NtpPacket answer(const unsigned char *request, uint32_t seconds) {
    foil::net::NtpPacket reply{};
    reply[0] = 0 << 6 | 4 << 3 | 4;
    reply[1] = 2;
    for (int i = 0; i < 8; ++i) reply[24 + i] = request[40 + i];
    for (int i = 0; i < 4; ++i) reply[40 + i] = static_cast<unsigned char>(seconds >> (24 - 8 * i));
    return reply;
}
}
// Internet time for the clock fix: only an answer to this request from a
// synchronized server counts.
int runNtpTests() {
    int failures = 0;
    using foil::net::ntpTime;
    constexpr uint64_t Cookie = 0x0123456789ABCDEFull;
    constexpr int64_t Unix = 1792000000; // 2026-10
    const auto request = foil::net::ntpRequest(Cookie);
    CHECK(request[0] == 0x23); // version 4, client
    CHECK(request[40] == 0x01 && request[47] == 0xEF);
    const auto good = answer(request.data(), uint32_t(Unix + 2208988800LL));
    CHECK(ntpTime(good.data(), good.size(), Cookie) == Unix);
    CHECK(!ntpTime(good.data(), good.size(), Cookie + 1)); // another request's answer
    CHECK(!ntpTime(good.data(), 47, Cookie));
    auto changed = good; changed[0] = 0 << 6 | 4 << 3 | 3; // a client, not a server
    CHECK(!ntpTime(changed.data(), changed.size(), Cookie));
    changed = good; changed[1] = 0; // kiss-of-death
    CHECK(!ntpTime(changed.data(), changed.size(), Cookie));
    changed = good; changed[0] = 3 << 6 | 4 << 3 | 4; // not synchronized
    CHECK(!ntpTime(changed.data(), changed.size(), Cookie));
    // After 2036 the seconds wrap into the next era.
    const auto wrapped = answer(request.data(), 16);
    CHECK(ntpTime(wrapped.data(), wrapped.size(), Cookie) == (int64_t(1) << 32) + 16 - 2208988800LL);
    // Over UDP, from a server on this machine.
    const int udp = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in at{}; at.sin_family = AF_INET; at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(udp >= 0 && bind(udp, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0);
    socklen_t size = sizeof(at); getsockname(udp, reinterpret_cast<sockaddr*>(&at), &size);
    std::thread server([&] {
        pollfd ready{udp, POLLIN, 0}; if (poll(&ready, 1, 5000) <= 0) return;
        unsigned char got[64]; sockaddr_in from{}; socklen_t length = sizeof(from);
        if (recvfrom(udp, got, sizeof got, 0, reinterpret_cast<sockaddr*>(&from), &length) < 48) return;
        const auto reply = answer(got, uint32_t(Unix + 2208988800LL));
        sendto(udp, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), length);
    });
    CHECK(foil::net::networkTime({"127.0.0.1"}, ntohs(at.sin_port), 3000) == Unix);
    server.join();
    // Nobody answering: nothing, after the wait.
    CHECK(!foil::net::networkTime({"127.0.0.1"}, ntohs(at.sin_port), 200));
    close(udp);
    return failures;
}
