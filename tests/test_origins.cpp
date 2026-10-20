// Two servers at once: what one says about its downloads (its token, its
// direct address, a refusal, a restart, its connections) stays its own, and
// its token never goes to the other.
#include "Test.hpp"
#include "core/Config.hpp"
#include "core/State.hpp"
#include "net/Api.hpp"
#include "net/Direct.hpp"
#include "net/Segments.hpp"
#include "net/Socket.hpp"
#include <algorithm>
namespace {
void hear(const std::string &from, const std::string &line) { foil::net::heardHeader(from, line.data(), line.size()); }
void reply(const std::string &from, const std::vector<std::string> &lines) {
  hear(from, "HTTP/1.1 200 OK\r\n");
  for (const auto &line : lines) hear(from, line + "\r\n");
  hear(from, "\r\n");
}
bool hasBearer(const std::vector<std::string> &headers, const std::string &token) {
  return std::find(headers.begin(), headers.end(), "Authorization: Bearer " + token) != headers.end();
}
bool anyBearer(const std::vector<std::string> &headers) { return !foil::net::bearerOf(headers).empty(); }
} // namespace
int runOriginTests() {
  int failures = 0;
  using namespace foil::net;
  forgetLinks();
  forgetDirect();
  const std::string a = "https://a.example", b = "https://b.example:8443";
  foil::setKnownServers(std::vector<std::string>{a, b});
  foil::Server serverA, serverB;
  serverA.url = a;
  serverB.url = b;
  foil::State state;
  {
    // Each server's token and direct address are its own.
    reply(a + "/api/shop/sections", {"X-CookingFoil-Download-Token: token-a", "X-CookingFoil-Download-Origin: https://dl-a.example"});
    reply(b + "/api/shop/sections", {"X-CookingFoil-Download-Token: token-b"});
    CHECK(downloadToken(a) == "token-a" && downloadToken(b) == "token-b");
    CHECK(downloadTokenOrigin(a) == "https://dl-a.example" && downloadTokenOrigin(b).empty());
    // A reply from the direct address counts as its server's.
    CHECK(serverOf("https://dl-a.example/api/get/x") == a);
    CHECK(downloadToken("https://dl-a.example/api/get/x") == "token-a");
    reply("https://dl-a.example/api/get/x", {"X-CookingFoil-Download-Token: token-a2"});
    CHECK(downloadToken(a) == "token-a2" && downloadToken(b) == "token-b");
  }
  {
    // A server naming another server's own address as its direct one is
    // not taken: the other's replies and token stay the other's.
    reply(b + "/api/shop/sections", {"X-CookingFoil-Download-Origin: https://a.example"});
    CHECK(downloadTokenOrigin(b).empty() && serverOf(a + "/api/x") == a);
  }
  {
    // Routed to the direct address only for the server that named one.
    CHECK(routeDownload(a + "/api/get/x.nsz?v=1") == "https://dl-a.example/api/get/x.nsz?v=1");
    CHECK(routeDownload(b + "/api/get/x.nsz") == b + "/api/get/x.nsz");
    CHECK(headersFor("https://dl-a.example/api/get/x", {"X-Access-Key: k"}) ==
          std::vector<std::string>{"Authorization: Bearer token-a2"});
  }
  {
    // A token goes to its own server's direct address and never to the
    // other server, whether by its own address or its direct one; a server
    // that named no direct address still keeps its token off the others.
    CHECK(hasBearer(downloadHeaders(serverA, state, "https://dl-a.example/api/get/x"), "token-a2"));
    CHECK(!anyBearer(downloadHeaders(serverA, state, b + "/api/get/x")));
    CHECK(hasBearer(downloadHeaders(serverB, state, "https://mirror.example/x.nsp"), "token-b"));
    CHECK(!anyBearer(downloadHeaders(serverB, state, a + "/api/get/x")));
    CHECK(!anyBearer(downloadHeaders(serverB, state, "https://dl-a.example/api/get/x")));
  }
  {
    // A refusal, a sign-in refused, and turning the direct address off
    // touch one server.
    reply(b + "/api/get/x", {"X-CookingFoil-Token: expired"});
    CHECK(downloadTokenRejected(b) && !downloadTokenRejected(a));
    forgetDownloadToken(a);
    CHECK(downloadToken(a).empty() && downloadTokenOrigin(a).empty() && downloadToken(b) == "token-b");
    int askedA = 0, askedB = 0;
    setTokenRenewer(a, [&] { ++askedA; return 401L; });
    setTokenRenewer(b, [&] {
      ++askedB;
      reply(b + "/api/shop/info", {"X-CookingFoil-Download-Token: token-b2"});
      return 200L;
    });
    CHECK(renewDownloadToken(b + "/api/get/x", "token-b") && askedB == 1 && askedA == 0);
    CHECK(downloadToken(b) == "token-b2" && !downloadTokenRejected(b));
    CHECK(!renewDownloadToken(a + "/api/get/x", "") && askedA == 1);
    CHECK(downloadSignInRefused(a) && !downloadSignInRefused(b));
    setTokenRenewer(a, {});
    setTokenRenewer(b, {});
  }
  {
    // One server restarting holds its own downloads, not the other's.
    hear(a + "/api/x", "HTTP/1.1 503 Service Unavailable\r\n");
    hear(a + "/api/x", "X-CookingFoil-Restarting: 20; id=ra\r\n");
    hear(a + "/api/x", "\r\n");
    CHECK(serverRestarting(a) && !serverRestarting(b) && serverRestarting());
    CHECK(linkDown(a + "/api/get/x") && !linkDown(b + "/api/get/x"));
    reply(a + "/api/x", {});
    CHECK(!serverRestarting(a) && !serverRestarting());
  }
  {
    // Connections and the take-back are each server's.
    reply(a + "/api/x", {"X-CookingFoil-Max-Connections: 2", "X-CookingFoil-Active-Downloads: 3"});
    CHECK(connectionsFor(a + "/api/get/x", 0) == 2 && connectionsFor(b + "/api/get/x", 0) == 4);
    CHECK(activeDownloads(a) == 3 && activeDownloads(b) == 0);
    takeConsoleRevoked();
    hear(b + "/api/x", "HTTP/1.1 401 Unauthorized\r\n");
    hear(b + "/api/x", "X-CookingFoil-Session: revoked\r\n");
    hear(b + "/api/x", "\r\n");
    CHECK(takeConsoleRevoked() == std::set<std::string>{b} && takeConsoleRevoked().empty());
    CHECK(sessionRefusals(b) == 1 && sessionRefusals(a) == 0);
  }
  foil::setKnownServers(std::nullopt);
  forgetLinks();
  forgetDirect();
  return failures;
}
