// 연결 to PC: the SD card over the USB cable (platform::usb), from the free
// space on the footer or 설정. One popup says how it stands; it closes by
// itself when the cable goes, and B cuts it at any time. USB stops and what
// a Mac left is swept on a worker, so the screen never waits on either; it
// says which of the two it is at.
#include "ui/Application.hpp"
#include "core/Forwarder.hpp"
#include "core/Log.hpp"
#include "core/MacJunk.hpp"
#include "platform/Cores.hpp"
#include "platform/Storage.hpp"
namespace foil::ui {
namespace {
constexpr int UsbIcon = 50;
// A cable that reads gone this long is gone: a PC re-enumerating the device
// drops it for a moment.
constexpr uint32_t CableGoneMs = 1500;
constexpr uint32_t ReadEveryMs = 200;
} // namespace
void Application::openUsbDrive() {
  if (usbOpen_) return;
  // An applet has too little memory to spare: said, with the way out.
  if (!platform::applicationMode()) {
    usbAppletAsk_ = true;
    return;
  }
  switch (platform::usb::start()) {
  case platform::usb::StartError::Memory:
    alertTitle_ = tr("Could not connect to the PC");
    alert_ = tr("There is not enough memory to connect. Try again once the downloads finish.");
    logLine("usb: not started, too little memory");
    return;
  case platform::usb::StartError::Usb:
    alertTitle_ = tr("Could not connect to the PC");
    alert_ = tr("USB could not be opened. Close other apps that use USB and try again.");
    logLine("usb: not started, usb:ds refused");
    return;
  case platform::usb::StartError::None: break;
  }
  usbOpen_ = true;
  usbStopping_ = false;
  usbReadAt_ = usbLostAt_ = 0;
  usbStatus_ = {};
  usbStatus_.phase = platform::usb::Phase::Waiting;
  logLine("usb: open, waiting for a PC");
}
void Application::closeUsbDrive(bool lost) {
  if (!usbOpen_ || usbStopping_) return;
  usbStopping_ = true;
  usbLost_ = lost;
  usbStopped_ = false;
  if (usbWorker_.joinable()) usbWorker_.join();
  usbWorker_ = std::thread([this, sd = sdRoot_] {
    const platform::OnCore off(platform::WriterCore);
    guarded("usb close", [&] { releaseUsbDrive(sd); });
    usbStopped_ = true;
  });
}
void Application::releaseUsbDrive(const std::string &sd) {
  usbSwept_ = usbDropped_ = 0;
  const auto touched = platform::usb::status().touched;
  // A copy cut by the close is deleted as USB stops.
  platform::usb::stop();
  usbDropped_ = platform::usb::status().dropped;
  usbCleaning_ = true;
  const auto sweep = sweepMacJunk(sd, touched);
  usbSwept_ = sweep.removed;
  logLine("usb: closed; " + std::to_string(touched.size()) + " folders written, " + std::to_string(usbDropped_) +
          " unfinished copies removed, " + std::to_string(sweep.removed) + " Mac files removed, " +
          std::to_string(sweep.failed) + " not");
}
void Application::pollUsbDrive() {
  if (usbStopping_ && usbStopped_.exchange(false)) {
    usbWorker_.join();
    usbOpen_ = usbStopping_ = usbCleaning_ = false;
    cardChanged(); // the PC may have changed anything on the card
    std::string said = usbLost_ ? tr("The cable came out, so the connection closed.") : tr("Disconnected from the PC.");
    if (usbDropped_ > 0) said += " " + trf("Removed {0} unfinished copies.", {std::to_string(usbDropped_)});
    if (usbSwept_ > 0) said += " " + trf("Removed {0} files a Mac left.", {std::to_string(usbSwept_)});
    message(said);
    return;
  }
  if (!usbOpen_ || usbStopping_) return;
  const uint32_t now = SDL_GetTicks();
  if (usbReadAt_ && now - usbReadAt_ < ReadEveryMs) return;
  usbReadAt_ = std::max<uint32_t>(1, now);
  usbStatus_ = platform::usb::status();
  using Phase = platform::usb::Phase;
  if (usbStatus_.phase == Phase::Failed) {
    closeUsbDrive(false);
    alertTitle_ = tr("Could not connect to the PC");
    alert_ = tr("USB could not be opened. Close other apps that use USB and try again.");
    return;
  }
  if (usbStatus_.phase == Phase::Plugged || usbStatus_.phase == Phase::Open) {
    usbLostAt_ = 0;
    return;
  }
  if (!usbStatus_.cableSeen) return; // still waiting for the first PC
  if (!usbLostAt_) usbLostAt_ = usbReadAt_;
  else if (now - usbLostAt_ >= CableGoneMs) closeUsbDrive(true);
}
void Application::usbDriveDialog() {
  using Phase = platform::usb::Phase;
  constexpr float Width = 720, Tile = 72, TextX = 32 + Tile + 24, Text = Width - TextX - 32;
  const auto &st = usbStatus_;
  const bool linked = !usbStopping_ && (st.phase == Phase::Plugged || st.phase == Phase::Open);
  const bool copying = !usbStopping_ && st.phase == Phase::Open && !st.file.empty();
  const std::string headline = usbCleaning_                ? tr("Cleaning up files")
                               : usbStopping_              ? tr("Disconnecting")
                               : copying && st.toConsole   ? tr("Copying to the console")
                               : copying                   ? tr("Copying to the PC")
                               : st.phase == Phase::Open   ? tr("The PC has the SD card open.")
                               : st.phase == Phase::Plugged ? tr("Connected to the PC.")
                                                           : tr("Connect the console to a PC with a USB cable.");
  const std::string detail = usbStopping_    ? std::string()
                             : copying       ? st.file + " · " + formatBytes(uint64_t(std::max<int64_t>(0, st.moved)))
                             : st.copied > 0 ? trf("{0} files copied", {std::to_string(st.copied)})
                                             : std::string();
  // What the PC gets, then why two folders are missing, each on its own line;
  // closing, what the sweep does instead.
  const std::string note = usbCleaning_   ? tr("Deleting unfinished copies and files a Mac left.")
                           : usbStopping_ ? std::string()
                                          : tr("You can browse and change the files on the SD card.");
  const std::string hidden = usbStopping_ ? std::string() : tr("Sensitive folders such as Nintendo and emuMMC cannot be changed.");
  const float headH = paint_.fonts.lineCount(headline, 22, Text, 2, true) * 22 * 1.3f;
  const float detailH = detail.empty() ? 0 : 8 + 18 * 1.3f;
  const auto lines = [&](const std::string &text) { return text.empty() ? 0 : paint_.fonts.lineCount(text, 17, Text, 2); };
  const float noteH = note.empty() ? 0 : 14 + (lines(note) + lines(hidden)) * 17 * 1.3f + 4;
  const float bodyH = std::max(Tile + 48, 24 + headH + detailH + noteH + 24);
  const auto body = modal(Width, bodyH, tr("Connect to PC via MTP"), usbStopping_ ? "" : tr("Disconnect|usb"), "", NoAccept, UsbIcon);
  // The cable's mark on a round tile: green once a PC holds it; while it
  // waits or closes, the tile breathes so the screen reads as busy, not stuck.
  const float tileX = body.x + 32, tileY = body.y + (bodyH - Tile) / 2;
  SDL_Color tile = linked ? GreenTint : Ground;
  if (!linked) {
    const float wave = 0.5f + 0.5f * std::sin(float(SDL_GetTicks() % 1600) / 1600.f * 6.2832f);
    tile.a = Uint8(140 + 115 * wave);
  }
  paint_.rect(tileX, tileY, Tile, Tile, tile, Tile / 2);
  paint_.icon(UsbIcon, tileX + (Tile - 40) / 2, tileY + (Tile - 40) / 2, linked ? Good : usbStopping_ ? Secondary : Ink, 40);
  float y = body.y + (bodyH - (headH + detailH + noteH)) / 2;
  paint_.fonts.wrapped(headline, body.x + TextX, y, 22, Ink, Text, 2, true);
  y += headH;
  if (!detail.empty()) {
    paint_.fonts.text(detail, body.x + TextX, y + 8, 18, Secondary, false, Text);
    y += detailH;
  }
  if (note.empty()) return;
  paint_.fonts.wrapped(note, body.x + TextX, y + 14, 17, Secondary, Text, 2);
  y += lines(note) * 17 * 1.3f + 4;
  if (!hidden.empty()) paint_.fonts.wrapped(hidden, body.x + TextX, y + 14, 17, Secondary, Text, 2);
}
void Application::usbDriveAction(Action a) {
  if (a == Action::Back) closeUsbDrive(false);
}
void Application::usbAppletDialog() {
  constexpr float Width = 760, Text = Width - 64;
  const bool canMake = selfShortcutPossible();
  const auto why = tr("Opened from the Album there is too little memory to connect to a PC. Opened from a HOME Menu shortcut, it can.");
  const float whyH = paint_.fonts.lineCount(why, 19, Text, 3) * 19 * 1.3f;
  const auto body = modal(Width, 24 + whyH + 24, tr("Running in applet mode"), tr("Close"), "",
                          canMake ? std::string(tr("Make shortcut")) : std::string(NoAccept), UsbIcon);
  paint_.fonts.wrapped(why, body.x + 32, body.y + 24, 19, Ink, Text, 3);
}
void Application::usbAppletAction(Action a) {
  if (a != Action::Back && !(a == Action::Accept && settled())) return;
  usbAppletAsk_ = false;
  if (a == Action::Accept && selfShortcutPossible()) makeSelfShortcut();
}
} // namespace foil::ui
