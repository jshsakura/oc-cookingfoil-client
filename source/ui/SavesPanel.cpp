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
  std::string detail = saves::deviceBackup(b)      ? std::string(tr("공용 세이브"))
                       : b.meta.userName.empty() ? std::string(tr("정보 없는 백업"))
                                                 : b.meta.userName;
  if (!b.meta.deviceLabel.empty()) detail += " · " + b.meta.deviceLabel;
  if (b.meta.isAutoBackup) detail += " · " + std::string(tr("복원 전 자동 백업"));
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
  if (installer_.running()) { message(tr("설치가 끝난 뒤 세이브를 관리합니다.")); return; }
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
      saveResult_.message = tr("세이브를 다루다 오류가 났습니다.");
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
      alertTitle_ = tr("세이브를 적용하지 못했습니다");
      alert_ = saveResult_.message;
    } else {
      message(trf("{0}을(를) 적용했습니다. 게임을 실행하면 바뀐 세이브로 시작합니다.", {applying_ ? applying_->name : ""}));
      if (managedGame_) backupCounts_.erase(managedGame_->applicationId);
      applying_.reset();
      managedGame_.reset();
      saves_.clear();
    }
    saveResult_ = {};
    return;
  }
  message(saveResult_ ? saveResult_.message : saveRestoring_ ? tr("복원했습니다.") : tr("백업했습니다."));
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
      message(tr("백업을 휴지통으로 옮겼습니다."));
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
    message(tr("아직 세이브가 없습니다. 백업을 골라 복원하면 새로 만듭니다."));
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
  const auto body = modal(860, 20 + 28 + Row + 16 + Label + listed * (Row + Gap) + 6, managedGame_->name, tr("뒤로"),
                          onBackup ? tr("휴지통") : "", "", 36);
  const float x = body.x + 32, w = body.w - 64;
  float y = body.y + 20;
  paint_.fonts.text(save.missing ? trf("{0} · 아직 세이브가 없습니다", {save.userName})
                                 : trf("{0} 세이브 · {1}", {save.userName, formatBytes(save.bytes)}),
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
    paint_.fonts.text(saveRestoring_ ? trf("복원하고 있습니다 · {0}", {copied}) : trf("백업하고 있습니다 · {0}", {copied}),
                      x + 62, y + 8, 21, Ink, true);
    paint_.fonts.text(tr("끝날 때까지 본체를 끄지 않습니다."), x + 62, y + 37, 16, Secondary);
  } else {
    if (save.missing) {
      paint_.fonts.text(tr("백업할 세이브가 없습니다"), x + 62, y + 8, 21, Secondary, true);
      paint_.fonts.text(save.device() ? tr("아래 백업을 복원하면 공용 세이브를 새로 만듭니다.")
                                      : tr("아래 백업을 복원하면 이 계정에 세이브를 새로 만듭니다."),
                        x + 62, y + 37, 16, Secondary, false, w - 82);
    } else {
      paint_.fonts.text(tr("지금 세이브를 백업합니다"), x + 62, y + 8, 21, Ink, true);
      paint_.fonts.text(tr("SD 카드의 oc-save-keeper 폴더에 저장해 두 앱이 함께 씁니다."), x + 62, y + 37, 16, Secondary,
                        false, w - 82);
    }
  }
  y += Row + 16;
  paint_.fonts.text(backups_.empty() ? std::string(tr("아직 백업이 없습니다.")) : trf("백업 {0}개", {std::to_string(backups_.size())}),
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
      paint_.fonts.text(tr("세이브 삭제"), x + 62, y + 8, 21, Bad, true);
      paint_.fonts.text(save.device() ? tr("모든 계정이 함께 쓰는 플레이 기록을 삭제합니다.") : tr("이 계정의 플레이 기록을 삭제합니다."),
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
  std::string text = !restore     ? std::string(tr("oc-save-keeper의 휴지통으로 옮기며, 그 앱에서 되살릴 수 있습니다."))
                     : save.missing ? trf("{0}에 세이브를 새로 만들고 이 백업을 넣습니다.", {save.userName})
                                    : trf("{0}의 지금 세이브를 이 백업으로 바꿉니다. 지금 세이브는 먼저 자동으로 백업합니다.", {save.userName});
  if (restore && b.hasMeta && !b.meta.userId.empty() && b.meta.userId != saves::userIdOf(save.user))
    text += " " + trf("{0} 계정에서 만든 백업입니다.", {b.meta.userName.empty() ? b.meta.userId : b.meta.userName});
  // A backup made on another console (its device id differs from this one's).
  if (restore && b.hasMeta && !b.meta.deviceId.empty() && !thisDevice_.empty() && b.meta.deviceId != thisDevice_)
    text += " " + trf("다른 기기({0})에서 만든 백업입니다.", {b.meta.deviceLabel.empty() ? b.meta.deviceId.substr(0, 8) : b.meta.deviceLabel});
  const auto confirm = modal(700, 20 + 30 + 84 + 16, restore ? tr("이 백업으로 복원합니까?") : tr("백업을 휴지통으로 옮깁니까?"),
                             tr("취소"), "", restore ? tr("복원합니다") : tr("옮깁니다"), restore ? 3 : 37);
  paint_.fonts.text(when(b.timestamp), confirm.x + 32, confirm.y + 20, 21, Ink, true);
  paint_.fonts.wrapped(text, confirm.x + 32, confirm.y + 56, 19, Secondary, confirm.w - 64, 3);
}
} // namespace foil::ui
