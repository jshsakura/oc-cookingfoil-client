#pragma once
// The SD card to a PC as an MTP device (Windows Explorer, OpenMTP on a Mac),
// written from the USB-IF MTP 1.1 and PTP (ISO 15740) specifications. The
// protocol lives here, apart from USB and the card, so the host tests drive
// it end to end; platform/UsbDrive.cpp gives it the Switch's USB and card.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace foil::mtp {
// The bulk endpoints. Each transfer is started, then finished; one of each
// direction may be in flight, which is what lets a copy read the card while
// the cable carries the previous block.
struct Pipe {
  virtual ~Pipe() = default;
  // Into `buf`, up to `size` bytes; it ends early at the PC's short packet.
  virtual bool startReceive(void *buf, size_t size) = 0;
  // The bytes that came, or -1 when the cable is gone.
  virtual long finishReceive() = 0;
  // `last`: the transfer's final block, closed with a zero-length packet when
  // it ends on a packet boundary.
  virtual bool startSend(const void *buf, size_t size, bool last) = 0;
  virtual bool finishSend() = 0;
};
struct Entry {
  std::string name;
  bool folder = false;
  uint64_t size = 0;
};
struct File {
  virtual ~File() = default;
  virtual long read(uint64_t offset, void *buf, size_t size) = 0; // -1 on failure
  virtual bool write(uint64_t offset, const void *buf, size_t size) = 0;
};
// The card, by paths from its top ("/switch/app.nro").
struct Storage {
  virtual ~Storage() = default;
  virtual bool list(const std::string &folder, std::vector<Entry> &out) = 0;
  virtual uint64_t totalSpace() = 0;
  virtual uint64_t freeSpace() = 0;
  // `size` is what is coming, UINT64_MAX when the PC did not say.
  virtual bool createFile(const std::string &path, uint64_t size) = 0;
  virtual bool createFolder(const std::string &path) = 0;
  virtual bool remove(const std::string &path, bool folder) = 0;
  virtual bool rename(const std::string &from, const std::string &to, bool folder) = 0;
  virtual std::unique_ptr<File> open(const std::string &path, bool write) = 0;
};
// What the screen shows, told as it happens (on the USB thread).
struct Listener {
  virtual ~Listener() = default;
  virtual void session(bool open) = 0;
  virtual void copyBegins(const std::string &name, bool toConsole) = 0;
  virtual void copyMoved(uint64_t bytes) = 0;
  virtual void copyEnds() = 0;
  // A folder the PC wrote in.
  virtual void touched(const std::string &folder) = 0;
};
// One of the drives the PC sees: the whole card, or one folder of it.
struct Volume {
  std::string name;   // as the PC shows it
  std::string folder; // on the card, "" for its top
  // False: files can be copied off and deleted, nothing added or renamed.
  bool adds = true;
  // Top folders the PC never sees or changes ("Nintendo").
  std::vector<std::string> guarded;
};
// The console's Nintendo folder on the card, where its album is: the card's
// own, or for an emuMMC boot the one emuMMC/emummc.ini names (`ini`, its text).
std::string nintendoFolder(bool emummc, const std::string &ini);
// The files a session has seen, by handle. Only folders the PC opened are
// read, and each file costs its name and a few numbers; a closed session
// gives it all back.
class Objects {
public:
  struct Object {
    std::string name; // empty once removed
    uint32_t parent = 0;
    uint64_t size = 0;
    bool folder = false;
    bool listed = false;
    std::vector<uint32_t> children;
  };
  // Handles below `roots` are the volumes' tops, never given to the PC.
  explicit Objects(uint32_t roots = 1) : roots_(roots) { clear(); }
  void clear();
  bool isRoot(uint32_t handle) const { return handle < roots_; }
  const Object *find(uint32_t handle) const;
  // A volume's top as well (find() leaves it out: it is not an object).
  const Object &folder(uint32_t handle) const { return table_[handle]; }
  uint32_t rootOf(uint32_t handle) const;
  // From its volume's top ("/2024/01/x.jpg").
  std::string path(uint32_t handle) const;
  // A folder's listing, as the card has it now: the handles of what stays
  // are kept, what left is removed.
  const std::vector<uint32_t> &sync(uint32_t folder, std::vector<Entry> listing);
  uint32_t add(uint32_t folder, Entry entry);
  void remove(uint32_t handle);
  void rename(uint32_t handle, std::string name);
  void resize(uint32_t handle, uint64_t size);
  // The child of `folder` named `name` (any case), 0 when there is none.
  uint32_t child(uint32_t folder, const std::string &name) const;

private:
  uint32_t roots_;
  std::vector<Object> table_; // the handle's index
  void unlink(uint32_t handle);
};
class Responder {
public:
  // `chunk`: the size of each of the two page-aligned buffers in `buffers`.
  Responder(Pipe &pipe, Storage &storage, Listener &listener, uint8_t *buffers[2], size_t chunk,
            std::vector<Volume> volumes);
  // One command, answered: false once the cable is gone.
  bool serve();
  // The cable left: the session and its handles go.
  void reset();

private:
  struct Command {
    uint16_t code = 0;
    uint32_t transaction = 0;
    uint32_t params[5] = {};
  };
  struct Pending {
    uint32_t handle = 0;
    uint64_t size = 0;
  };
  Pipe &pipe_;
  Storage &storage_;
  Listener &listener_;
  uint8_t *buffers_[2];
  size_t chunk_;
  std::vector<Volume> volumes_;
  Objects objects_;
  bool session_ = false;
  Pending pending_; // SendObjectInfo's file, until SendObject fills it

  // A path from volume `volume`'s top in a folder it hides.
  bool guarded(uint32_t volume, const std::string &path) const;
  // Where a handle is on the card.
  std::string onCard(uint32_t handle) const;
  // The volume a StorageID names.
  bool volumeOf(uint32_t storage, uint32_t &volume) const;
  bool sendData(const Command &command, const std::vector<uint8_t> &payload);
  bool respond(const Command &command, uint16_t code, std::vector<uint32_t> params = {});
  // The PC's data phase in memory (small datasets only).
  bool receiveData(std::vector<uint8_t> &out);
  // A folder's children as the card has them now; null when it cannot be read.
  const std::vector<uint32_t> *listFolder(uint32_t folder);
  // A handle the PC named as a folder (0 and 0xFFFFFFFF: `storage`'s top).
  bool folderOf(uint32_t storage, uint32_t param, uint32_t &folder) const;
  uint16_t createObject(uint32_t folder, Entry entry, uint32_t &handle);
  void dropPending();

  bool deviceInfo(const Command &c);
  bool storageInfo(const Command &c);
  bool objectHandles(const Command &c);
  bool objectInfo(const Command &c);
  bool getObject(const Command &c);
  bool sendObjectInfo(const Command &c);
  bool sendObjectPropList(const Command &c);
  bool sendObject(const Command &c);
  bool deleteObject(const Command &c);
  bool propsSupported(const Command &c);
  bool propDesc(const Command &c);
  bool propValue(const Command &c);
  bool setPropValue(const Command &c);
  bool propList(const Command &c);
};
} // namespace foil::mtp
