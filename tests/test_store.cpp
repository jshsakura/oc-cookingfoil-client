#include "Test.hpp"
#include "core/DownloadStore.hpp"
#include <filesystem>
using namespace foil;
namespace {
history::Entry entry(const std::string &name, history::Outcome outcome) {
  history::Entry e;
  e.install.name = name;
  e.install.target = "sd";
  Item file;
  file.url = "https://shop.example/f/" + name;
  file.name = name;
  file.size = 1000;
  e.install.files = {file};
  e.outcome = outcome;
  e.at = 100;
  return e;
}
} // namespace
int runStoreTests() {
  int failures = 0;
  const auto dir = std::filesystem::temp_directory_path() / "cookingfoil-store-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = (dir / "cookingfoil.db").string();
  std::vector<history::Entry> list;
  {
    DownloadStore store;
    std::string error;
    CHECK(store.open(path, error));
    CHECK(store.load().empty());
    history::add(list, entry("base", history::Outcome::Queued));
    history::add(list, entry("update", history::Outcome::Queued));
    list.back().group = 3;
    auto done = entry("old", history::Outcome::Done);
    done.bytes = 4ull << 30;
    done.downloadSeconds = 120.5;
    done.installSeconds = 30;
    done.startedAt = 50;
    history::add(list, done);
    CHECK(store.save(list, error));
  }
  {
    // A fresh open reads the list back in its order, with every field.
    DownloadStore store;
    std::string error;
    CHECK(store.open(path, error));
    const auto back = store.load();
    CHECK(back.size() == 3);
    if (back.size() == 3) {
      CHECK(back[0].install.name == "old" && back[0].bytes == (4ull << 30) && back[0].downloadSeconds == 120.5 &&
            back[0].installSeconds == 30 && back[0].startedAt == 50 && back[0].outcome == history::Outcome::Done);
      CHECK(back[2].install.name == "base" && back[2].group == 3 && back[2].outcome == history::Outcome::Queued);
      CHECK(back[2].install.files.size() == 1 && back[2].install.files[0].url == "https://shop.example/f/base");
    }
    // A row that goes is deleted; a row that changes is written again.
    list.erase(list.begin());
    list[0].outcome = history::Outcome::Cancelled;
    CHECK(store.save(list, error));
    const auto again = store.load();
    CHECK(again.size() == 2 && again[0].outcome == history::Outcome::Cancelled);
    // Where a cut-off file stands, by its key, until it is forgotten.
    DownloadStore::ResumePoint point{"https://shop.example/f/base", "\"etag-1\"", 1000, "0-500-1000"};
    CHECK(store.saveResumePoint("nca:abc", point));
    const auto read = store.resumePoint("nca:abc");
    CHECK(read && read->url == point.url && read->etag == point.etag && read->total == 1000 && read->plan == point.plan);
    store.forgetResumePoint("nca:abc");
    CHECK(!store.resumePoint("nca:abc"));
  }
  {
    // Not a database: open fails with a reason instead of crashing.
    const auto bad = (dir / "bad.db").string();
    FILE *f = std::fopen(bad.c_str(), "wb");
    std::fputs("not a database at all, just text that is long enough to have a header", f);
    std::fclose(f);
    DownloadStore store;
    std::string error;
    CHECK(!store.open(bad, error) && !error.empty());
  }
  std::filesystem::remove_all(dir);
  return failures;
}
