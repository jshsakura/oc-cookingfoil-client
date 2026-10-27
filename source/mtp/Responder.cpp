#include "mtp/Mtp.hpp"
#include <algorithm>
#include <cstring>
#include <strings.h>
namespace foil::mtp {
namespace {
enum Container : uint16_t { CommandBlock = 1, DataBlock = 2, ResponseBlock = 3 };
constexpr size_t Header = 12;
constexpr uint32_t Storage = 0x00010001, Any = 0xFFFFFFFF;
// Operations (PTP 1.1 table 18, MTP 1.1 appendix D).
enum Op : uint16_t {
  GetDeviceInfo = 0x1001,
  OpenSession = 0x1002,
  CloseSession = 0x1003,
  GetStorageIDs = 0x1004,
  GetStorageInfo = 0x1005,
  GetObjectHandles = 0x1007,
  GetObjectInfo = 0x1008,
  GetObject = 0x1009,
  DeleteObject = 0x100B,
  SendObjectInfo = 0x100C,
  SendObject = 0x100D,
  GetObjectPropsSupported = 0x9801,
  GetObjectPropDesc = 0x9802,
  GetObjectPropValue = 0x9803,
  SetObjectPropValue = 0x9804,
  GetObjectPropList = 0x9805,
  SendObjectPropList = 0x9808,
};
constexpr uint16_t Operations[] = {GetDeviceInfo,     OpenSession,       CloseSession,      GetStorageIDs,
                                   GetStorageInfo,    GetObjectHandles,  GetObjectInfo,     GetObject,
                                   DeleteObject,      SendObjectInfo,    SendObject,        GetObjectPropsSupported,
                                   GetObjectPropDesc, GetObjectPropValue, SetObjectPropValue, GetObjectPropList,
                                   SendObjectPropList};
enum Code : uint16_t {
  Ok = 0x2001,
  GeneralError = 0x2002,
  SessionNotOpen = 0x2003,
  NotSupported = 0x2005,
  InvalidStorage = 0x2008,
  InvalidHandle = 0x2009,
  StoreFull = 0x200C,
  AccessDenied = 0x200F,
  NoValidObjectInfo = 0x2015,
  InvalidParent = 0x201A,
  InvalidParameter = 0x201D,
  InvalidPropCode = 0xA801,
  InvalidPropValue = 0xA803,
  InvalidDataset = 0xA806,
  GroupUnsupported = 0xA807,
  DepthUnsupported = 0xA808,
};
enum Format : uint16_t { Undefined = 0x3000, Association = 0x3001 };
enum Prop : uint16_t {
  StorageId = 0xDC01,
  ObjectFormat = 0xDC02,
  ObjectSize = 0xDC04,
  FileName = 0xDC07,
  ParentObject = 0xDC0B,
  UniqueId = 0xDC41,
};
constexpr uint16_t Props[] = {StorageId, ObjectFormat, ObjectSize, FileName, ParentObject, UniqueId};
enum Type : uint16_t { U8 = 0x0002, U16 = 0x0004, U32 = 0x0006, U64 = 0x0008, U128 = 0x000A, Str = 0xFFFF };
Type typeOf(uint16_t prop) {
  switch (prop) {
  case ObjectFormat: return U16;
  case ObjectSize: return U64;
  case FileName: return Str;
  case UniqueId: return U128;
  default: return U32;
  }
}
bool supported(uint32_t prop) { return std::find(std::begin(Props), std::end(Props), prop) != std::end(Props); }

// Little-endian datasets; PTP strings are a count byte and UTF-16 with its
// terminator.
struct Out {
  std::vector<uint8_t> bytes;
  void u8(uint8_t v) { bytes.push_back(v); }
  void u16(uint16_t v) { u8(uint8_t(v)), u8(uint8_t(v >> 8)); }
  void u32(uint32_t v) { u16(uint16_t(v)), u16(uint16_t(v >> 16)); }
  void u64(uint64_t v) { u32(uint32_t(v)), u32(uint32_t(v >> 32)); }
  void str(const std::string &text) {
    std::vector<uint16_t> units;
    for (size_t i = 0; i < text.size();) {
      const auto c = uint8_t(text[i]);
      const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
      uint32_t cp = extra ? c & (0x3F >> extra) : c;
      ++i;
      for (int k = 0; k < extra && i < text.size(); ++k, ++i) cp = cp << 6 | (uint8_t(text[i]) & 0x3F);
      if (cp >= 0x10000) {
        cp -= 0x10000;
        units.push_back(uint16_t(0xD800 | cp >> 10));
        units.push_back(uint16_t(0xDC00 | (cp & 0x3FF)));
      } else {
        units.push_back(uint16_t(cp));
      }
    }
    if (units.empty()) return u8(0);
    units.resize(std::min<size_t>(units.size(), 254));
    u8(uint8_t(units.size() + 1));
    for (const auto u : units) u16(u);
    u16(0);
  }
  template <class T> void array16(const T &values) {
    u32(uint32_t(std::size(values)));
    for (const auto v : values) u16(uint16_t(v));
  }
};
struct In {
  const uint8_t *at;
  size_t left;
  bool ok = true;
  uint64_t take(int n) {
    if (left < size_t(n)) return ok = false, left = 0, 0;
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= uint64_t(at[i]) << (8 * i);
    at += n, left -= size_t(n);
    return v;
  }
  uint16_t u16() { return uint16_t(take(2)); }
  uint32_t u32() { return uint32_t(take(4)); }
  std::string str() {
    const auto count = take(1);
    std::string out;
    for (uint64_t i = 0; i < count && ok; ++i) {
      uint32_t cp = u16();
      if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < count) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (u16() - 0xDC00u);
        ++i;
      }
      if (!cp) continue;
      if (cp < 0x80) out += char(cp);
      else if (cp < 0x800) out += char(0xC0 | cp >> 6), out += char(0x80 | (cp & 0x3F));
      else if (cp < 0x10000) out += char(0xE0 | cp >> 12), out += char(0x80 | (cp >> 6 & 0x3F)), out += char(0x80 | (cp & 0x3F));
      else
        out += char(0xF0 | cp >> 18), out += char(0x80 | (cp >> 12 & 0x3F)), out += char(0x80 | (cp >> 6 & 0x3F)),
            out += char(0x80 | (cp & 0x3F));
    }
    return out;
  }
  // A value of `type`, passed over (SendObjectPropList's other properties).
  void skip(uint16_t type) {
    if (type == Str) return void(str());
    const int width[] = {0, 1, 1, 2, 2, 4, 4, 8, 8, 16, 16};
    const uint16_t base = type & 0x0FFF;
    if (base > 10 || !width[base]) return void(ok = false);
    const uint64_t count = type & 0x4000 ? take(4) : 1;
    if (count * width[base] > left) return void(ok = false);
    at += count * width[base], left -= count * width[base];
  }
};
uint32_t parentParam(uint32_t parent) { return parent == Objects::Root ? 0 : parent; }
std::string join(const std::string &folder, const std::string &name) { return folder == "/" ? "/" + name : folder + "/" + name; }
bool validName(const std::string &name) {
  return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\") == std::string::npos;
}
} // namespace

Responder::Responder(Pipe &pipe, class Storage &storage, Listener &listener, uint8_t *buffers[2], size_t chunk,
                     std::vector<std::string> guarded)
    : pipe_(pipe), storage_(storage), listener_(listener), buffers_{buffers[0], buffers[1]}, chunk_(chunk),
      guarded_(std::move(guarded)) {}

void Responder::reset() {
  if (session_) listener_.session(false);
  session_ = false;
  pending_ = {};
  objects_.clear();
}
bool Responder::guarded(const std::string &path) const {
  const char *rest = path.c_str();
  while (*rest == '/') ++rest;
  for (const auto &top : guarded_)
    if (!strncasecmp(rest, top.c_str(), top.size()) && (rest[top.size()] == '\0' || rest[top.size()] == '/')) return true;
  return false;
}
bool Responder::sendData(const Command &command, const std::vector<uint8_t> &payload) {
  const uint64_t total = Header + payload.size();
  uint8_t *buf = buffers_[0];
  Out head;
  head.u32(uint32_t(std::min<uint64_t>(total, 0xFFFFFFFF)));
  head.u16(DataBlock);
  head.u16(command.code);
  head.u32(command.transaction);
  std::memcpy(buf, head.bytes.data(), Header);
  size_t from = 0, fill = Header;
  for (;;) {
    const size_t n = std::min(chunk_ - fill, payload.size() - from);
    if (n) std::memcpy(buf + fill, payload.data() + from, n);
    from += n;
    const bool last = from == payload.size();
    if (!pipe_.startSend(buf, fill + n, last) || !pipe_.finishSend()) return false;
    if (last) return true;
    fill = 0;
  }
}
bool Responder::respond(const Command &command, uint16_t code, std::vector<uint32_t> params) {
  Out out;
  out.u32(uint32_t(Header + 4 * params.size()));
  out.u16(ResponseBlock);
  out.u16(code);
  out.u32(command.transaction);
  for (const auto p : params) out.u32(p);
  std::memcpy(buffers_[1], out.bytes.data(), out.bytes.size());
  return pipe_.startSend(buffers_[1], out.bytes.size(), true) && pipe_.finishSend();
}
bool Responder::receiveData(std::vector<uint8_t> &out) {
  constexpr size_t Kept = 1 << 20; // a dataset; anything past it is read and dropped
  out.clear();
  uint64_t expected = 0, got = 0;
  bool first = true;
  for (;;) {
    if (!pipe_.startReceive(buffers_[0], chunk_)) return false;
    const long n = pipe_.finishReceive();
    if (n < 0) return false;
    if (first && n == 0) continue; // a stray zero-length packet
    size_t skip = 0;
    if (first) {
      if (n < long(Header)) return true;
      expected = uint32_t(buffers_[0][0] | buffers_[0][1] << 8 | buffers_[0][2] << 16 | uint32_t(buffers_[0][3]) << 24);
      skip = Header;
      first = false;
    }
    got += uint64_t(n);
    const size_t keep = std::min(size_t(n) - skip, Kept - std::min(Kept, out.size()));
    out.insert(out.end(), buffers_[0] + skip, buffers_[0] + skip + keep);
    if (size_t(n) < chunk_ || (expected != 0xFFFFFFFF && got >= expected)) return true;
  }
}
const std::vector<uint32_t> *Responder::listFolder(uint32_t folder) {
  std::vector<Entry> entries;
  if (!storage_.list(objects_.path(folder), entries)) return nullptr;
  if (folder == Objects::Root)
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry &e) { return guarded("/" + e.name); }),
                  entries.end());
  return &objects_.sync(folder, std::move(entries));
}
bool Responder::folderOf(uint32_t param, uint32_t &folder) const {
  folder = param == Any ? Objects::Root : param;
  if (folder == Objects::Root) return true;
  const auto *f = objects_.find(folder);
  return f && f->folder;
}
uint16_t Responder::createObject(uint32_t folder, Entry entry, uint32_t &handle) {
  handle = 0;
  if (!validName(entry.name)) return InvalidDataset;
  const auto folderPath = objects_.path(folder);
  const auto path = join(folderPath, entry.name);
  if (guarded(path)) return AccessDenied;
  // What is there now, so a name already taken is found.
  if (!objects_.folder(folder).listed && !listFolder(folder)) return GeneralError;
  if (const auto had = objects_.child(folder, entry.name)) {
    const bool wasFolder = objects_.find(had)->folder;
    // A folder copied onto one of its name fills it; a file replaces the
    // old one (the PC asked before sending it).
    if (wasFolder && entry.folder) return handle = had, Ok;
    if (wasFolder != entry.folder || !storage_.remove(objects_.path(had), false)) return AccessDenied;
    objects_.remove(had);
  }
  if (!entry.folder && entry.size != UINT64_MAX && entry.size > storage_.freeSpace()) return StoreFull;
  const bool made = entry.folder ? storage_.createFolder(path) : storage_.createFile(path, entry.size);
  if (!made) return entry.folder ? AccessDenied : StoreFull;
  if (entry.size == UINT64_MAX) entry.size = 0;
  handle = objects_.add(folder, std::move(entry));
  listener_.touched(folderPath);
  return Ok;
}

bool Responder::serve() {
  if (!pipe_.startReceive(buffers_[0], chunk_)) return false;
  const long n = pipe_.finishReceive();
  if (n < 0) return false;
  if (n < long(Header)) return true;
  In in{buffers_[0], size_t(n)};
  const auto length = in.u32();
  if (in.u16() != CommandBlock) return true;
  Command c;
  c.code = in.u16();
  c.transaction = in.u32();
  for (size_t i = 0; i < 5 && Header + 4 * (i + 1) <= std::min<size_t>(length, size_t(n)); ++i) c.params[i] = in.u32();
  const bool hostData = c.code == SendObjectInfo || c.code == SendObject || c.code == SetObjectPropValue ||
                        c.code == SendObjectPropList;
  if (!session_ && c.code != GetDeviceInfo && c.code != OpenSession) {
    std::vector<uint8_t> dropped;
    if (hostData && !receiveData(dropped)) return false;
    return respond(c, SessionNotOpen);
  }
  switch (c.code) {
  case GetDeviceInfo: return deviceInfo(c);
  case OpenSession:
    // A PC that opens again without closing starts over.
    if (!c.params[0]) return respond(c, InvalidParameter);
    if (session_) objects_.clear();
    session_ = true;
    listener_.session(true);
    return respond(c, Ok);
  case CloseSession: reset(); return respond(c, Ok);
  case GetStorageIDs: {
    Out out;
    out.u32(1);
    out.u32(Storage);
    return sendData(c, out.bytes) && respond(c, Ok);
  }
  case GetStorageInfo: return storageInfo(c);
  case GetObjectHandles: return objectHandles(c);
  case GetObjectInfo: return objectInfo(c);
  case GetObject: return getObject(c);
  case DeleteObject: return deleteObject(c);
  case SendObjectInfo: return sendObjectInfo(c);
  case SendObjectPropList: return sendObjectPropList(c);
  case SendObject: return sendObject(c);
  case GetObjectPropsSupported: return propsSupported(c);
  case GetObjectPropDesc: return propDesc(c);
  case GetObjectPropValue: return propValue(c);
  case SetObjectPropValue: return setPropValue(c);
  case GetObjectPropList: return propList(c);
  default: return respond(c, NotSupported);
  }
}

bool Responder::deviceInfo(const Command &c) {
  Out out;
  out.u16(100);                     // PTP 1.00
  out.u32(6);                       // the MTP vendor extension
  out.u16(100);
  out.str("microsoft.com: 1.0;");
  out.u16(0);                       // functional mode: standard
  out.array16(Operations);
  out.array16(std::vector<uint16_t>{}); // events
  out.array16(std::vector<uint16_t>{}); // device properties
  out.array16(std::vector<uint16_t>{}); // capture formats
  out.array16(std::vector<uint16_t>{Undefined, Association});
  out.str("Nintendo");
  out.str("Nintendo Switch");
  out.str("1.0");
  out.str("CookingFoil");
  return sendData(c, out.bytes) && respond(c, Ok);
}
bool Responder::storageInfo(const Command &c) {
  if (c.params[0] != Storage) return respond(c, InvalidStorage);
  Out out;
  out.u16(0x0004); // removable RAM, as a memory card reads
  out.u16(0x0002); // hierarchical
  out.u16(0x0000); // read and write
  out.u64(storage_.totalSpace());
  out.u64(storage_.freeSpace());
  out.u32(0xFFFFFFFF);
  out.str("microSD Card");
  out.str("");
  return sendData(c, out.bytes) && respond(c, Ok);
}
bool Responder::objectHandles(const Command &c) {
  if (c.params[0] != Storage && c.params[0] != Any) return respond(c, InvalidStorage);
  uint32_t folder;
  if (!folderOf(c.params[2], folder)) return respond(c, objects_.find(c.params[2]) ? InvalidParent : InvalidHandle);
  const auto *children = listFolder(folder);
  if (!children) return respond(c, GeneralError);
  const auto format = uint16_t(c.params[1]);
  Out out;
  out.u32(0);
  uint32_t count = 0;
  for (const auto h : *children) {
    const bool folderish = objects_.find(h)->folder;
    if (format && format != (folderish ? Association : Undefined)) continue;
    out.u32(h);
    ++count;
  }
  std::memcpy(out.bytes.data(), &count, 4);
  return sendData(c, out.bytes) && respond(c, Ok);
}
bool Responder::objectInfo(const Command &c) {
  const auto *o = objects_.find(c.params[0]);
  if (!o) return respond(c, InvalidHandle);
  Out out;
  out.u32(Storage);
  out.u16(o->folder ? Association : Undefined);
  out.u16(0); // no protection
  out.u32(uint32_t(std::min<uint64_t>(o->size, 0xFFFFFFFF)));
  out.u16(0); // no thumbnail: format, size, width, height
  out.u32(0);
  out.u32(0);
  out.u32(0);
  out.u32(0); // image width, height, depth
  out.u32(0);
  out.u32(0);
  out.u32(parentParam(o->parent));
  out.u16(o->folder ? 1 : 0); // a generic folder
  out.u32(0);
  out.u32(0);
  out.str(o->name);
  out.str(""); // captured, modified, keywords
  out.str("");
  out.str("");
  return sendData(c, out.bytes) && respond(c, Ok);
}
// The file to the PC, two buffers turning: one goes over the cable while the
// card reads into the other.
bool Responder::getObject(const Command &c) {
  const auto *o = objects_.find(c.params[0]);
  if (!o) return respond(c, InvalidHandle);
  if (o->folder) return respond(c, InvalidHandle);
  const auto file = storage_.open(objects_.path(c.params[0]), false);
  if (!file) return respond(c, AccessDenied);
  const uint64_t size = o->size;
  listener_.copyBegins(o->name, false);
  Out head;
  head.u32(uint32_t(std::min<uint64_t>(Header + size, 0xFFFFFFFF)));
  head.u16(DataBlock);
  head.u16(c.code);
  head.u32(c.transaction);
  uint64_t offset = 0;
  // Fills buffer `b` from the card; its length, or -1.
  const auto fill = [&](int b, size_t from) -> long {
    const size_t want = size_t(std::min<uint64_t>(chunk_ - from, size - offset));
    if (!want) return long(from);
    const long got = file->read(offset, buffers_[b] + from, want);
    if (got != long(want)) return -1;
    offset += want;
    return long(from + want);
  };
  std::memcpy(buffers_[0], head.bytes.data(), Header);
  long length = fill(0, Header);
  for (int b = 0;; b ^= 1) {
    // A card that stops reading mid-file: the transfer is cut, and the PC
    // reports the copy failed rather than taking a short file.
    if (length < 0) return listener_.copyEnds(), false;
    const bool last = offset == size;
    if (!pipe_.startSend(buffers_[b], size_t(length), last)) return listener_.copyEnds(), false;
    const long next = last ? 0 : fill(b ^ 1, 0);
    if (!pipe_.finishSend()) return listener_.copyEnds(), false;
    listener_.copyMoved(offset);
    if (last) break;
    length = next;
  }
  listener_.copyEnds();
  return respond(c, Ok);
}
bool Responder::sendObjectInfo(const Command &c) {
  std::vector<uint8_t> data;
  if (!receiveData(data)) return false;
  if (c.params[0] && c.params[0] != Storage) return respond(c, InvalidStorage);
  uint32_t folder;
  if (!folderOf(c.params[1], folder)) return respond(c, InvalidParent);
  In in{data.data(), data.size()};
  in.take(4);
  const auto format = in.u16();
  in.take(2);
  const uint32_t size = in.u32();
  in.take(2 + 4 * 6 + 4 + 2 + 4 + 4); // thumbnail and image fields, parent, association, sequence
  Entry entry{in.str(), format == Association, size == 0xFFFFFFFF ? UINT64_MAX : size};
  if (!in.ok) return respond(c, InvalidDataset);
  uint32_t handle;
  const auto code = createObject(folder, entry, handle);
  if (code != Ok) return respond(c, code);
  pending_ = entry.folder ? Pending{} : Pending{handle, entry.size};
  return respond(c, Ok, {Storage, parentParam(folder), handle});
}
// As SendObjectInfo, with the size in 64 bits: what Windows sends for a file
// of 4 GB or more.
bool Responder::sendObjectPropList(const Command &c) {
  std::vector<uint8_t> data;
  if (!receiveData(data)) return false;
  if (c.params[0] && c.params[0] != Storage) return respond(c, InvalidStorage);
  uint32_t folder;
  if (!folderOf(c.params[1], folder)) return respond(c, InvalidParent);
  Entry entry;
  entry.folder = uint16_t(c.params[2]) == Association;
  entry.size = uint64_t(c.params[3]) << 32 | c.params[4];
  In in{data.data(), data.size()};
  const auto count = in.u32();
  for (uint32_t i = 0; i < count && in.ok; ++i) {
    in.u32();
    const auto prop = in.u16(), type = in.u16();
    if (prop == FileName && type == Str) entry.name = in.str();
    else in.skip(type);
  }
  if (!in.ok) return respond(c, InvalidDataset);
  uint32_t handle;
  const auto code = createObject(folder, entry, handle);
  if (code != Ok) return respond(c, code, {0, 0, 0, 0});
  pending_ = entry.folder ? Pending{} : Pending{handle, entry.size};
  return respond(c, Ok, {Storage, parentParam(folder), handle});
}
// The PC's file onto the card, two buffers turning: the cable fills one
// while the card writes the other.
bool Responder::sendObject(const Command &c) {
  const auto pending = std::exchange(pending_, Pending{});
  const auto *o = objects_.find(pending.handle);
  std::unique_ptr<File> file;
  const auto path = o ? objects_.path(pending.handle) : std::string();
  if (o) file = storage_.open(path, true);
  if (!file) {
    std::vector<uint8_t> dropped;
    if (!receiveData(dropped)) return false;
    return respond(c, o ? AccessDenied : NoValidObjectInfo);
  }
  listener_.copyBegins(o->name, true);
  uint64_t total = pending.size, received = 0, written = 0;
  bool failed = false, first = true;
  size_t posted = size_t(total == UINT64_MAX ? chunk_ : std::min<uint64_t>(chunk_, Header + total));
  if (!pipe_.startReceive(buffers_[0], posted)) return listener_.copyEnds(), false;
  for (int b = 0;; b ^= 1) {
    const long n = pipe_.finishReceive();
    if (n < 0) {
      file.reset();
      storage_.remove(path, false);
      objects_.remove(pending.handle);
      return listener_.copyEnds(), false;
    }
    size_t skip = 0;
    if (first) {
      if (n < long(Header)) {
        if (n == 0 && pipe_.startReceive(buffers_[b], posted)) { b ^= 1; continue; } // a stray zero-length packet
        failed = true;
        break;
      }
      const uint32_t length = uint32_t(buffers_[b][0] | buffers_[b][1] << 8 | buffers_[b][2] << 16 | uint32_t(buffers_[b][3]) << 24);
      if (length != 0xFFFFFFFF) total = length - Header;
      skip = Header;
      first = false;
    }
    const size_t bytes = size_t(n) - skip;
    received += bytes;
    const bool more = size_t(n) == posted && (total == UINT64_MAX || received < total);
    if (more) {
      posted = size_t(total == UINT64_MAX ? chunk_ : std::min<uint64_t>(chunk_, total - received));
      if (!pipe_.startReceive(buffers_[b ^ 1], posted)) return listener_.copyEnds(), false;
    }
    if (!failed && bytes) {
      failed = !file->write(written, buffers_[b] + skip, bytes);
      written += bytes;
    }
    listener_.copyMoved(received);
    if (!more) break;
  }
  file.reset();
  listener_.copyEnds();
  if (failed) {
    storage_.remove(path, false);
    objects_.remove(pending.handle);
    return respond(c, storage_.freeSpace() < 1 << 20 ? StoreFull : GeneralError);
  }
  objects_.resize(pending.handle, written);
  return respond(c, Ok);
}
bool Responder::deleteObject(const Command &c) {
  const auto *o = objects_.find(c.params[0]);
  if (!o) return respond(c, c.params[0] == Any ? AccessDenied : InvalidHandle);
  const auto path = objects_.path(c.params[0]);
  if (guarded(path)) return respond(c, AccessDenied);
  if (!storage_.remove(path, o->folder)) return respond(c, AccessDenied);
  listener_.touched(objects_.path(o->parent));
  objects_.remove(c.params[0]);
  return respond(c, Ok);
}
bool Responder::propsSupported(const Command &c) {
  Out out;
  out.array16(Props);
  return sendData(c, out.bytes) && respond(c, Ok);
}
bool Responder::propDesc(const Command &c) {
  if (!supported(c.params[0])) return respond(c, InvalidPropCode);
  const auto prop = uint16_t(c.params[0]);
  const auto type = typeOf(prop);
  Out out;
  out.u16(prop);
  out.u16(type);
  out.u8(prop == FileName ? 1 : 0); // only the name can be set
  switch (type) {
  case Str: out.str(""); break;
  case U16: out.u16(0); break;
  case U64: out.u64(0); break;
  case U128: out.u64(0), out.u64(0); break;
  default: out.u32(0); break;
  }
  out.u32(0); // group
  out.u8(0);  // no form
  return sendData(c, out.bytes) && respond(c, Ok);
}
namespace {
void value(Out &out, uint16_t prop, uint32_t handle, const Objects::Object &o) {
  switch (prop) {
  case StorageId: return out.u32(Storage);
  case ObjectFormat: return out.u16(o.folder ? Association : Undefined);
  case ObjectSize: return out.u64(o.size);
  case FileName: return out.str(o.name);
  case ParentObject: return out.u32(parentParam(o.parent));
  case UniqueId: return out.u64(handle), out.u64(0);
  }
}
} // namespace
bool Responder::propValue(const Command &c) {
  const auto *o = objects_.find(c.params[0]);
  if (!o) return respond(c, InvalidHandle);
  if (!supported(c.params[1])) return respond(c, InvalidPropCode);
  Out out;
  value(out, uint16_t(c.params[1]), c.params[0], *o);
  return sendData(c, out.bytes) && respond(c, Ok);
}
// A rename: the only property the PC can set.
bool Responder::setPropValue(const Command &c) {
  std::vector<uint8_t> data;
  if (!receiveData(data)) return false;
  const auto *o = objects_.find(c.params[0]);
  if (!o) return respond(c, InvalidHandle);
  if (c.params[1] != FileName) return respond(c, supported(c.params[1]) ? AccessDenied : InvalidPropCode);
  In in{data.data(), data.size()};
  const auto name = in.str();
  if (!in.ok || !validName(name)) return respond(c, InvalidPropValue);
  const auto from = objects_.path(c.params[0]);
  const auto folder = objects_.path(o->parent);
  const auto to = join(folder, name);
  if (guarded(from) || guarded(to)) return respond(c, AccessDenied);
  if (name == o->name) return respond(c, Ok);
  const auto taken = objects_.child(o->parent, name);
  // Only the case of the name changes: the card sees one file either way.
  if (taken && taken != c.params[0]) return respond(c, AccessDenied);
  if (!storage_.rename(from, to, o->folder)) return respond(c, AccessDenied);
  objects_.rename(c.params[0], name);
  listener_.touched(folder);
  return respond(c, Ok);
}
bool Responder::propList(const Command &c) {
  const uint32_t handle = c.params[0], prop = c.params[2], depth = c.params[4];
  const auto format = uint16_t(c.params[1]);
  if (prop == 0) return respond(c, GroupUnsupported);
  if (prop != Any && !supported(prop)) return respond(c, InvalidPropCode);
  std::vector<uint32_t> handles;
  if (depth == 0) {
    if (!objects_.find(handle)) return respond(c, InvalidHandle);
    handles.push_back(handle);
  } else if (depth == 1) {
    uint32_t folder;
    if (!folderOf(handle == 0 ? Any : handle, folder)) return respond(c, InvalidHandle);
    const auto *children = listFolder(folder);
    if (!children) return respond(c, GeneralError);
    handles = *children;
  } else {
    return respond(c, DepthUnsupported);
  }
  Out out;
  out.u32(0);
  uint32_t count = 0;
  for (const auto h : handles) {
    const auto &o = *objects_.find(h);
    if (format && format != (o.folder ? Association : Undefined)) continue;
    for (const auto p : Props) {
      if (prop != Any && p != prop) continue;
      out.u32(h);
      out.u16(p);
      out.u16(typeOf(p));
      value(out, p, h, o);
      ++count;
    }
  }
  std::memcpy(out.bytes.data(), &count, 4);
  return sendData(c, out.bytes) && respond(c, Ok);
}
} // namespace foil::mtp
