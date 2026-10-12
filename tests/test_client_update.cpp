#include "Test.hpp"
#include "core/ClientUpdate.hpp"
#include "core/Json.hpp"
#include "platform/Storage.hpp"
#include <cstdlib>
#include <filesystem>
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
  // The downloaded build takes the app's place; the old one goes.
  {
    char temporary[] = "/tmp/cookingfoil-swap-XXXXXX";
    if (const char *root = mkdtemp(temporary)) {
      const std::string target = std::string(root) + "/cookingfoil.nro";
      std::string ignored, reason;
      foil::atomicWrite(target, "old build", ignored);
      foil::atomicWrite(target + ".new", "new build", ignored);
      CHECK(foil::platform::replaceSelf(target + ".new", target, reason) && reason.empty());
      CHECK(foil::readFile(target) == "new build" && !std::filesystem::exists(target + ".new") &&
            !std::filesystem::exists(target + ".old"));
      // Nothing downloaded: the app's file stays as it was.
      CHECK(!foil::platform::replaceSelf(target + ".new", target, reason) && !reason.empty());
      CHECK(foil::readFile(target) == "new build");
      std::filesystem::remove_all(root);
    }
  }
  CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  return failures;
}
