#include "ui/Application.hpp"
#include "core/Log.hpp"
#include "platform/Saves.hpp"
#include <algorithm>
namespace foil::ui {
namespace {
constexpr int VisibleRows = 4;
std::string when(std::time_t at) {
  std::tm local{};
  localtime_r(&at, &local);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
  return text;
}
} // namespace
std::string Application::backupDetail(const saves::LocalBackup &b) const {
  std::string detail = saves::deviceBackup(b)      ? std::string(tr("Shared save"))
                       : b.meta.userName.empty() ? std::string(tr("Backup without details"))
                                                 : b.meta.userName;
  if (!b.meta.deviceLabel.empty()) detail += " · " + b.meta.deviceLabel;
  if (b.meta.isAutoBackup) detail += " · " + std::string(tr("Auto backup before restore"));
  return detail;
}
void Application::openSave(int index) {
  if (!managedGame_ || index < 0 || index >= int(saves_.size())) return;
  saveOpen_ = index;
  saveRow_ = 0;
  saveConfirm_ = SaveConfirm::None;
  backups_ = saves::listBackups(saves::keeperRoot(sdRoot_), managedGame_->applicationId);
  // This console's id, to tell backups made elsewhere.
  if (thisDevice_.empty()) {
    const auto device = saves::loadDevice(saves::keeperRoot(sdRoot_), std::time(nullptr), unsigned(std::clock()));
    if (!device.error) thisDevice_ = device.value.id;
  }
}
void Application::runSaveJob(bool restore) {
  if (!managedGame_ || !saveOpen_ || deviceBusy_ || saveWorker_.joinable()) return;
  if (installer_.running()) { message(tr("Manage saves after the install finishes.")); return; }
  platform::SaveJob job;
  job.save = saves_[*saveOpen_];
  job.game = *managedGame_;
  job.sd = sdRoot_;
  job.copied = &saveCopied_;
  std::optional<saves::LocalBackup> source;
  if (restore) {
    if (saveRow_ < 1 || saveRow_ > int(backups_.size())) return;
    source = backups_[saveRow_ - 1];
  }
  saveCopied_ = 0;
  saveRestoring_ = restore;
  deviceBusy_ = true;
  saveWorker_ = std::thread([this, job, source] {
    if (!guarded("saves", [this, &job, &source] {
          saveResult_ = source ? platform::restoreSave(job, *source) : platform::backupSave(job).error;
        }))
      saveResult_.message = tr("Something went wrong while handling the save.");
    saveDone_ = true;
  });
}
void Application::pollSaveJob() {
  if (!saveDone_.exchange(false)) return;
  saveWorker_.join();
  deviceBusy_ = false;
  if (managedGame_) {
    backups_ = saves::listBackups(saves::keeperRoot(sdRoot_), managedGame_->applicationId);
    backupCounts_.erase(managedGame_->applicationId);
  }
  // After a backup the cursor stays on "back up"; the new one is listed first.
  saveRow_ = std::clamp(saveRow_, 0, int(backups_.size()) + 1);
  if (saveResult_) logLine("saves: " + saveResult_.message);
  if (applyRunning_) {
    // A shop save went in: the dialog closes on success, stays on failure.
    applyRunning_ = false;
    if (saveResult_) {
      alertTitle_ = tr("Could not apply the save");
      alert_ = saveResult_.message;
    } else {
      message(trf("Applied {0}. The game starts with it next time.", {applying_ ? applying_->name : ""}));
      if (managedGame_) backupCounts_.erase(managedGame_->applicationId);
      applying_.reset();
      managedGame_.reset();
      saves_.clear();
    }
    saveResult_ = {};
    return;
  }
  message(saveResult_ ? saveResult_.message : saveRestoring_ ? tr("Restored.") : tr("Backed up."));
  saveResult_ = {};
}
void Application::saveAction(Action a) {
  if (saveConfirm_ != SaveConfirm::None) {
    // One decision in the footer: A does it, B backs out.
    if (a == Action::Back) saveConfirm_ = SaveConfirm::None;
    if (a != Action::Accept) return;
    const auto confirmed = std::exchange(saveConfirm_, SaveConfirm::None);
    if (confirmed == SaveConfirm::Restore) {
      runSaveJob(true);
      return;
    }
    if (auto error = saves::moveToTrash(saves::keeperRoot(sdRoot_), backups_[saveRow_ - 1], std::time(nullptr)))
      message(error.message);
    else
      message(tr("Moved the backup to the trash."));
    backupCounts_.erase(managedGame_->applicationId);
    backups_ = saves::listBackups(saves::keeperRoot(sdRoot_), managedGame_->applicationId);
    saveRow_ = std::clamp(saveRow_, 0, int(backups_.size()) + 1);
    return;
  }
  if (deviceBusy_) return;
  const int last = int(backups_.size()) + 1;
  const bool onBackup = saveRow_ >= 1 && saveRow_ < last;
  if (a == Action::Back) { saveOpen_.reset(); return; }
  if (a == Action::Up) saveRow_ = std::max(0, saveRow_ - 1);
  if (a == Action::Down) saveRow_ = std::min(last, saveRow_ + 1);
  if ((a == Action::Search || a == Action::Queue) && onBackup) {
    saveConfirm_ = SaveConfirm::Trash;
  }
  if (a != Action::Accept) return;
  // No save yet: nothing to back up or delete, only a backup to restore.
  if (saves_[*saveOpen_].missing && (saveRow_ == 0 || saveRow_ == last)) {
    message(tr("No save yet. Pick a backup to restore and one is created."));
    return;
  }
  if (saveRow_ == 0) {
    runSaveJob(false);
  } else if (onBackup) {
    // A shared save's backup never goes into an account, nor the other way.
    if (auto error = saves::restoreMismatch(backups_[saveRow_ - 1], saves_[*saveOpen_].device())) message(error.message);
    else saveConfirm_ = SaveConfirm::Restore;
  } else {
    RemovalRequest request;
    request.applicationId = managedGame_->applicationId;
    request.kind = RemovalKind::Save;
    request.save = saves_[*saveOpen_];
    removal_ = request;
    removalAt_ = SDL_GetTicks();
  }
}
void Application::savePanel() {
  const auto &save = saves_[*saveOpen_];
  const int last = int(backups_.size()) + 1;
  const bool onBackup = saveRow_ >= 1 && saveRow_ < last;
  constexpr float Row = 64, Gap = 10, Label = 34;
  const int listed = std::min(VisibleRows, last);
  // Y moves a backup to the trash; it only shows on a backup row.
  const auto body = modal(860, 20 + 28 + Row + 16 + Label + listed * (Row + Gap) + 6, managedGame_->name, tr("Back"),
                          onBackup ? tr("Trash") : "", "", 36);
  const float x = body.x + 32, w = body.w - 64;
  float y = body.y + 20;
  paint_.fonts.text(save.missing ? trf("{0} · No save yet", {save.userName})
                                 : trf("{0}'s save · {1}", {save.userName, formatBytes(save.bytes)}),
                    x, y, 17, Secondary, false, w);
  y += 28;
  const bool idle = !removal_ && saveConfirm_ == SaveConfirm::None;
  // Back up now.
  hit(x, y, w, Row, [this] { saveRow_ = 0; saveAction(Action::Accept); });
  if (saveRow_ == 0 && idle) paint_.focusRow(x, y, w, Row, Ground, 12);
  else paint_.rect(x, y, w, Row, Ground, 12);
  paint_.icon(4, x + 20, y + Row / 2 - 13, Ink, 26);
  if (deviceBusy_ && !removalOperation_) {
    const auto copied = formatBytes(saveCopied_.load());
    paint_.fonts.text(saveRestoring_ ? trf("Restoring · {0}", {copied}) : trf("Backing up · {0}", {copied}),
                      x + 62, y + 8, 21, Ink, true);
    paint_.fonts.text(tr("Keep the console on until it finishes."), x + 62, y + 37, 16, Secondary);
  } else {
    if (save.missing) {
      paint_.fonts.text(tr("No save to back up"), x + 62, y + 8, 21, Secondary, true);
      paint_.fonts.text(save.device() ? tr("Restoring a backup below creates the shared save.")
                                      : tr("Restoring a backup below creates a save for this account."),
                        x + 62, y + 37, 16, Secondary, false, w - 82);
    } else {
      paint_.fonts.text(tr("Back up the current save"), x + 62, y + 8, 21, Ink, true);
      paint_.fonts.text(tr("Saved in the oc-save-keeper folder on the SD card, shared by both apps."), x + 62, y + 37, 16, Secondary,
                        false, w - 82);
    }
  }
  y += Row + 16;
  paint_.fonts.text(backups_.empty() ? std::string(tr("No backups yet.")) : trf("{0} backups", {std::to_string(backups_.size())}),
                    x, y + 4, 19, Secondary, true);
  y += Label;
  // Backups, then "delete this save".
  const int first = std::clamp(saveRow_ - VisibleRows / 2, 1, std::max(1, last - VisibleRows + 1));
  for (int row = first; row <= last && row < first + VisibleRows; ++row, y += Row + Gap) {
    hit(x, y, w, Row, [this, row] { saveRow_ = row; saveAction(Action::Accept); });
    if (saveRow_ == row && idle) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    if (row == last) {
      paint_.icon(37, x + 20, y + Row / 2 - 13, Bad, 26);
      paint_.fonts.text(tr("Delete save"), x + 62, y + 8, 21, Bad, true);
      paint_.fonts.text(save.device() ? tr("Deletes play data shared by all users.") : tr("Deletes this user's play data."),
                        x + 62, y + 37, 16, Secondary);
      continue;
    }
    const auto &b = backups_[row - 1];
    // A backup of the other kind of save is listed greyed: it cannot go here.
    const bool fits = !saves::restoreMismatch(b, save.device());
    paint_.icon(30, x + 20, y + Row / 2 - 13, fits ? Ink : Secondary, 26);
    paint_.fonts.text(when(b.timestamp), x + 62, y + 8, 21, fits ? Ink : Secondary, true);
    paint_.fonts.text(backupDetail(b), x + 62, y + 37, 16, Secondary, false, w - 220);
    const auto size = formatBytes(uint64_t(b.size));
    paint_.fonts.middle(size, x + w - 20 - paint_.fonts.measure(size, 18, true), y + Row / 2, 18, Ink, true);
  }
  if (saveConfirm_ == SaveConfirm::None) return;
  // Confirmation over the panel, one decision in its footer.
  const auto &b = backups_[saveRow_ - 1];
  const bool restore = saveConfirm_ == SaveConfirm::Restore;
  paint_.rect(0, 0, 1280, 720, {0, 0, 0, 150});
  blockTouch();
  std::string text = !restore     ? std::string(tr("Moves it to oc-save-keeper's trash, where that app can bring it back."))
                     : save.missing ? trf("Creates a save for {0} and puts this backup in it.", {save.userName})
                                    : trf("Replaces {0}'s current save with this backup. The current save is backed up first.", {save.userName});
  if (restore && b.hasMeta && !b.meta.userId.empty() && b.meta.userId != saves::userIdOf(save.user))
    text += " " + trf("This backup was made by {0}.", {b.meta.userName.empty() ? b.meta.userId : b.meta.userName});
  // A backup made on another console (its device id differs from this one's).
  if (restore && b.hasMeta && !b.meta.deviceId.empty() && !thisDevice_.empty() && b.meta.deviceId != thisDevice_)
    text += " " + trf("This backup was made on another console ({0}).", {b.meta.deviceLabel.empty() ? b.meta.deviceId.substr(0, 8) : b.meta.deviceLabel});
  const auto confirm = modal(700, 20 + 30 + 84 + 16, restore ? tr("Restore this backup?") : tr("Move the backup to the trash?"),
                             tr("Cancel"), "", restore ? tr("Restore") : tr("Move"), restore ? 3 : 37);
  paint_.fonts.text(when(b.timestamp), confirm.x + 32, confirm.y + 20, 21, Ink, true);
  paint_.fonts.wrapped(text, confirm.x + 32, confirm.y + 56, 19, Secondary, confirm.w - 64, 3);
}
} // namespace foil::ui
