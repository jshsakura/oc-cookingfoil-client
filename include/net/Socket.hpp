#pragma once
// The TCP receive window. One connection carries at most its window per
// round trip, so it caps a download over a distant edge. The system's stack
// is BSD's: a socket that sets SO_RCVBUF itself stays at that size for good,
// while one that leaves it grows its window as the transfer needs, up to
// tcp_rx_buf_max_size (main.cpp). So downloads leave it.
#include <cstddef>
#include <cstdint>
namespace foil::net {
void setReceiveWindow(int bytes);
int receiveWindow();
// For CURLOPT_SOCKOPTFUNCTION: leaves the window to grow on its own and logs
// (the first time only) the size it starts from and the most it may reach.
int tuneSocket(void *user, int socket, int purpose);
// Every byte a download's connections take in, counted as curl hands it
// over: the download screen's speed reads this, so it moves while pieces
// that must arrive in order are still filling.
void countReceived(size_t bytes);
// The console says it has no internet right now (its own link check). A
// download waits that out instead of giving up; false when it cannot tell.
bool linkDown();
uint64_t receivedBytes();
// Puts a curl handle on the app's one pool of open connections, TLS
// sessions and DNS answers, so a request reuses a connection already warm.
void shareConnections(void *curl);
} // namespace foil::net
