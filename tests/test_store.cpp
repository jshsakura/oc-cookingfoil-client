#include "Test.hpp"
#include "core/DownloadStore.hpp"
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
#include <utime.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
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
bool exists(const std::string &path) { return std::filesystem::exists(path); }
std::string slurp(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
void spit(const std::string &path, const std::string &bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
}
// SQL straight on the file, as another build (or a test) would leave it.
bool raw(const std::string &path, const std::string &sql) {
  sqlite3 *db = nullptr;
  const bool opened = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, "unix-none") == SQLITE_OK;
  const bool ok = opened && sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
  sqlite3_close(db);
  return ok;
}
int userVersion(const std::string &path) {
  sqlite3 *db = nullptr;
  int version = -1;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, "unix-none") == SQLITE_OK) {
    sqlite3_stmt *read = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &read, nullptr) == SQLITE_OK && sqlite3_step(read) == SQLITE_ROW)
      version = sqlite3_column_int(read, 0);
    sqlite3_finalize(read);
  }
  sqlite3_close(db);
  return version;
}
bool healthy(const std::string &path) {
  sqlite3 *db = nullptr;
  bool ok = false;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, "unix-none") == SQLITE_OK) {
    sqlite3_stmt *check = nullptr;
    ok = sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &check, nullptr) == SQLITE_OK &&
         sqlite3_step(check) == SQLITE_ROW &&
         std::string(reinterpret_cast<const char *>(sqlite3_column_text(check, 0))) == "ok";
    sqlite3_finalize(check);
  }
  sqlite3_close(db);
  return ok;
}
// A lock file as another instance leaves it, taken `age` seconds ago.
void heldLock(const std::string &db, long age) {
  const auto at = std::time(nullptr) - age;
  spit(db + ".lock", std::to_string(at) + " 4242 other\n");
  utimbuf times{at, at};
  utime((db + ".lock").c_str(), &times);
}
double secondsOf(const std::function<void()> &work) {
  const auto start = std::chrono::steady_clock::now();
  work();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
std::vector<history::Entry> many(size_t count, const std::string &prefix) {
  std::vector<history::Entry> list;
  for (size_t i = 0; i < count; ++i) {
    history::add(list, entry(prefix + std::to_string(i), history::Outcome::Done));
    list.back().error = std::string(300, 'x'); // a few rows per page, so the file spans many pages
  }
  return list;
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
    // Not a database: the file is put aside as .broken (an older one replaced)
    // and a fresh store opens, so the app keeps working.
    const auto bad = (dir / "bad.db").string();
    spit(bad + ".broken", "an older broken file");
    spit(bad + "-journal", "a journal that belonged to the broken file");
    const std::string garbage = "not a database at all, just text that is long enough to have a header";
    spit(bad, garbage);
    DownloadStore store;
    std::string error;
    CHECK(store.open(bad, error));
    CHECK(store.isOpen() && store.load().empty());
    CHECK(slurp(bad + ".broken") == garbage);
    // Its journal is not left for the fresh file (TRUNCATE mode leaves an
    // empty one of the fresh file's own).
    CHECK(!exists(bad + "-journal") || std::filesystem::file_size(bad + "-journal") == 0);
    CHECK(store.save({entry("fresh", history::Outcome::Queued)}, error));
    CHECK(store.load().size() == 1);
  }
  {
    // A real database cut short (a card pulled mid-copy): set aside, fresh one made.
    const auto cut = (dir / "cut.db").string();
    {
      DownloadStore store;
      std::string error;
      CHECK(store.open(cut, error));
      CHECK(store.save(many(200, "row"), error));
    }
    const auto whole = slurp(cut);
    CHECK(whole.size() > 16 * 4096);
    spit(cut, whole.substr(0, whole.size() / 2));
    DownloadStore store;
    std::string error;
    CHECK(store.open(cut, error));
    CHECK(store.load().empty());
    CHECK(slurp(cut + ".broken").size() == whole.size() / 2);
    CHECK(store.save(many(3, "new"), error) && store.load().size() == 3);
  }
  {
    // A page scribbled over inside a database that still has a good header.
    const auto torn = (dir / "torn.db").string();
    {
      DownloadStore store;
      std::string error;
      CHECK(store.open(torn, error));
      CHECK(store.save(many(200, "row"), error));
    }
    auto bytes = slurp(torn);
    for (size_t at = 4096 * 3; at < 4096 * 6 && at < bytes.size(); ++at)
      bytes[at] = char(0xA5);
    spit(torn, bytes);
    DownloadStore store;
    std::string error;
    CHECK(store.open(torn, error));
    CHECK(store.load().empty() && exists(torn + ".broken"));
  }
  {
    // Somewhere it cannot be made at all: open fails with a reason, and
    // nothing is renamed.
    const auto nowhere = (dir / "missing-folder" / "cookingfoil.db").string();
    DownloadStore store;
    std::string error;
    CHECK(!store.open(nowhere, error) && !error.empty() && !store.isOpen());
    CHECK(!exists(nowhere + ".broken"));
    CHECK(store.load().empty() && !store.save({}, error) && !store.saveResumePoint("k", {}));
    CHECK(!store.resumePoint("k") && store.resumePoints().empty());
  }
  {
    // The process killed mid-transaction: the journal it left behind is hot,
    // and the next open rolls it back to the last committed list.
    const auto hot = (dir / "hot.db").string();
    const auto committed = many(20, "kept");
    {
      DownloadStore store;
      std::string error;
      CHECK(store.open(hot, error));
      CHECK(store.save(committed, error));
      CHECK(store.saveResumePoint("nca:kept", {"https://shop.example/f/kept", "e", 9, "plan"}));
    }
    // A write cut off part way leaves its temporary file (half written) and
    // perhaps an older build's journal: neither touches the file.
    {
      std::ofstream(hot + ".tmp", std::ios::binary) << std::string(3000, 'x');
      std::ofstream(hot + "-journal", std::ios::binary) << std::string(4096, 'j');
    }
    DownloadStore store;
    std::string error;
    CHECK(store.open(hot, error));
    const auto back = store.load();
    CHECK(back.size() == committed.size());
    if (back.size() == committed.size())
      CHECK(back.front().install.name == committed.front().install.name &&
            back.back().install.name == committed.back().install.name);
    CHECK(store.resumePoint("nca:kept") && store.resumePoint("nca:kept")->plan == "plan");
    CHECK(!exists(hot + ".broken") && !exists(hot + "-journal"));
    // Cut between the two renames of a write: the file gone, its .old there.
    std::filesystem::rename(hot, hot + ".old");
    DownloadStore again;
    CHECK(again.open(hot, error));
    CHECK(again.load().size() == committed.size());
  }
  {
    // A statement that fails mid-save rolls the whole save back: nothing is
    // half written, and the next save still writes everything it should.
    const auto half = (dir / "half.db").string();
    DownloadStore store;
    std::string error;
    CHECK(store.open(half, error));
    std::vector<history::Entry> list = many(5, "a");
    CHECK(store.save(list, error));
    store.load();
    CHECK(raw(half, "CREATE TRIGGER boom BEFORE INSERT ON downloads WHEN NEW.kind='boom'"
                    " BEGIN SELECT RAISE(ABORT, 'boom'); END;"));
    auto changed = list;
    changed.erase(changed.begin());            // a delete
    changed[0].outcome = history::Outcome::Failed; // an update
    history::add(changed, entry("bad", history::Outcome::Queued));
    changed.front().kind = "boom";             // the insert that fails
    error.clear();
    CHECK(!store.save(changed, error) && !error.empty());
    DownloadStore other;
    std::string otherError;
    CHECK(other.open(half, otherError));
    const auto after = other.load();
    CHECK(after.size() == 5);
    if (after.size() == 5)
      CHECK(after[1].outcome == history::Outcome::Done && after[0].install.name == list[0].install.name);
    other.close();
    CHECK(raw(half, "DROP TRIGGER boom;"));
    CHECK(store.save(changed, error));
    const auto fixed = store.load();
    CHECK(fixed.size() == 5 && fixed[0].kind == "boom");
  }
  {
    // A plan of 200 KB (a big file cut into many segments) round trips whole.
    const auto big = (dir / "big.db").string();
    std::string plan;
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < 200 * 1024; ++i)
      plan += alphabet[(i * 7 + i / 64) % 64];
    plan += "==";
    {
      DownloadStore store;
      std::string error;
      CHECK(store.open(big, error));
      CHECK(store.saveResumePoint("nca:big", {"https://shop.example/f/big", "\"e\"", 1ull << 34, plan}));
      CHECK(store.saveResumePoint("nca:small", {"u", "", 1, "0-1"}));
    }
    DownloadStore store;
    std::string error;
    CHECK(store.open(big, error));
    const auto read = store.resumePoint("nca:big");
    CHECK(read && read->plan == plan && read->total == (1ull << 34));
    const auto all = store.resumePoints();
    CHECK(all.size() == 2 && all.count("nca:big") && all.at("nca:big").plan == plan);
    CHECK(store.forgetResumePoint("nca:big") && !store.resumePoint("nca:big"));
  }
  {
    // A database from a newer build: read, never migrated, never written,
    // never set aside. Going back to that build finds it as it was.
    const auto newer = (dir / "newer.db").string();
    {
      DownloadStore store;
      std::string error;
      CHECK(store.open(newer, error));
      CHECK(store.save(many(4, "n"), error));
      CHECK(store.saveResumePoint("nca:n", {"u", "e", 5, "p"}));
    }
    CHECK(raw(newer, "PRAGMA user_version=99; ALTER TABLE downloads ADD COLUMN future TEXT;"));
    const auto bytes = slurp(newer);
    {
      DownloadStore store;
      std::string error;
      CHECK(store.open(newer, error));
      CHECK(store.load().size() == 4);
      CHECK(store.resumePoint("nca:n") && store.resumePoints().size() == 1);
      CHECK(!store.save(many(1, "z"), error) && !error.empty());
      CHECK(!store.saveResumePoint("nca:z", {"u", "e", 1, "p"}));
      CHECK(!store.forgetResumePoint("nca:n"));
    }
    CHECK(userVersion(newer) == 99 && slurp(newer) == bytes && !exists(newer + ".broken"));
  }
  {
    // An empty file (an older, never written store) migrates up.
    const auto empty = (dir / "empty.db").string();
    spit(empty, "");
    DownloadStore store;
    std::string error;
    CHECK(store.open(empty, error));
    CHECK(userVersion(empty) >= 1 && !exists(empty + ".broken"));
    CHECK(store.save(many(2, "e"), error) && store.load().size() == 2);
  }
  {
    // Two instances on one file (the app suspended in the background and a
    // second one from the Album), used in turn: each sees what the other
    // committed, and a stale picture of the file never decides what is written.
    const auto two = (dir / "two.db").string();
    DownloadStore a, b;
    std::string error;
    CHECK(a.open(two, error) && b.open(two, error));
    auto list = many(5, "t");
    CHECK(a.save(list, error));
    auto seen = b.load();
    CHECK(seen.size() == 5);
    // b changes a row; a, which still has its own list, saves it unchanged:
    // a's list is what it holds, so the row is written back to a's version.
    seen[2].outcome = history::Outcome::Failed;
    CHECK(b.save(seen, error));
    CHECK(a.load()[2].outcome == history::Outcome::Failed);
    CHECK(a.save(list, error));
    CHECK(b.load()[2].outcome == history::Outcome::Done);
    // A row b added and a never knew is not deleted by a.
    seen = b.load();
    history::add(seen, entry("from-b", history::Outcome::Queued));
    CHECK(b.save(seen, error));
    CHECK(a.save(list, error));
    bool kept = false;
    for (const auto &e : b.load())
      kept = kept || e.install.name == "from-b";
    CHECK(kept);
    // Many turns of both, every kind of write: the file stays sound.
    for (int turn = 0; turn < 60; ++turn) {
      auto &who = turn % 2 ? a : b;
      auto mine = who.load();
      if (turn % 3 == 0 && !mine.empty())
        mine.erase(mine.begin());
      history::add(mine, entry("turn" + std::to_string(turn), history::Outcome::Queued));
      CHECK(who.save(mine, error));
      CHECK(who.saveResumePoint("k" + std::to_string(turn % 4), {"u", "e", uint64_t(turn), std::string(3000, 'p')}));
      if (turn % 5 == 0)
        CHECK(who.forgetResumePoint("k" + std::to_string((turn + 1) % 4)));
    }
    CHECK(a.load().size() == b.load().size() && a.resumePoints().size() == b.resumePoints().size());
    CHECK(healthy(two) && !exists(two + ".lock") && !exists(two + ".broken"));
  }
  {
    // A lock left by an instance that died (or sleeps for long): taken over.
    const auto stale = (dir / "stale.db").string();
    DownloadStore store;
    std::string error;
    CHECK(store.open(stale, error));
    heldLock(stale, 60);
    double took = secondsOf([&] { CHECK(store.save(many(2, "s"), error)); });
    CHECK(took < 1.0);
    CHECK(store.load().size() == 2 && !exists(stale + ".lock"));
    // One held just now: the call gives up within the bound and changes nothing.
    heldLock(stale, 0);
    took = secondsOf([&] { CHECK(!store.save(many(3, "x"), error) && !error.empty()); });
    CHECK(took >= 1.5 && took < 3.0);
    CHECK(!store.saveResumePoint("k", {"u", "e", 1, "p"}));
    CHECK(exists(stale + ".lock")); // not ours, not removed
    std::filesystem::remove(stale + ".lock");
    CHECK(store.load().size() == 2);
  }
  {
    // An instance killed mid-transaction while it held the lock: the others
    // wait it out, then take the lock over and roll its journal back.
    const auto killed = (dir / "killed.db").string();
    const auto committed = many(10, "c");
    DownloadStore store;
    std::string error;
    CHECK(store.open(killed, error));
    CHECK(store.save(committed, error));
    std::cout.flush();
    std::cerr.flush();
    const pid_t child = fork();
    if (child == 0) {
      const int fd = ::open((killed + ".lock").c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
      if (fd < 0)
        _exit(3);
      const std::string token = std::to_string(std::time(nullptr)) + " child\n";
      if (::write(fd, token.data(), token.size()) != ssize_t(token.size()))
        _exit(4);
      ::close(fd);
      // Killed while writing the new file: its temporary file, half written.
      std::ofstream(killed + ".tmp", std::ios::binary) << std::string(5000, 'z');
      _exit(0);
    }
    int status = 0;
    CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(exists(killed + ".lock"));
    CHECK(store.load().empty()); // its lock is fresh: not taken yet
    heldLock(killed, 60);         // ...and later it is stale
    const auto back = store.load();
    CHECK(back.size() == committed.size());
    if (back.size() == committed.size())
      CHECK(back.front().install.name == committed.front().install.name);
    CHECK(healthy(killed) && !exists(killed + ".lock"));
    CHECK(store.save(many(4, "after"), error) && healthy(killed));
  }
  {
    // The console's "sdmc:/" names the SD card's root, which is "/" to SQLite.
    CHECK(DownloadStore::fileName("sdmc:/switch/cookingfoil/cookingfoil.db") == "/switch/cookingfoil/cookingfoil.db");
    CHECK(DownloadStore::fileName("/tmp/x.db") == "/tmp/x.db");
    CHECK(DownloadStore::fileName("sdmc:") == "sdmc:");
  }
  std::filesystem::remove_all(dir);
  return failures;
}
