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
  const int wanted = window;
  if (wanted > 0) {
    setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &wanted, sizeof(wanted));
    if (!logged.exchange(true)) {
      int granted = 0;
      socklen_t size = sizeof(granted);
      getsockopt(socket, SOL_SOCKET, SO_RCVBUF, &granted, &size);
      logLine("socket receive window " + std::to_string(wanted) + " asked, " + std::to_string(granted) + " granted");
    }
  }
  return 0; // CURL_SOCKOPT_OK
}
} // namespace foil::net
