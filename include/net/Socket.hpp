#pragma once
// The TCP receive window. One connection carries at most its window per
// round trip, so it caps a download over a distant edge. The system's stack
// is BSD's: a socket that sets SO_RCVBUF itself stays at that size for good,
// while one that leaves it grows its window as the transfer needs, up to
// tcp_rx_buf_max_size (main.cpp). So downloads leave it.
#include <cstddef>
#include <cstdint>
#include <string>
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
// The way to the server is down for now: the console says it has no
// internet (its own link check), or the server said it is restarting. A
// download waits that out instead of giving up; false when it cannot tell.
bool linkDown();
// A server restart, from its X-CookingFoil-Restarting header ("20; id=…"),
// which every reply carries while it restarts. Each header line of every
// reply passes here (heardHeader); a successful reply without it ends the
// restart. restartSerial() counts the restarts heard, one per id, so the
// screen says it once each.
void heardHeader(const char *line, size_t bytes);
bool serverRestarting();
// The download token the server hands out with its lists
// (X-CookingFoil-Download-Token): downloads from its download host carry it
// as a bearer token. downloadTokenRejected() says a download was refused
// for it (401, X-CookingFoil-Token: expired or revoked) since the last one
// came; a new one clears it.
std::string downloadToken();
bool downloadTokenRejected();
uint64_t restartSerial();
// For a curl handle with no header callback of its own (the engine's).
void watchRestart(void *curl);
uint64_t receivedBytes();
// Puts a curl handle on the app's one pool of open connections, TLS
// sessions and DNS answers, so a request reuses a connection already warm.
void shareConnections(void *curl);
} // namespace foil::net
