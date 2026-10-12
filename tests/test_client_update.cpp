#include "Test.hpp"
#include "core/ClientUpdate.hpp"
using namespace foil::update;
int runClientUpdateTests() {
  int failures = 0;
  // Numbers compare as numbers, missing parts read as 0, suffixes are ignored.
  CHECK(compareVersions("0.1.10", "0.1.9") > 0);
  CHECK(compareVersions("0.1.0", "0.1") == 0);
  CHECK(compareVersions("0.2.0", "0.10.0") < 0);
  CHECK(compareVersions("0.1.1", "0.0.0-dev") > 0);
  CHECK(compareVersions("1.0.0-dev", "1.0.0") == 0);
  const std::string sha(64, 'a');
  const auto r = parseRelease(R"({"version":"0.1.3","size":1000,"sha256":")" + sha +
                                  R"(","url":"/api/client/cookingfoil.nro"})",
                              "https://shop.example");
  CHECK(r.has_value());
  if (r) CHECK(r->url == "https://shop.example/api/client/cookingfoil.nro" && r->size == 1000 && r->version == "0.1.3");
  // Anything off is no update at all.
  CHECK(!parseRelease(R"({"version":"0.1.3","size":1000,"sha256":"abc","url":"/x"})", "https://s"));
  CHECK(!parseRelease(R"({"version":"0.1.3","size":0,"sha256":")" + sha + R"(","url":"/x"})", "https://s"));
  CHECK(!parseRelease(R"({"version":"0.1.3","size":10,"sha256":")" + sha + R"(","url":"ftp://x"})", "https://s"));
  CHECK(!parseRelease("nope", "https://s"));
  // The app's own HOME Menu shortcut, when the server builds one.
  const auto withShortcut = parseRelease(R"({"version":"0.1.17","size":1000,"sha256":")" + sha +
                                         R"(","url":"/api/client/cookingfoil.nro","forwarder_url":"/api/client/forwarder",)"
                                         R"("forwarder_title_id":"0512BCE4CF34D000","forwarder_size":271064})",
                                         "https://shop.example");
  CHECK(withShortcut && withShortcut->forwarderUrl == "https://shop.example/api/client/forwarder" &&
        withShortcut->forwarderTitleId == "0512BCE4CF34D000" && withShortcut->forwarderSize == 271064);
  CHECK(r && r->forwarderUrl.empty());
  CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  return failures;
}
