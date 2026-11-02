#pragma once
// The SD card to a PC over the USB cable, as an MTP device (mtp/Mtp.hpp): open
// in Explorer on Windows, in OpenMTP on a Mac. Only the card is offered,
// never the system's own storage. Nothing is held before start() or after
// stop(): the transfer buffers (8 MiB) come with start(), the file names of
// the folders a PC opened only while it has the card open.
// On the host, preview() walks the screen through each state instead.
#include <cstdint>
#include <string>
#include <vector>
namespace foil::platform::usb {
enum class Phase {
  Off,
  Waiting,   // no PC on the cable yet
  Plugged,   // a PC took the cable, nothing has opened the card yet
  Open,      // a PC has the card open
  Failed,    // USB could not be set up; stop() then
};
struct Status {
  Phase phase = Phase::Off;
  std::string file; // being copied now (its name only), empty between files
  bool toConsole = false; // that copy goes from the PC to the card
  int64_t moved = 0;      // of that file so far
  int copied = 0;         // files copied this time, either way
  int dropped = 0;        // copies that did not finish, deleted from the card
  // The folders (paths on the card) the PC wrote to, for sweepMacJunk.
  std::vector<std::string> touched;
  bool cableSeen = false; // the PC took the cable at some point
};
enum class StartError { None, Memory, Usb };
// The free memory a session needs: buffers, the file list and the two
// transfer threads' blocks, with room to spare.
uint64_t neededMemory();
StartError start();
// Waits for the USB thread to end (a copy in flight is cut): call it off the
// drawing thread. Safe to call when not started.
void stop();
Status status();
#ifndef __SWITCH__
void preview(Status status);
#endif
} // namespace foil::platform::usb
