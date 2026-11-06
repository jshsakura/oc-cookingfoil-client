// 패치's 펌웨어 tab: the firmware versions the server keeps, each a zip of
// .nca files unpacked into firmware/<version> on the SD card. Installing is
// Daybreak's: nothing here touches the console's system.
#include "core/I18n.hpp"
#include "core/Log.hpp"
#include "net/FirmwareApi.hpp"
#include "platform/FileSystem.hpp"
#include "platform/Storage.hpp"
#include "ui/Application.hpp"
#include <algorithm>
namespace foil::ui {
namespace {
constexpr int VisibleRows = 4;
constexpr float RowTop = 90, RowHeight = 108, RowStep = 124;
std::string where(const std::string &version) { return "/" + firmware::folder(version); }
} // namespace
std::vector<const firmware::Version *> Application::firmwareRows() const {
  std::vector<const firmware::Version *> rows;
  for (const auto &v : firmware_.versions)
    if (patchQuery_.empty() || v.version.find(patchQuery_) != std::string::npos) rows.push_back(&v);
  return rows;
}
// The list from the first server that offers it; the card read beside it.
void Application::loadFirmware() {
  const auto servers = serversWith("firmware");
  if (servers.empty()) return;
  const auto sd = sdRoot_;
  if (!firmwareOnCard_ && !firmwareCardJob_.busy())
    firmwareCardJob_.run("firmware card", [this, sd]() -> Job::Finish {
      auto found = firmware::onCard(sd);
      return [this, found = std::move(found)] { firmwareOnCard_ = found; };
    }, [this] { firmwareOnCard_ = std::set<std::string>(); });
  if (firmwareJob_.busy()) return;
  const auto server = servers.front();
  // Another server now: its list, not the old one's 304.
  auto etag = server.url == firmwareServer_ ? firmwareEtag_ : std::string();
  firmwareJob_.run("firmware list", [this, server, state = state_, etag]() mutable -> Job::Finish {
    firmware::List list;
    bool unchanged = false;
    Error error;
    const bool ok = net::firmwareList(server, state, etag, list, unchanged, error, nullptr);
    return [this, ok, unchanged, list = std::move(list), error, etag, url = server.url] {
      firmwareLoaded_ = true;
      firmwareError_ = ok ? Error{} : error;
      if (!ok) return; // the list already shown stays
      firmwareServer_ = url;
      firmwareEtag_ = etag;
      if (!unchanged) firmware_ = list;
      if (screen_ == Screen::Patches && patchFilter_ == PatchFilter::Firmware && !hasFocusable()) sidebar_ = true;
    };
  }, [this] {
    firmwareLoaded_ = true;
    firmwareError_ = Error{tr("Could not read the firmware list.")};
  });
}
void Application::pollFirmware() {
  firmwareJob_.poll();
  firmwareCardJob_.poll();
}
void Application::firmwareTab(float titleRight) {
  if (!firmwareLoaded_ && !firmwareJob_.busy() && connected_) loadFirmware();
  // The server could not reach where it takes firmware from: what it keeps
  // still comes, the rest reads as not available.
  if (!firmware_.reachable) {
    const std::string label = tr("Source offline");
    paint_.rect(titleRight + 16, 48 - 4, 8, 8, Pending, 4);
    paint_.fonts.middle(label, titleRight + 30, 48, 15, Secondary);
  }
  const auto rows = firmwareRows();
  if (rows.empty()) {
    if (!firmware_.versions.empty() && !patchQuery_.empty())
      notice(trf("No patches match '{0}'.", {patchQuery_}), tr("Press B to clear the search."), 23);
    else if (firmwareJob_.busy() || (!firmwareLoaded_ && connected_))
      for (int n = 0; n <= VisibleRows; ++n) paint_.skeleton(260, RowTop + n * RowStep, 992, RowHeight, 18);
    else if (!connected_)
      offlineNotice(tr("Connect to a server to load the patch list."));
    else if (firmwareError_)
      notice(tr("Could not read the firmware list."), firmwareError_.message, 25);
    else
      notice(tr("The server lists no firmware yet."), "", 32);
    return;
  }
  selected_ = std::clamp(selected_, 0, int(rows.size()) - 1);
  const float top = glide("firmware", 232, RowTop, 1048, 660 - RowTop,
                          float(std::clamp(selected_ - VisibleRows / 2, 0, std::max(0, int(rows.size()) - VisibleRows))),
                          float(int(rows.size()) - VisibleRows), RowStep);
  const int first = std::max(0, int(top));
  paint_.scrollbar(1266, RowTop, RowStep * VisibleRows - 16, top, VisibleRows, float(rows.size()));
  const auto console = platform::firmwareVersion();
  clipList(RowTop - 8, 660 - RowTop + 8);
  for (int i = first; i < int(rows.size()) && i <= first + VisibleRows + 1; ++i) {
    const auto &v = *rows[i];
    const float y = RowTop + (i - top) * RowStep;
    rowHit(260, y, 992, RowHeight, 18, [this, i] {
      if (inContent() && selected_ == i) action(Action::Accept);
      sidebar_ = false;
      selected_ = i;
    });
    if (inContent() && i == selected_) paint_.ring(260, y, 992, RowHeight, 18);
    paint_.rect(260, y, 992, RowHeight, Surface, 18);
    paint_.icon(34, 290, y + 30, v.available ? Ink : Secondary, 48);
    paint_.fonts.text(v.version, 362, y + 18, 26, v.available ? Ink : Secondary, true);
    std::string meta = v.released;
    if (v.archive.size) meta += (meta.empty() ? "" : " · ") + formatBytes(v.archive.size);
    paint_.fonts.text(meta, 362, y + 60, 18, Secondary, false, 600);
    // Right column: where it stands; a running one shows its bar.
    const float right = 1230;
    const bool running = retroBusy_ == "firmware:" + v.version;
    const bool queued = std::any_of(installQueue_.begin(), installQueue_.end(), [&](const QueuedInstall &q) {
      firmware::Version other;
      return q.kind == "firmware" && firmware::parseOne(q.payload, other) && other.version == v.version;
    });
    const bool stored = firmwareOnCard_ && firmwareOnCard_->count(v.version);
    const std::string state = running ? tr("Downloading")
                              : queued ? tr("Queued")
                              : stored ? tr("On SD card")
                              : !v.available ? tr("Not available") : "";
    if (!state.empty()) {
      const SDL_Color fill = running || queued ? Butter : stored ? Good : Line;
      const SDL_Color ink = running || queued ? OnButter : stored ? White : Ink;
      const float w = paint_.fonts.measure(state, 16, true) + 20;
      paint_.rect(right - w, y + 16, w, 30, fill, 8);
      paint_.fonts.middle(state, right - w + 10, y + 31, 16, ink, true);
    }
    if (!console.empty() && console == v.version) {
      const std::string mine = tr("This console");
      const float w = paint_.fonts.measure(mine, 16, true) + 20;
      const float x = state.empty() ? right - w : right - w - paint_.fonts.measure(state, 16, true) - 28;
      paint_.rect(x, y + 16, w, 30, Line, 8);
      paint_.fonts.middle(mine, x + 10, y + 31, 16, Ink, true);
    }
    if (running && retroBytes_ > 0) {
      const float ratio = std::min(1.f, float(retroReceived_.load()) / float(retroBytes_.load()));
      paint_.rect(right - 180, y + 76, 180, 8, Line, 4);
      paint_.rect(right - 180, y + 76, std::max(8.f, 180 * ratio), 8, Butter, 4);
    }
  }
  unclip();
}
void Application::firmwareAction(Action a) {
  const auto rows = firmwareRows();
  const auto *picked = rows.empty() ? nullptr : rows[std::clamp(selected_, 0, int(rows.size()) - 1)];
  if (firmwareAsk_) {
    if (!picked || a == Action::Back) firmwareAsk_ = false;
    else if (a == Action::Accept && settled()) {
      firmwareAsk_ = false;
      queueFirmware(*picked);
    } else if (a == Action::Search && settled() && firmwareOnCard_ && firmwareOnCard_->count(picked->version)) {
      firmwareAsk_ = false;
      if (firmwareCardJob_.busy()) return;
      const auto version = picked->version;
      firmwareCardJob_.run("firmware delete", [this, version, sd = sdRoot_]() -> Job::Finish {
        std::error_code ec;
        const bool ok = platform::removePath(sd + "/" + firmware::folder(version), ec);
        return [this, ok, version, why = ec.message()] {
          cardChanged();
          if (!ok) {
            alertTitle_ = tr("Could not delete the firmware");
            alert_ = why;
            return;
          }
          if (firmwareOnCard_) firmwareOnCard_->erase(version);
          message(trf("Deleted firmware {0}.", {version}));
        };
      }, [] {});
    }
    return;
  }
  if (rows.empty()) {
    if (a == Action::Left) sidebar_ = true;
    return;
  }
  if (a == Action::Up) selected_ = std::max(0, selected_ - 1);
  if (a == Action::Down) selected_ = std::min(int(rows.size()) - 1, selected_ + 1);
  if (a == Action::Left) sidebar_ = true;
  selected_ = std::clamp(selected_, 0, int(rows.size()) - 1);
  if (a != Action::Accept) return;
  const auto &v = *rows[selected_];
  if (!v.available) {
    message(tr("This firmware cannot be downloaded right now."));
    return;
  }
  firmwareAsk_ = true;
}
// A on a version: what it does, a warning when it is older than the
// console's, and X to delete the copy already on the card.
void Application::firmwareDialog() {
  const auto rows = firmwareRows();
  if (rows.empty()) {
    firmwareAsk_ = false;
    return;
  }
  const auto &v = *rows[std::clamp(selected_, 0, int(rows.size()) - 1)];
  const auto console = platform::firmwareVersion();
  const bool older = !console.empty() && firmware::compare(v.version, console) < 0;
  const bool stored = firmwareOnCard_ && firmwareOnCard_->count(v.version);
  constexpr float Width = 720, Text = Width - 64;
  const auto unpack = trf("Unpacks {0} into {1} on the SD card.",
                          {formatBytes(std::max(v.archive.unpacked, v.archive.size)), where(v.version)});
  const std::string daybreak = tr("Install it with Daybreak. The console stays as it is until then.");
  const auto warning = older ? trf("Older than this console's firmware {0}. Going back a version can stop games and saves from working.", {console})
                             : std::string();
  const float unpackH = paint_.fonts.lineCount(unpack, 19, Text, 2) * 19 * 1.4f;
  const float daybreakH = paint_.fonts.lineCount(daybreak, 19, Text, 2) * 19 * 1.4f;
  const float warningH = older ? paint_.fonts.lineCount(warning, 19, Text, 3) * 19 * 1.4f + 12 : 0;
  const auto body = modal(Width, 24 + unpackH + 8 + daybreakH + warningH + 16, trf("Download firmware {0}?", {v.version}),
                          tr("Cancel"), "", stored ? tr("Download again") : tr("Download"), 34,
                          stored ? std::string(tr("Delete")) : std::string());
  const float x = body.x + 32;
  float y = body.y + 24;
  paint_.fonts.wrapped(unpack, x, y, 19, Ink, Text, 2, false, 1.4f);
  y += unpackH + 8;
  paint_.fonts.wrapped(daybreak, x, y, 19, Secondary, Text, 2, false, 1.4f);
  y += daybreakH + 12;
  if (older) paint_.fonts.wrapped(warning, x, y, 19, Bad, Text, 3, true, 1.4f);
}
// Into 다운로드's queue like any download, with room for the unpacked files
// beside what the queue already needs on the card.
bool Application::queueFirmware(const firmware::Version &v) {
  const uint64_t room = std::max(v.archive.unpacked, v.archive.size);
  uint64_t queued = 0;
  for (const auto &q : installQueue_)
    if (q.target != "nand") queued = q.file.size > UINT64_MAX - queued ? UINT64_MAX : queued + q.file.size;
  const uint64_t slack = 8u << 20;
  if (sdFree_ && (room > UINT64_MAX - queued - slack || *sdFree_ < room + queued + slack)) {
    alertTitle_ = tr("Could not download the firmware");
    alert_ = trf("Not enough space on the SD card. {0} is needed.", {formatBytes(room)});
    return false;
  }
  return queueJob("firmware", trf("Firmware {0}", {v.version}), "", v.archive.url, v.archive.size, firmware::serialize(v),
                  0, false, firmwareServer_);
}
bool Application::runFirmware(const firmware::Version &v, const Server &server) {
  retroBytes_ = v.archive.size;
  retroCancel_ = false;
  return runRetroJob("firmware:" + v.version, [this, v, server, state = state_, sd = sdRoot_]() -> std::function<void()> {
    Error error;
    const bool ok = net::firmwareDownload(server, state, v, sd, retroReceived_, error, &retroCancel_);
    const bool cancelled = retroCancel_;
    const uint64_t received = retroReceived_;
    return [this, ok, cancelled, received, error, v] {
      cardChanged();
      if (ok && firmwareOnCard_) firmwareOnCard_->insert(v.version);
      if (finishRetroEntry("firmware", ok ? history::Outcome::Done : cancelled ? history::Outcome::Cancelled : history::Outcome::Failed,
                           ok || cancelled ? "" : error.message, received))
        return;
      if (cancelled && !ok) {
        message(trf("Cancelled the download of {0}.", {trf("Firmware {0}", {v.version})}));
        return;
      }
      if (!ok) {
        alertTitle_ = tr("Could not download the firmware");
        alert_ = error.message;
        return;
      }
      message(trf("Firmware {0} is in {1}. Install it with Daybreak.", {v.version, where(v.version)}));
    };
  });
}
} // namespace foil::ui
