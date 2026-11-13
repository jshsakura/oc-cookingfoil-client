#include "Test.hpp"
#include "core/Firmware.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
namespace fs = std::filesystem;
int runFirmwareTests() {
  int failures = 0;
  using namespace foil::firmware;
  // Version names: three numbers, nothing else, so a folder name never
  // carries a path or the source's odd tags.
  for (const char *good : {"23.0.1", "1.0.0", "9.2.0", "10.0.0"}) CHECK(isVersion(good));
  for (const char *bad : {"1.0.0pr", "1.0", "1.0.0.0", "", "../1.0.0", "1.0.0/", "1..0", "1234.0.0", "v1.0.0", " 1.0.0"})
    CHECK(!isVersion(bad));
  CHECK(compare("9.0.0", "10.0.0") < 0 && compare("10.0.0", "9.2.0") > 0 && compare("23.0.1", "23.0.1") == 0);
  CHECK(compare("22.5.0", "22.10.0") < 0);
  {
    // The server's list: odd and repeated names left out, newest first by
    // number, its addresses made full, nothing to fetch never available.
    const std::string body = R"({"source":{"name":"x","reachable":false,"checked_at":"2026-11-06T00:00:00Z"},"versions":[)"
        R"({"version":"9.2.0","released":"2019-12-10","available":true,"archive":{"url":"/api/firmware/9.2.0/download","size":10,"files":2,"unpacked":9}},)"
        R"({"version":"10.0.0","released":"2020-04-14","available":true,"archive":{"url":"/api/firmware/10.0.0/download","size":20,"files":3}},)"
        R"({"version":"1.0.0pr","available":true,"archive":{"url":"/api/firmware/x/download","size":5}},)"
        R"({"version":"9.2.0","available":true,"archive":{"url":"/twice","size":1}},)"
        R"({"version":"8.0.0","available":true},)"
        R"({"version":"7.0.0","available":false,"reason":"upstream_gone","archive":{"url":"/api/firmware/7.0.0/download","size":7}}]})";
    List list;
    CHECK(parse(body, "https://server.example", list));
    CHECK(!list.reachable && list.checkedAt == "2026-11-06T00:00:00Z");
    CHECK(list.versions.size() == 4);
    if (list.versions.size() == 4) {
      CHECK(list.versions[0].version == "10.0.0" && list.versions[1].version == "9.2.0" &&
            list.versions[2].version == "8.0.0" && list.versions[3].version == "7.0.0");
      CHECK(list.versions[1].archive.url == "https://server.example/api/firmware/9.2.0/download" &&
            list.versions[1].archive.files == 2 && list.versions[1].archive.unpacked == 9 && list.versions[1].available);
      CHECK(!list.versions[2].available);
      CHECK(!list.versions[3].available && list.versions[3].reason == "upstream_gone");
      // Through the download queue and back, the same.
      Version back;
      CHECK(parseOne(serialize(list.versions[1]), back));
      CHECK(back.version == "9.2.0" && back.archive.url == list.versions[1].archive.url && back.archive.size == 10 &&
            back.archive.files == 2 && back.available);
    }
    // Not a list (an error page answered 200): false, the old list kept.
    List kept = list;
    CHECK(!parse("<html>busy</html>", "https://server.example", kept));
    CHECK(kept.versions.size() == 4);
    List none;
    CHECK(parse(R"({"versions":[]})", "https://server.example", none) && none.versions.empty() && none.reachable);
  }
  // Only bare .nca names are firmware files.
  for (const char *good : {"00e57b67b4dbd750a2df0574cddc123d.nca", "007ab9b9016a588732d828ab1874d1b8.cnmt.nca"})
    CHECK(isFirmwareEntry(good));
  for (const char *bad : {".nca", "a.nca/", "x/a.nca", "../a.nca", "/a.nca", "C:a.nca", "a\\b.nca", "a.txt", "a.nca.zip", ""})
    CHECK(!isFirmwareEntry(bad));
  {
    // Every file of the zip goes to the staging folder; a folder record is
    // passed over; a count other than the server's refuses the zip.
    Version v;
    v.version = "23.0.1";
    v.archive.files = 2;
    std::vector<foil::ZipEntry> inZip(3);
    inZip[0].name = "a.nca";
    inZip[1].name = "folder/";
    inZip[2].name = "b.cnmt.nca";
    std::vector<std::pair<foil::ZipEntry, std::string>> wanted;
    std::string why;
    CHECK(pickEntries(v, inZip, wanted, why) && wanted.size() == 2);
    CHECK(wanted.size() == 2 && wanted[0].second == "switch/cookingfoil/staging/firmware-23.0.1/a.nca" &&
          wanted[1].second == "switch/cookingfoil/staging/firmware-23.0.1/b.cnmt.nca");
    v.archive.files = 3;
    CHECK(!pickEntries(v, inZip, wanted, why) && wanted.empty() && !why.empty());
    v.archive.files = 0; // not counted by the server: any count of good files
    inZip[1].name = "notes.txt";
    CHECK(!pickEntries(v, inZip, wanted, why) && wanted.empty());
    inZip[1].encrypted = true;
    inZip[1].name = "c.nca";
    CHECK(!pickEntries(v, inZip, wanted, why));
    CHECK(folder("23.0.1") == "firmware/23.0.1");
  }
  {
    // On the card: version folders holding .nca files, nothing else.
    const auto sd = fs::temp_directory_path() / ("cf-fw-card-" + std::to_string(getpid()));
    fs::remove_all(sd);
    for (const char *dir : {"firmware/23.0.1", "firmware/22.0.0", "firmware/1.0.0pr", "firmware/notes"})
      fs::create_directories(sd / dir);
    std::ofstream(sd / "firmware/23.0.1/a.nca") << "x";
    std::ofstream(sd / "firmware/1.0.0pr/a.nca") << "x";
    std::ofstream(sd / "firmware/22.0.0/readme.txt") << "x";
    std::ofstream(sd / "firmware/9.0.0") << "a file, not a folder";
    const auto found = onCard(sd.string());
    CHECK(found == std::set<std::string>{"23.0.1"});
    CHECK(onCard((sd / "nowhere").string()).empty());
    fs::remove_all(sd);
  }
  return failures;
}
