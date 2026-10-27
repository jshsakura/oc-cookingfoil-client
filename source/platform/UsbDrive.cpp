#include "platform/UsbDrive.hpp"
#include <algorithm>
#include <mutex>
#include <set>
#ifdef __SWITCH__
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <haze.h>
#include <map>
#include <strings.h>
#include <switch.h>
#endif
namespace foil::platform::usb {
namespace {
std::mutex guard;
Status shown;
#ifdef __SWITCH__
bool running = false, ready = false, failed = false, sessionOpen = false;
std::set<std::string> touched;
constexpr size_t TouchedMax = 512; // past it, only the card's top is swept
// The folder a path from the PC is in ("/switch/app/x.nro" -> "/switch/app").
std::string folderOf(const char *path) {
  const char *slash = std::strrchr(path, '/');
  return slash && slash != path ? std::string(path, slash) : std::string("/");
}
const char *nameOf(const char *path) {
  const char *slash = std::strrchr(path, '/');
  return slash ? slash + 1 : path;
}
void noteTouched(const char *path) {
  if (touched.size() < TouchedMax) touched.insert(folderOf(path));
}
// Called on libhaze's thread: only notes what happened.
void onEvent(const haze::CallbackData *data) {
  std::lock_guard lock(guard);
  switch (data->type) {
  case haze::CallbackType_Ready: ready = true; break;
  case haze::CallbackType_Failed: failed = true; break;
  case haze::CallbackType_OpenSession: sessionOpen = true; break;
  case haze::CallbackType_CloseSession: sessionOpen = false; break;
  case haze::CallbackType_CreateFile:
  case haze::CallbackType_CreateFolder:
  case haze::CallbackType_DeleteFile:
  case haze::CallbackType_DeleteFolder: noteTouched(data->file.filename); break;
  case haze::CallbackType_RenameFile:
  case haze::CallbackType_RenameFolder: noteTouched(data->rename.newname); break;
  case haze::CallbackType_ReadBegin:
  case haze::CallbackType_WriteBegin:
    shown.file = nameOf(data->file.filename);
    shown.toConsole = data->type == haze::CallbackType_WriteBegin;
    shown.moved = 0;
    if (shown.toConsole) noteTouched(data->file.filename);
    break;
  case haze::CallbackType_ReadProgress:
  case haze::CallbackType_WriteProgress: shown.moved = data->progress.offset + data->progress.size; break;
  case haze::CallbackType_ReadEnd:
  case haze::CallbackType_WriteEnd:
    shown.file.clear();
    ++shown.copied;
    break;
  }
}
// The card's top folders a PC never sees: the installed games' own data and
// an emuMMC. Deleting either from a file manager loses what no copy brings
// back, so they are left out of every listing and refused by path.
bool hidden(const char *path) {
  while (*path == '/') ++path;
  for (const char *top : {"Nintendo", "emuMMC"}) {
    const size_t n = std::strlen(top);
    if (!strncasecmp(path, top, n) && (path[n] == '\0' || path[n] == '/')) return true;
  }
  return false;
}
const Result Refused = MAKERESULT(Module_Libnx, LibnxError_NotFound);
// The SD card as libhaze sees it, over the card's own file system.
struct SdCard final : haze::FileSystemProxyImpl {
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  std::mutex dirsGuard;
  std::set<FsDir *> topDirs; // listings of the card's top, filtered
  const char *GetName() const override { return ""; }
  const char *GetDisplayName() const override { return "microSD Card"; }
  Result GetTotalSpace(const char *, s64 *out) override { return fsFsGetTotalSpace(fs, "/", out); }
  Result GetFreeSpace(const char *, s64 *out) override { return fsFsGetFreeSpace(fs, "/", out); }
  Result GetEntryType(const char *path, FsDirEntryType *out) override {
    return hidden(path) ? Refused : fsFsGetEntryType(fs, path, out);
  }
  Result CreateFile(const char *path, s64 size, u32 option) override {
    return hidden(path) ? Refused : fsFsCreateFile(fs, path, size, option);
  }
  Result DeleteFile(const char *path) override { return hidden(path) ? Refused : fsFsDeleteFile(fs, path); }
  Result RenameFile(const char *from, const char *to) override {
    return hidden(from) || hidden(to) ? Refused : fsFsRenameFile(fs, from, to);
  }
  Result OpenFile(const char *path, u32 mode, FsFile *out) override {
    return hidden(path) ? Refused : fsFsOpenFile(fs, path, mode, out);
  }
  Result GetFileSize(FsFile *file, s64 *out) override { return fsFileGetSize(file, out); }
  Result SetFileSize(FsFile *file, s64 size) override { return fsFileSetSize(file, size); }
  Result ReadFile(FsFile *file, s64 off, void *buf, u64 size, u32 option, u64 *read) override {
    return fsFileRead(file, off, buf, size, option, read);
  }
  Result WriteFile(FsFile *file, s64 off, const void *buf, u64 size, u32 option) override {
    return fsFileWrite(file, off, buf, size, option);
  }
  void CloseFile(FsFile *file) override { fsFileClose(file); }
  Result CreateDirectory(const char *path) override { return hidden(path) ? Refused : fsFsCreateDirectory(fs, path); }
  Result DeleteDirectoryRecursively(const char *path) override {
    // The card's top itself is never emptied from the PC.
    const char *rest = path;
    while (*rest == '/') ++rest;
    return hidden(path) || !*rest ? Refused : fsFsDeleteDirectoryRecursively(fs, path);
  }
  Result RenameDirectory(const char *from, const char *to) override {
    return hidden(from) || hidden(to) ? Refused : fsFsRenameDirectory(fs, from, to);
  }
  Result OpenDirectory(const char *path, u32 mode, FsDir *out) override {
    if (hidden(path)) return Refused;
    const Result rc = fsFsOpenDirectory(fs, path, mode, out);
    const char *rest = path;
    while (*rest == '/') ++rest;
    if (R_SUCCEEDED(rc) && !*rest) {
      std::lock_guard lock(dirsGuard);
      topDirs.insert(out);
    }
    return rc;
  }
  Result ReadDirectory(FsDir *dir, s64 *total, size_t max, FsDirectoryEntry *buf) override {
    bool top;
    {
      std::lock_guard lock(dirsGuard);
      top = topDirs.count(dir);
    }
    for (;;) {
      const Result rc = fsDirRead(dir, total, max, buf);
      if (R_FAILED(rc) || !top || *total <= 0) return rc;
      // Drop the hidden ones; a batch left empty reads on, since an empty
      // batch tells libhaze the listing ended.
      s64 kept = 0;
      for (s64 i = 0; i < *total; ++i)
        if (!hidden(buf[i].name)) buf[kept++] = buf[i];
      *total = kept;
      if (kept) return rc;
    }
  }
  Result GetDirectoryEntryCount(FsDir *dir, s64 *out) override { return fsDirGetEntryCount(dir, out); }
  void CloseDirectory(FsDir *dir) override {
    {
      std::lock_guard lock(dirsGuard);
      topDirs.erase(dir);
    }
    fsDirClose(dir);
  }
};
#endif
} // namespace
uint64_t neededMemory() {
  // 8 MiB of buffers, 16 MiB for the session's file list, 16 MiB for the
  // transfer threads' blocks, and room for the app to keep drawing.
  return uint64_t(64) << 20;
}
#ifdef __SWITCH__
StartError start() {
  {
    std::lock_guard lock(guard);
    if (running) return StartError::None;
  }
  // libnx hands the app its whole heap at launch, so the system's free
  // memory says nothing: whether the heap has the room is asked directly.
  void *room = std::malloc(neededMemory());
  if (!room) return StartError::Memory;
  std::free(room);
  {
    std::lock_guard lock(guard);
    shown = {};
    touched.clear();
    ready = failed = sessionOpen = false;
  }
  haze::FsEntries entries;
  entries.emplace_back(std::make_shared<SdCard>());
  // Below the drawing thread's priority, on the core downloads do not take.
  if (!haze::Initialize(onEvent, 0x2D, 2, entries)) return StartError::Usb;
  std::lock_guard lock(guard);
  running = true;
  return StartError::None;
}
void stop() {
  {
    std::lock_guard lock(guard);
    if (!running) return;
    // From here status() leaves usb:ds alone, before libhaze closes it.
    running = ready = false;
  }
  haze::Exit();
  std::lock_guard lock(guard);
  sessionOpen = false;
  shown.file.clear();
}
Status status() {
  std::lock_guard lock(guard);
  Status out = shown;
  out.touched.assign(touched.begin(), touched.end());
  if (!running) return out.phase = Phase::Off, out;
  if (failed) return out.phase = Phase::Failed, out;
  UsbState state = UsbState_Detached;
  const bool cable = ready && R_SUCCEEDED(usbDsGetState(&state)) && state == UsbState_Configured;
  if (cable) shown.cableSeen = out.cableSeen = true;
  out.phase = !cable ? Phase::Waiting : sessionOpen ? Phase::Open : Phase::Plugged;
  return out;
}
#else
StartError start() {
  std::lock_guard lock(guard);
  if (shown.phase == Phase::Off) {
    shown = {};
    shown.phase = Phase::Waiting;
  }
  return StartError::None;
}
void stop() {
  std::lock_guard lock(guard);
  shown = {};
}
Status status() {
  std::lock_guard lock(guard);
  return shown;
}
void preview(Status status) {
  std::lock_guard lock(guard);
  shown = std::move(status);
}
#endif
} // namespace foil::platform::usb
