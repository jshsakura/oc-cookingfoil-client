#pragma once
#include <atomic>
// The TCP receive window. One connection carries at most its window per
// round trip, so it caps a download over a distant edge. The system's stack
// is BSD's: a socket that sets SO_RCVBUF itself stays at that size for good,
// while one that leaves it grows its window as the transfer needs, up to
// tcp_rx_buf_max_size (main.cpp). So downloads leave it.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
namespace foil::net {
void setReceiveWindow(int bytes);
int receiveWindow();
// Every byte a download's connections take in, counted as curl hands it
// over: the download screen's speed reads this, so it moves while pieces
// that must arrive in order are still filling.
void countReceived(size_t bytes);
// The way to `url`'s server is down for now: the console says it has no
// internet (its own link check), or that server said it is restarting. A
// download waits that out instead of giving up; false when it cannot tell.
bool linkDown(const std::string &url);
// Everything below is kept per server, by the server's own origin: a
// request URL finds its server by that origin or by the direct address
// the server named, so one server's replies never touch another's token,
// address or restart. serverOf gives that origin.
std::string serverOf(const std::string &url);
// A server restart, from its X-CookingFoil-Restarting header ("20; id=…"),
// which every reply carries while it restarts. Each header line of every
// reply passes here (heardHeader) with the URL it answered; a successful
// reply without it ends the restart. restartSerial() counts the restarts
// heard, one per id, so the screen says it once each.
void heardHeader(const std::string &url, const char *line, size_t bytes);
bool serverRestarting(const std::string &url);
bool serverRestarting(); // any server
// The download token the server hands out with its lists
// (X-CookingFoil-Download-Token): downloads from its download host carry it
// as a bearer token. downloadTokenRejected() says a download was refused
// for it (401, X-CookingFoil-Token: expired or revoked) since the last one
// came; a new one clears it.
std::string downloadToken(const std::string &url);
bool downloadTokenRejected(const std::string &url);
// The direct address the token is for (X-CookingFoil-Download-Origin,
// "https://host:port"); empty while the server has not said. Once it has,
// the token goes there and nowhere else.
std::string downloadTokenOrigin(const std::string &url);
void forgetDownloadToken(const std::string &url);
// `serverUrl`'s token went to `url`, a host off it that is no server (the
// server named no direct address): that host's replies are the server's.
void lendToken(const std::string &serverUrl, const std::string &url);
void forgetLinks(); // tests: every server's
// How a fresh token is asked for (the app sets it with its server; it
// returns the reply's status), and asking after a 401 for a request sent
// with `sent`: true when a newer token is held, and the request goes again
// with it. Parts refused together find the one the first of them got; the
// server is asked at most once in RenewGapSeconds, so a refusal is never
// hammered (a server login asked again and again locks the account).
constexpr int RenewGapSeconds = 30;
void setTokenRenewer(const std::string &serverUrl, std::function<long()> renew);
bool renewDownloadToken(const std::string &url, const std::string &sent);
// The server refused the login itself when asked for a token: the console
// needs its account checked, not another try. Cleared by the next token.
bool downloadSignInRefused(const std::string &url);
// How many connections the server takes from a console now
// (X-CookingFoil-Max-Connections, or its info's download.maxConnections),
// 0 when it has not said; and how many consoles download from it now.
void recommendConnections(const std::string &url, int most);
int recommendedConnections(const std::string &url);
void heardActiveDownloads(const std::string &url, int count);
int activeDownloads(const std::string &url);
// The bearer token in a request's headers, or empty.
std::string bearerOf(const std::vector<std::string> &headers);
// A request's headers with the bearer token swapped for the current one.
void freshenBearer(const std::string &url, std::vector<std::string> &headers);
uint64_t restartSerial();
// For a curl handle with no header callback of its own (the engine's): its
// replies are heard as from the URL it asked.
void watchRestart(void *curl);
uint64_t receivedBytes();
// Puts a curl handle on its thread's pool of open connections, TLS sessions
// and DNS answers, so a request reuses a connection already warm.
void shareConnections(void *curl);
// Before curl_global_cleanup: the caches threads still hold are left to the
// process's end instead of closed after curl is gone.
void endConnections();
// The session token (a server offering "session-token"): asked for with the
// server's sign-in (Basic, access key), then sent to the server's own
// address in its place, so the password goes out once in 12 hours instead
// of with every request. Held in memory only, never saved.
struct SessionGrant {
  long status = 0;
  std::string token;
  int64_t secondsLeft = 0; // until the server's expiresAt, by the server's clock
};
// The server offers it: `ask` gets one. `credentials` tells the sign-in it
// was asked with; while it is the same, a refusal stands for the run (403:
// the server keeps Basic), and a token held stays.
void offerSession(const std::string &origin, const std::string &credentials, std::function<SessionGrant()> ask);
void withdrawSession(const std::string &origin);
// The token for a request to `origin`, asked for first when there is none,
// when it ends within RenewBeforeSeconds, or after the server turned one
// down; empty when the server gives none (the request signs in as before).
constexpr int64_t RenewBeforeSeconds = 600;
std::string sessionToken(const std::string &origin);
// The stop flag of the work this thread does, while the scope lasts: asking
// for a session token (sessionToken, renewSession), and waiting for another
// request's ask, end when it is set, as the request itself does.
class CancelScope {
public:
  explicit CancelScope(const std::atomic<bool> *cancel);
  ~CancelScope();
  CancelScope(const CancelScope &) = delete;
  CancelScope &operator=(const CancelScope &) = delete;

private:
  const std::atomic<bool> *outer_;
};
const std::atomic<bool> *scopedCancel();
// After a 401 with X-CookingFoil-Session for a request sent with `sent`:
// a new token is asked for once, true when one newer than `sent` is held.
bool renewSession(const std::string &sent);
bool isSessionToken(const std::string &token);
// The X-CookingFoil-Session replies heard so far from `url`'s server
// (expired | revoked | invalid).
uint64_t sessionRefusals(const std::string &url);
// The server took this console back (its account turned off or removed, its
// password changed): a reply said X-CookingFoil-Session: revoked, or asking
// for a session token was refused (401). True once per take-back of the
// server `serverUrl`, for the screen to refresh it and ask for approval again.
bool takeConsoleRevoked(const std::string &serverUrl);
// Tests: every session forgotten.
void forgetSessions();
} // namespace foil::net
