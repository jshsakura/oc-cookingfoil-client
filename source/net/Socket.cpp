#include "net/Socket.hpp"
#include "core/Log.hpp"
#include <atomic>
#include <string>
#include <sys/socket.h>
namespace foil::net {
namespace {
std::atomic<int> window{0};
std::atomic<bool> logged{false};
std::atomic<uint64_t> received{0};
} // namespace
void setReceiveWindow(int bytes) { window = bytes; }
void countReceived(size_t bytes) { received.fetch_add(bytes, std::memory_order_relaxed); }
uint64_t receivedBytes() { return received.load(std::memory_order_relaxed); }
int receiveWindow() { return window; }
int tuneSocket(void *, int socket, int) {
  if (!logged.exchange(true)) {
    int start = 0;
    socklen_t size = sizeof(start);
    getsockopt(socket, SOL_SOCKET, SO_RCVBUF, &start, &size);
    logLine("socket receive window starts at " + std::to_string(start) + ", grows to " + std::to_string(window.load()));
  }
  return 0; // CURL_SOCKOPT_OK
}
} // namespace foil::net
