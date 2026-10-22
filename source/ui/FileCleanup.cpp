// 설정 > 파일 정리: one popup for what the app leaves on the card. It shows
// how much each kind takes (leftover install files, half-downloaded files,
// the image cache) and deletes the ones picked, all on a worker thread.
#include "ui/Application.hpp"
#include "ui/Images.hpp"
#include "platform/Cores.hpp"
#include "platform/Installed.hpp"
#include "core/Log.hpp"
#include "core/Resume.hpp"
#include <algorithm>
#include <filesystem>
namespace foil::ui {
namespace {
bool endsWith(const std::string &text, const std::string &end) {
  return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}
// Files a download stopped part way leaves beside where it was going: an
// app's or a retro game's .part (and its .resume or .plan note), and the record of
// an app's folder install. A patch keeps its own and settles them itself.
std::vector<std::pair<std::string, uint64_t>> downloadPieces(const std::string &sd) {
  namespace fs = std::filesystem;
  std::vector<std::pair<std::string, uint64_t>> out;
  for (const char *root : {"switch", "retroarch"}) {
    std::error_code ec;
    fs::recursive_directory_iterator it(sd + "/" + root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      const auto name = it->path().filename().string();
      if (it->is_directory(ec)) {
        if (endsWith(name, ".part")) it.disable_recursion_pending(); // a patch's staging
        continue;
      }
      const bool record = it->path().parent_path().filename() == "resume" && name.rfind("homebrew-", 0) == 0 &&
                          endsWith(name, ".json");
      if (!endsWith(name, ".part") && !endsWith(name, std::string(".part") + ResumeFile) &&
          !endsWith(name, std::string(".part") + PlanFile) && !record) continue;
      std::error_code size;
      const auto bytes = it->file_size(size);
      out.push_back({it->path().string(), size ? 0 : uint64_t(bytes)});
    }
  }
  return out;
}
} // namespace
bool Application::tidyLeftoversAllowed() const { return !installer_.running() && !deviceBusy_; }
// A row still queued or running may go on from its .part: none are touched
// until the queue is empty.
bool Application::tidyPiecesAllowed() const {
  if (downloadRunning()) return false;
  return std::none_of(history_.begin(), history_.end(), [](const history::Entry &e) {
    return e.outcome == history::Outcome::Queued || e.outcome == history::Outcome::Running;
  });
}
uint64_t Application::tidyBytes(int row) const {
  if (row == 0) return tidyFound_.leftovers.error ? 0 : tidyFound_.leftovers.value.bytes;
  if (row == 1) return tidyFound_.pieceBytes;
  return uint64_t(std::max<int64_t>(0, imageCacheTally()));
}
// Picked, and there is something there to delete now.
bool Application::tidyPicked(int row) const {
  if (!tidyOn_[row] || tidyBusy_) return false;
  if (row == 0) return tidyKnown_ && tidyLeftoversAllowed() && tidyBytes(0) > 0;
  if (row == 1) return tidyKnown_ && tidyPiecesAllowed() && tidyBytes(1) > 0;
  return tidyBytes(2) > 0 && !imageCacheFolder().empty();
}
void Application::openFileCleanup() {
  tidyOpen_ = true;
  tidyRow_ = 0;
  if (tidyBusy_) return; // a look still going: it shows when it ends
  tidyKnown_ = false;
  tidyOn_ = {true, true, false};
  if (tidyWorker_.joinable()) tidyWorker_.join();
  tidyBusy_ = true;
  tidyDeleting_ = false;
  cleanupProgress_.stage = cleanupProgress_.done = cleanupProgress_.total = 0;
  cleanupProgress_.listed = false;
  cleanupProgress_.bytes = 0;
  // The pieces are measured even while the queue runs, so the size shows;
  // only deleting them waits for the queue.
  const bool leftovers = tidyLeftoversAllowed();
  tidyWorker_ = std::thread([this, leftovers, sd = sdRoot_] {
    const platform::OnCore off(platform::WriterCore); // the drawing core keeps the popup moving
    TidyFound found;
    if (!guarded("cleanup look", [&] {
          if (leftovers)
            found.leftovers = platform::cleanOrphans(false, &cleanupProgress_, [this](const std::string &key) {
              return installer_.resumePoint(key).has_value();
            });
          if (!sd.empty()) found.pieces = downloadPieces(sd);
          for (const auto &[path, bytes] : found.pieces) found.pieceBytes += bytes;
        }))
      found.leftovers = {{}, {tr("Cleanup failed. Check log.txt.")}};
    tidyIncoming_ = std::move(found);
    tidyDone_ = true;
  });
}
void Application::deletePickedFiles() {
  const bool leftovers = tidyPicked(0), pieces = tidyPicked(1), cache = tidyPicked(2);
  if (!leftovers && !pieces && !cache) return;
  if (tidyWorker_.joinable()) tidyWorker_.join();
  tidyBusy_ = tidyDeleting_ = true;
  tidyRemoving_ = {leftovers, pieces, cache};
  cleanupProgress_.stage = cleanupProgress_.done = cleanupProgress_.total = 0;
  cleanupProgress_.listed = false;
  cleanupProgress_.bytes = 0;
  if (cache) cacheBusy_ = true;
  tidyWorker_ = std::thread([this, leftovers, pieces, cache, list = tidyFound_.pieces, folder = imageCacheFolder()] {
    const platform::OnCore off(platform::WriterCore);
    TidyFound freed;
    if (!guarded("cleanup", [&] {
          if (leftovers)
            freed.leftovers = platform::cleanOrphans(true, &cleanupProgress_, [this](const std::string &key) {
              return installer_.resumePoint(key).has_value();
            });
          if (pieces)
            for (const auto &[path, bytes] : list) {
              std::error_code ec;
              if (std::filesystem::remove(path, ec)) freed.pieceBytes += bytes;
            }
          if (cache) {
            const auto before = std::max<int64_t>(0, imageCacheTally());
            clearImageCache(folder);
            freed.cacheBytes = uint64_t(std::max<int64_t>(0, before - std::max<int64_t>(0, imageCacheTally())));
          }
        }))
      freed.leftovers = {{}, {tr("Cleanup failed. Check log.txt.")}};
    if (cache) cacheBusy_ = false;
    tidyIncoming_ = std::move(freed);
    tidyDone_ = true;
  });
}
void Application::pollFileCleanup() {
  if (!tidyDone_.exchange(false)) return;
  tidyWorker_.join();
  tidyBusy_ = false;
  if (!tidyDeleting_) {
    tidyFound_ = std::move(tidyIncoming_);
    tidyKnown_ = true;
    return;
  }
  tidyDeleting_ = false;
  tidyKnown_ = false;
  tidyOpen_ = false;
  const auto done = std::move(tidyIncoming_);
  tidyFound_ = {};
  const uint64_t freed = (done.leftovers.error ? 0 : done.leftovers.value.bytes) + done.pieceBytes + done.cacheBytes;
  if (tidyRemoving_[0] || tidyRemoving_[1]) cardChanged(); // free space changed
  if (tidyRemoving_[0]) refreshInstalled();
  if (done.leftovers.error) {
    alertTitle_ = tr("Could not clean up leftovers");
    alert_ = done.leftovers.error.message;
    return;
  }
  message(trf("Cleanup finished. Freed {0}.", {formatBytes(freed)}));
}
void Application::fileCleanupAction(Action a) {
  if (tidyDeleting_) return; // the files are going: nothing to press
  if (a == Action::Back) tidyOpen_ = false;
  else if (a == Action::Up) tidyRow_ = std::max(0, tidyRow_ - 1);
  else if (a == Action::Down) tidyRow_ = std::min(2, tidyRow_ + 1);
  else if (a == Action::Accept) tidyOn_[tidyRow_] = !tidyOn_[tidyRow_];
  else if (a == Action::Search && settled()) deletePickedFiles();
}
void Application::fileCleanupDialog() {
  constexpr float Width = 760, Row = 76, Gap = 8;
  const bool any = tidyPicked(0) || tidyPicked(1) || tidyPicked(2);
  const auto body = modal(Width, 16 + 3 * (Row + Gap) + 64, tr("File cleanup"), tidyDeleting_ ? "" : tr("Close"), "",
                          tidyDeleting_ ? "" : tr("Select"), 14, any && !tidyDeleting_ ? tr("Delete") : "");
  const float x = body.x + 24, w = body.w - 48, top = body.y + 16;
  struct Line {
    std::string name, note;
    bool allowed;
  };
  const Line lines[3] = {
      {tr("Leftover install files"),
       tidyLeftoversAllowed() || tidyDeleting_ ? tr("Cut-off installs and unused files")
                                               : tr("Can be cleaned once the install finishes"),
       tidyLeftoversAllowed()},
      {tr("Orphaned downloads"),
       tidyPiecesAllowed() ? tr("Temporary files of downloads no longer queued")
                           : tr("Available once the queue is empty"),
       tidyPiecesAllowed()},
      {tr("Image cache"), tr("Covers and screenshots, fetched again"), !imageCacheFolder().empty()},
  };
  uint64_t picked = 0;
  for (int n = 0; n < 3; ++n) {
    const float y = top + n * (Row + Gap), mid = y + Row / 2;
    if (!tidyDeleting_)
      hit(x, y, w, Row, [this, n] {
        tidyRow_ = n;
        action(Action::Accept);
      });
    if (n == tidyRow_ && !tidyDeleting_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    // A mark like the filter popup's: filled when it will be deleted.
    const bool on = tidyPicked(n) || (tidyDeleting_ && tidyRemoving_[n]);
    paint_.rect(x + 18, mid - 11, 22, 22, on ? Ink : SDL_Color{176, 174, 167, 255}, 6);
    paint_.rect(x + 20, mid - 9, 18, 18, on ? Butter : Surface, 4);
    if (on) paint_.icon(8, x + 20, mid - 9, Ink, 18);
    const auto &line = lines[n];
    paint_.fonts.text(line.name, x + 56, y + 12, 21, line.allowed ? Ink : Secondary, true, w - 260);
    paint_.fonts.text(line.note, x + 56, y + 44, 17, Secondary, false, w - 260);
    // How much, or what it is doing now. The pieces show their size while
    // the queue runs, greyed: they can be deleted once it is empty.
    const bool measured = line.allowed || n == 1;
    const bool looking = n < 2 && measured && !tidyKnown_;
    const std::string size = tidyDeleting_ && tidyRemoving_[n] ? std::string(tr("Deleting"))
                             : !measured                        ? std::string()
                             : looking                          ? std::string(tr("Checking|size"))
                             : tidyBytes(n) == 0                ? std::string(tr("Nothing|size"))
                                                                : formatBytes(tidyBytes(n));
    paint_.fonts.middle(size, x + w - 20 - paint_.fonts.measure(size, 19, true), mid, 19, looking || !line.allowed ? Secondary : Ink, true);
    if (tidyPicked(n)) picked += tidyBytes(n);
  }
  // What A on X frees, and what stays.
  const float y = top + 3 * (Row + Gap) + 8;
  if (tidyDeleting_) {
    paint_.fonts.text(tr("Keep the console on until it finishes."), x + 4, y + 8, 18, Secondary, false, w - 8);
    return;
  }
  paint_.fonts.text(tr("Installed games and saves stay."), x + 4, y + 8, 18, Secondary, false, w - 220);
  if (picked) {
    const auto total = formatBytes(picked), label = std::string(tr("Selected|files")) + " ";
    const float tw = paint_.fonts.measure(total, 20, true);
    paint_.fonts.text(total, x + w - 4 - tw, y + 6, 20, Ink, true);
    paint_.fonts.text(label, x + w - 4 - tw - paint_.fonts.measure(label, 18), y + 8, 18, Secondary);
  }
}
} // namespace foil::ui
