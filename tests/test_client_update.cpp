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
      // An NRO as far as its header goes: the magic at 0x10, its size at 0x18.
      const auto nro = [](const std::string &tail) {
        std::string file(0x80, '\0');
        file.replace(0x10, 4, "NRO0");
        file[0x18] = char(0x80);
        return file + tail;
      };
      foil::atomicWrite(target, nro("old build"), ignored);
      foil::atomicWrite(target + ".new", nro("new build"), ignored);
      // At the next start, before anything opens the app's file.
      CHECK(foil::platform::finishUpdate(target, reason) && reason.find("swapped") != std::string::npos);
      CHECK(foil::readFile(target) == nro("new build") && !std::filesystem::exists(target + ".new"));
      foil::platform::confirmUpdate(target); // it came up: the old build goes
      CHECK(!std::filesystem::exists(target + ".old") && !std::filesystem::exists(target + ".trial"));
      // Nothing downloaded: the app's file stays as it was, and nothing is said.
      reason.clear();
      CHECK(!foil::platform::finishUpdate(target, reason) && reason.empty());
      CHECK(foil::readFile(target) == nro("new build"));
      // A swap that stopped half way left the app only as ".old": it comes
      // back before anything else, and the build waiting goes in after it.
      std::filesystem::rename(target, target + ".old");
      foil::atomicWrite(target + ".new", nro("newer build"), ignored);
      reason.clear();
      CHECK(foil::platform::finishUpdate(target, reason) && foil::readFile(target) == nro("newer build"));
      CHECK(foil::readFile(target + ".old") == nro("new build"));
      foil::platform::confirmUpdate(target);
      std::filesystem::rename(target, target + ".old");
      reason.clear();
      CHECK(!foil::platform::finishUpdate(target, reason) && foil::readFile(target) == nro("newer build"));
      CHECK(!std::filesystem::exists(target + ".old"));
      foil::atomicWrite(target, nro("new build"), ignored);
      // A new build that dies before it is up goes back to the one before it:
      // the swap keeps that one as ".old"; the first start marks the trial,
      // a second start that finds it marked undoes the swap.
      foil::atomicWrite(target + ".new", nro("broken build"), ignored);
      CHECK(foil::platform::finishUpdate(target, reason) && foil::readFile(target + ".old") == nro("new build"));
      CHECK(!foil::platform::rollBackUpdate(target, reason) && foil::readFile(target) == nro("broken build"));
      reason.clear();
      CHECK(foil::platform::rollBackUpdate(target, reason) && reason.find("rolled back") != std::string::npos);
      CHECK(foil::readFile(target) == nro("new build") && !std::filesystem::exists(target + ".old") &&
            !std::filesystem::exists(target + ".trial") && std::filesystem::exists(target + ".rolledback"));
      std::filesystem::remove(target + ".rolledback");
      // One that comes up is kept; the one before it goes.
      foil::atomicWrite(target + ".new", nro("good build"), ignored);
      CHECK(foil::platform::finishUpdate(target, reason) && !foil::platform::rollBackUpdate(target, reason));
      foil::platform::confirmUpdate(target);
      CHECK(!foil::platform::rollBackUpdate(target, reason) && foil::readFile(target) == nro("good build"));
      CHECK(!std::filesystem::exists(target + ".old") && !std::filesystem::exists(target + ".trial"));
      foil::atomicWrite(target, nro("new build"), ignored);
      // A download cut off (shorter than its header says, or not an NRO at
      // all, or an earlier version's half with its resume note) never takes
      // the app's place; it goes, and the app stays.
      for (const auto &half : {nro("x").substr(0, 0x40), std::string("<html>error</html>"), nro("whole")}) {
        foil::atomicWrite(target + ".new", half, ignored);
        if (half == nro("whole")) foil::atomicWrite(target + ".new.resume", "url=x\n", ignored);
        reason.clear();
        CHECK(!foil::platform::finishUpdate(target, reason) && reason.find("not whole") != std::string::npos);
        CHECK(foil::readFile(target) == nro("new build") && !std::filesystem::exists(target + ".new") &&
              !std::filesystem::exists(target + ".new.resume"));
      }
      std::filesystem::remove_all(root);
    }
  }
  CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  return failures;
}
