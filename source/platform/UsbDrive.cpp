#include "platform/UsbDrive.hpp"
#include <algorithm>
#include <mutex>
#include <set>
#ifdef __SWITCH__
#include "mtp/Mtp.hpp"
#include <atomic>
#include <cstdlib>
#include <memory>
#include <switch.h>
#include <utility>
#endif
namespace foil::platform::usb {
namespace {
std::mutex guard;
Status shown;
#ifdef __SWITCH__
bool running = false, ready = false, sessionOpen = false;
std::set<std::string> touched;
constexpr size_t TouchedMax = 512; // past it, only the card's top is swept
// Two buffers, one on the cable while the card reads or writes the other.
constexpr size_t Chunk = 4 << 20;
constexpr u16 Vendor = 0x057e, Product = 0x201d;
// The card's top folders a PC never sees: the installed games' own data and
// an emuMMC. Deleting either from a file manager loses what no copy brings
// back, so they are left out of every listing and refused by path.
const std::vector<std::string> Guarded = {"Nintendo", "emuMMC"};
std::atomic<bool> stopping{false};
UsbDsEndpoint *bulkIn = nullptr, *bulkOut = nullptr;
bool configured() {
  UsbState state = UsbState_Detached;
  return R_SUCCEEDED(usbDsGetState(&state)) && state == UsbState_Configured;
}
// A transfer's end, waited for in short steps so stop() and a pulled cable
// are seen: -1 then, the transfer cancelled.
long finish(UsbDsEndpoint *endpoint, u32 urb) {
  while (R_FAILED(eventWait(&endpoint->CompletionEvent, 100'000'000))) {
    if (!stopping && configured()) continue;
    usbDsEndpoint_Cancel(endpoint);
    eventWait(&endpoint->CompletionEvent, 1'000'000'000);
    eventClear(&endpoint->CompletionEvent);
    return -1;
  }
  eventClear(&endpoint->CompletionEvent);
  UsbDsReportData report;
  u32 requested = 0, transferred = 0;
  if (R_FAILED(usbDsEndpoint_GetReportData(endpoint, &report)) ||
      R_FAILED(usbDsParseReportData(&report, urb, &requested, &transferred)))
    return -1;
  return long(transferred);
}
struct UsbPipe final : mtp::Pipe {
  u32 inUrb = 0, outUrb = 0;
  void *into = nullptr;
  bool zlt = false;
  bool startReceive(void *buf, size_t size) override {
    into = buf;
    armDCacheFlush(buf, size);
    return R_SUCCEEDED(usbDsEndpoint_PostBufferAsync(bulkOut, buf, size, &outUrb));
  }
  long finishReceive() override {
    const long n = finish(bulkOut, outUrb);
    if (n > 0) armDCacheFlush(into, size_t(n));
    return n;
  }
  bool startSend(const void *buf, size_t size, bool last) override {
    // Only a transfer's final block closes with a zero-length packet (when
    // it ends on a packet); the full blocks before it must not.
    if (last != zlt && R_FAILED(usbDsEndpoint_SetZlt(bulkIn, last))) return false;
    zlt = last;
    armDCacheFlush(const_cast<void *>(buf), size);
    return R_SUCCEEDED(usbDsEndpoint_PostBufferAsync(bulkIn, const_cast<void *>(buf), size, &inUrb));
  }
  bool finishSend() override { return finish(bulkIn, inUrb) >= 0; }
};
struct SdFile final : mtp::File {
  FsFile file{};
  bool writing = false;
  ~SdFile() override {
    if (writing) fsFileFlush(&file);
    fsFileClose(&file);
  }
  long read(uint64_t offset, void *buf, size_t size) override {
    u64 got = 0;
    return R_SUCCEEDED(fsFileRead(&file, s64(offset), buf, size, FsReadOption_None, &got)) ? long(got) : -1;
  }
  bool write(uint64_t offset, const void *buf, size_t size) override {
    return R_SUCCEEDED(fsFileWrite(&file, s64(offset), buf, size, FsWriteOption_None));
  }
};
struct SdCard final : mtp::Storage {
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  std::vector<FsDirectoryEntry> batch = std::vector<FsDirectoryEntry>(64);
  bool list(const std::string &folder, std::vector<mtp::Entry> &out) override {
    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(fs, folder.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir))) return false;
    s64 count = 0;
    Result rc;
    while (R_SUCCEEDED(rc = fsDirRead(&dir, &count, batch.size(), batch.data())) && count > 0)
      for (s64 i = 0; i < count; ++i)
        out.push_back({batch[i].name, batch[i].type == FsDirEntryType_Dir, uint64_t(std::max<s64>(0, batch[i].file_size))});
    fsDirClose(&dir);
    return R_SUCCEEDED(rc);
  }
  uint64_t totalSpace() override {
    s64 n = 0;
    return R_SUCCEEDED(fsFsGetTotalSpace(fs, "/", &n)) ? uint64_t(n) : 0;
  }
  uint64_t freeSpace() override {
    s64 n = 0;
    return R_SUCCEEDED(fsFsGetFreeSpace(fs, "/", &n)) ? uint64_t(n) : 0;
  }
  bool createFile(const std::string &path, uint64_t size) override {
    const s64 bytes = size == UINT64_MAX ? 0 : s64(size);
    if (R_SUCCEEDED(fsFsCreateFile(fs, path.c_str(), bytes, 0))) return true;
    // A FAT32 card holds 4 GB and up only as the console's split file.
    return size != UINT64_MAX && size >= (4ull << 30) &&
           R_SUCCEEDED(fsFsCreateFile(fs, path.c_str(), bytes, FsCreateOption_BigFile));
  }
  bool createFolder(const std::string &path) override { return R_SUCCEEDED(fsFsCreateDirectory(fs, path.c_str())); }
  bool remove(const std::string &path, bool folder) override {
    return R_SUCCEEDED(folder ? fsFsDeleteDirectoryRecursively(fs, path.c_str()) : fsFsDeleteFile(fs, path.c_str()));
  }
  bool rename(const std::string &from, const std::string &to, bool folder) override {
    return R_SUCCEEDED(folder ? fsFsRenameDirectory(fs, from.c_str(), to.c_str()) : fsFsRenameFile(fs, from.c_str(), to.c_str()));
  }
  std::unique_ptr<mtp::File> open(const std::string &path, bool write) override {
    auto out = std::make_unique<SdFile>();
    const u32 mode = write ? FsOpenMode_Write | FsOpenMode_Append : FsOpenMode_Read;
    if (R_FAILED(fsFsOpenFile(fs, path.c_str(), mode, &out->file))) return nullptr;
    out->writing = write;
    return out;
  }
};
struct Shown final : mtp::Listener {
  void session(bool open) override {
    std::lock_guard lock(guard);
    sessionOpen = open;
  }
  void copyBegins(const std::string &name, bool toConsole) override {
    std::lock_guard lock(guard);
    shown.file = name;
    shown.toConsole = toConsole;
    shown.moved = 0;
  }
  void copyMoved(uint64_t bytes) override {
    std::lock_guard lock(guard);
    shown.moved = int64_t(bytes);
  }
  void copyEnds() override {
    std::lock_guard lock(guard);
    shown.file.clear();
    ++shown.copied;
  }
  void touched(const std::string &folder) override {
    std::lock_guard lock(guard);
    if (usb::touched.size() < TouchedMax) usb::touched.insert(folder);
  }
};
struct Session {
  UsbPipe pipe;
  SdCard card;
  Shown listener;
  uint8_t *buffers[2];
  mtp::Responder responder;
  Session(uint8_t *a, uint8_t *b) : buffers{a, b}, responder(pipe, card, listener, buffers, Chunk, Guarded) {}
};
Thread worker;
uint8_t *buffers[2] = {};
std::unique_ptr<Session> session;
// The USB thread: a PC's commands, answered until stop(); a pulled cable
// ends the session and the next PC starts over.
void serve(void *) {
  while (!stopping) {
    if (!configured()) {
      svcSleepThread(100'000'000);
      continue;
    }
    while (!stopping && session->responder.serve()) {
    }
    session->responder.reset();
  }
}
bool setUpUsb() {
  if (R_FAILED(usbDsInitialize())) return false;
  static const u16 English = 0x0409;
  u8 manufacturer = 0, product = 0, serial = 0;
  usb_device_descriptor device{USB_DT_DEVICE_SIZE, USB_DT_DEVICE, 0x0110, 0, 0, 0, 0x40, Vendor, Product, 0x0100, 0, 0, 0, 1};
  // USB 2.0 LPM and USB 3 SuperSpeed, so a USB 3 port runs at its speed.
  static const u8 bos[] = {0x05, USB_DT_BOS, 0x16, 0x00, 0x02,
                           0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,
                           0x0A, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00};
  bool ok = R_SUCCEEDED(usbDsClearDeviceData()) && R_SUCCEEDED(usbDsAddUsbLanguageStringDescriptor(nullptr, &English, 1)) &&
            R_SUCCEEDED(usbDsAddUsbStringDescriptor(&manufacturer, "Nintendo")) &&
            R_SUCCEEDED(usbDsAddUsbStringDescriptor(&product, "Nintendo Switch")) &&
            R_SUCCEEDED(usbDsAddUsbStringDescriptor(&serial, "CookingFoil"));
  device.iManufacturer = manufacturer, device.iProduct = product, device.iSerialNumber = serial;
  ok = ok && R_SUCCEEDED(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device));
  device.bcdUSB = 0x0200;
  ok = ok && R_SUCCEEDED(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device));
  device.bcdUSB = 0x0300, device.bMaxPacketSize0 = 0x09;
  ok = ok && R_SUCCEEDED(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device)) &&
       R_SUCCEEDED(usbDsSetBinaryObjectStore(bos, sizeof(bos)));
  UsbDsInterface *usbInterface = nullptr;
  ok = ok && R_SUCCEEDED(usbDsRegisterInterface(&usbInterface));
  if (!ok) return usbDsExit(), false;
  // A still-image interface with the PTP protocol: what Windows, macOS and
  // Linux take as an MTP device.
  const usb_interface_descriptor face{USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE, usbInterface->interface_index, 0, 3,
                                      USB_CLASS_IMAGE, 1, 1, 0};
  usb_endpoint_descriptor in{USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, u8(USB_ENDPOINT_IN | (usbInterface->interface_index + 1)),
                             USB_TRANSFER_TYPE_BULK, 0, 0};
  usb_endpoint_descriptor out{USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, u8(USB_ENDPOINT_OUT | (usbInterface->interface_index + 1)),
                              USB_TRANSFER_TYPE_BULK, 0, 0};
  usb_endpoint_descriptor events{USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, u8(USB_ENDPOINT_IN | (usbInterface->interface_index + 2)),
                                 USB_TRANSFER_TYPE_INTERRUPT, 0x40, 6};
  usb_ss_endpoint_companion_descriptor companion{USB_DT_SS_ENDPOINT_COMPANION_SIZE, USB_DT_SS_ENDPOINT_COMPANION, 0x0F, 0, 0};
  usb_ss_endpoint_companion_descriptor eventsCompanion{USB_DT_SS_ENDPOINT_COMPANION_SIZE, USB_DT_SS_ENDPOINT_COMPANION, 0, 0, 0x40};
  const auto append = [&](UsbDeviceSpeed speed, const void *d, size_t n) {
    ok = ok && R_SUCCEEDED(usbDsInterface_AppendConfigurationData(usbInterface, speed, d, n));
  };
  for (const auto [speed, packet] : {std::pair{UsbDeviceSpeed_Full, 64}, {UsbDeviceSpeed_High, 512}, {UsbDeviceSpeed_Super, 1024}}) {
    const bool super = speed == UsbDeviceSpeed_Super;
    in.wMaxPacketSize = out.wMaxPacketSize = u16(packet);
    append(speed, &face, USB_DT_INTERFACE_SIZE);
    append(speed, &in, USB_DT_ENDPOINT_SIZE);
    if (super) append(speed, &companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    append(speed, &out, USB_DT_ENDPOINT_SIZE);
    if (super) append(speed, &companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    append(speed, &events, USB_DT_ENDPOINT_SIZE);
    if (super) append(speed, &eventsCompanion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
  }
  UsbDsEndpoint *eventsEndpoint = nullptr;
  ok = ok && R_SUCCEEDED(usbDsInterface_RegisterEndpoint(usbInterface, &bulkIn, in.bEndpointAddress)) &&
       R_SUCCEEDED(usbDsInterface_RegisterEndpoint(usbInterface, &bulkOut, out.bEndpointAddress)) &&
       R_SUCCEEDED(usbDsInterface_RegisterEndpoint(usbInterface, &eventsEndpoint, events.bEndpointAddress)) &&
       R_SUCCEEDED(usbDsInterface_EnableInterface(usbInterface)) && R_SUCCEEDED(usbDsEnable());
  if (!ok) return usbDsExit(), false;
  return true;
}
void freeBuffers() {
  for (auto &b : buffers) std::free(std::exchange(b, nullptr));
}
#endif
} // namespace
uint64_t neededMemory() {
  // 8 MiB of buffers, the session's file list (names only, a few MiB for a
  // card of many thousands), and room for the app to keep drawing.
  return uint64_t(24) << 20;
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
    ready = sessionOpen = false;
  }
  // Page-aligned, as usb:ds asks of what it moves.
  for (auto &b : buffers) b = static_cast<uint8_t *>(std::aligned_alloc(0x1000, Chunk));
  if (!buffers[0] || !buffers[1]) return freeBuffers(), StartError::Memory;
  if (!setUpUsb()) return freeBuffers(), StartError::Usb;
  stopping = false;
  session = std::make_unique<Session>(buffers[0], buffers[1]);
  // Below the drawing thread's priority, on the core downloads do not take.
  if (R_FAILED(threadCreate(&worker, serve, nullptr, nullptr, 0x20000, 0x2D, 2)) || R_FAILED(threadStart(&worker))) {
    threadClose(&worker);
    session.reset();
    usbDsExit();
    return freeBuffers(), StartError::Usb;
  }
  std::lock_guard lock(guard);
  running = ready = true;
  return StartError::None;
}
void stop() {
  {
    std::lock_guard lock(guard);
    if (!running) return;
    // From here status() leaves usb:ds alone, before it closes.
    running = ready = false;
  }
  stopping = true;
  threadWaitForExit(&worker);
  threadClose(&worker);
  session.reset();
  usbDsExit();
  bulkIn = bulkOut = nullptr;
  freeBuffers();
  std::lock_guard lock(guard);
  sessionOpen = false;
  shown.file.clear();
}
Status status() {
  std::lock_guard lock(guard);
  Status out = shown;
  out.touched.assign(touched.begin(), touched.end());
  if (!running) return out.phase = Phase::Off, out;
  const bool cable = ready && configured();
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
