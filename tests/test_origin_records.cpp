// A download's record names the server it comes from, survives a restart
// with it, reads as before when it names none, and follows the server when
// its address is edited.
#include "Test.hpp"
#include "core/DownloadHistory.hpp"
#include "core/State.hpp"
namespace h = foil::history;
int runOriginRecordTests() {
  int failures = 0;
  foil::Item file;
  file.url = "https://b.example/api/get/game.nsz";
  file.name = "game.nsz";
  file.size = 42;
  {
    foil::PendingInstall pending{"Game", "", "sd", {file}, "https://b.example"};
    const auto back = foil::parsePending(foil::serializePending(pending));
    CHECK(back && back->server == "https://b.example" && back->files.size() == 1);
    // A row from an earlier build names no server: the one in use.
    pending.server.clear();
    const auto json = foil::serializePending(pending);
    CHECK(json.find("\"server\"") == std::string::npos);
    const auto old = foil::parsePending(json);
    CHECK(old && old->server.empty());
  }
  std::vector<h::Entry> rows;
  {
    h::Entry a, b, job;
    a.install = {"A", "", "sd", {file}, "https://a.example"};
    b.install = {"B", "", "sd", {file}, "https://b.example"};
    job.install = {"Retro", "", "sd", {file}, "https://a.example"};
    job.kind = "retro";
    job.payload = "{}";
    h::add(rows, a);
    h::add(rows, b);
    h::add(rows, job);
    const auto back = h::parse(h::serialize(rows));
    CHECK(back.size() == 3);
    int fromA = 0;
    for (const auto &row : back) fromA += row.install.server == "https://a.example";
    CHECK(fromA == 2);
  }
  {
    // Server A moved to a new address: its rows follow, B's stay.
    CHECK(h::moveServer(rows, "https://a.example", "https://a2.example"));
    int moved = 0, kept = 0;
    for (const auto &row : rows) {
      moved += row.install.server == "https://a2.example";
      kept += row.install.server == "https://b.example";
    }
    CHECK(moved == 2 && kept == 1);
    CHECK(!h::moveServer(rows, "https://none.example", "https://x.example"));
    CHECK(!h::moveServer(rows, "", "https://x.example"));
  }
  return failures;
}
