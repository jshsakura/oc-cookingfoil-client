#include "Test.hpp"
#include "net/Api.hpp"
int runPairingTests() {
  int failures = 0;
  foil::Server server;
  server.url = "https://shop.example.com";
  const std::string path = "/admin/pair/" + std::string(32, 'a');
  const auto response = [&](const std::string &url) {
    return "{\"status\":\"pending\",\"pairUrl\":\"" + url +
           "\",\"pairCode\":\"1234ABCD\",\"expiresAt\":2000000000000}";
  };
  const auto valid = foil::net::parsePairingInfo(response(server.url + path), server);
  CHECK(!valid.error);
  CHECK(valid.value.url == server.url + path);
  CHECK(valid.value.code == "1234ABCD");
  CHECK(valid.value.expiresAt == 2000000000000LL);
  // The login-first QR path that newer servers hand out.
  const std::string pairPath = "/pair/" + std::string(32, 'b');
  CHECK(foil::net::parsePairingInfo(response(server.url + pairPath), server).value.url == server.url + pairPath);
  CHECK(foil::net::parsePairingInfo(response(server.url + "/pair/" + std::string(31, 'b')), server).value.url.empty());
  CHECK(foil::net::parsePairingInfo(response("https://other.example.com" + pairPath), server).value.url.empty());
  CHECK(foil::net::parsePairingInfo(response("https://other.example.com" + path), server).value.url.empty());
  CHECK(foil::net::parsePairingInfo(response("http://shop.example.com" + path), server).value.url.empty());
  CHECK(foil::net::parsePairingInfo(response(server.url + path + "?accessKey=secret"), server).value.url.empty());
  CHECK(foil::net::parsePairingInfo(response(server.url + "/shop.tfl"), server).value.url.empty());
  CHECK(foil::net::parsePairingInfo(response("javascript:alert(1)"), server).value.url.empty());
  CHECK(foil::net::parsePairingInfo("{\"status\":\"pending\"}", server).value.url.empty());
  CHECK(foil::net::parsePairingInfo("bad-json", server).error);
  CHECK(foil::net::parsePairingInfo(response("https://user:secret@shop.example.com" + path), server).value.url.empty());
  // The note sent with an approval request: no message, no body at all.
  const std::string key(64, 'f');
  CHECK(foil::net::pairRequestBody(key, "").empty());
  CHECK(foil::net::pairRequestBody(key, " \n\t \r\n").empty());
  CHECK(foil::net::pairRequestBody(key, "철수 거실 기기") ==
        "{\"deviceKey\":\"" + key + "\",\"message\":\"철수 거실 기기\"}");
  // Trimmed, line breaks and tabs read as spaces, a run of spaces as one.
  CHECK(foil::net::cleanPairMessage("  철수\n거실 기기\r\n ") == "철수 거실 기기");
  CHECK(foil::net::cleanPairMessage("철수\t\t거실   기기") == "철수 거실 기기");
  // Quotes and slashes kept as JSON.
  CHECK(foil::net::pairRequestBody(key, " a\"b/c\n") ==
        "{\"deviceKey\":\"" + key + "\",\"message\":\"a\\\"b/c\"}");
  // At most 100 characters, cut between characters rather than bytes.
  std::string korean;
  for (int i = 0; i < 120; ++i) korean += "가";
  const auto cut = foil::net::cleanPairMessage(korean);
  CHECK(cut.size() == 100 * 3);
  CHECK(cut == korean.substr(0, 300));
  CHECK(foil::net::cleanPairMessage(std::string(99, 'a') + "가나").size() == 99 + 3);
  CHECK(foil::net::cleanPairMessage(std::string(150, 'a')).size() == 100);
  // A cut that lands on a space does not end in one.
  CHECK(foil::net::cleanPairMessage(std::string(99, 'a') + " b") == std::string(99, 'a'));
  // The server's features, for the menu; anything odd reads as none.
  const auto features = foil::net::parseFeatures("{\"features\":[\"retro\",\"homebrew\",3]}");
  CHECK(features.count("retro") && features.count("homebrew") && features.size() == 2);
  CHECK(foil::net::parseFeatures("{\"features\":\"retro\"}").empty());
  CHECK(foil::net::parseFeatures("bad-json").empty());
  return failures;
}
