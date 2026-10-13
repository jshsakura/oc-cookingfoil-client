#pragma once
// The TCP receive window every download asks for, set once at start from the
// socket driver's configuration. One connection carries at most its window
// per round trip, so this caps a single download over a distant edge.
#include <cstddef>
#include <cstdint>
namespace foil::net {
void setReceiveWindow(int bytes);
int receiveWindow();
// For CURLOPT_SOCKOPTFUNCTION: asks the socket for the window, and logs (the
// first time only) what the system actually granted, which is the real cap.
int tuneSocket(void *user, int socket, int purpose);
// Every byte a download's connections take in, counted as curl hands it
// over: the download screen's speed reads this, so it moves while pieces
// that must arrive in order are still filling.
void countReceived(size_t bytes);
uint64_t receivedBytes();
} // namespace foil::net
