// MTP to a PC: a PC's side of the conversation over a fake cable, against a
// folder standing in for the SD card. Small buffers make every copy cross
// several of them, so the two-buffer turns are what is tested.
#include "Test.hpp"
#include "mtp/Mtp.hpp"
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unistd.h>
namespace fs = std::filesystem;
using foil::mtp::Entry;
namespace {
constexpr size_t Packet = 512, Chunk = 4096;
std::vector<uint8_t> le(std::initializer_list<uint64_t> values, std::initializer_list<int> widths) {
  std::vector<uint8_t> out;
  auto w = widths.begin();
  for (const auto v : values) {
    for (int i = 0; i < *w; ++i) out.push_back(uint8_t(v >> (8 * i)));
    ++w;
  }
  return out;
}
uint32_t u32(const std::vector<uint8_t> &b, size_t at) { return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t(b[at + 3]) << 24; }
uint16_t u16(const std::vector<uint8_t> &b, size_t at) { return uint16_t(b[at] | b[at + 1] << 8); }
void str(std::vector<uint8_t> &out, const std::u16string &text) {
  if (text.empty()) return out.push_back(0);
  out.push_back(uint8_t(text.size() + 1));
  for (const auto c : text) out.push_back(uint8_t(c)), out.push_back(uint8_t(c >> 8));
  out.push_back(0), out.push_back(0);
}
// The cable: what the PC sent, transfer by transfer, and what came back.
struct FakePipe : foil::mtp::Pipe {
  std::deque<std::vector<uint8_t>> fromPc;
  std::vector<std::vector<uint8_t>> toPc;
  std::vector<uint8_t> sending;
  size_t posted = 0, zlps = 0;
  bool receiving = false, inFlight = false, twoSends = false;
  // The card read while a block was on the cable, written while the next came.
  bool readOverlapped = false, writeOverlapped = false;
  uint8_t *into = nullptr;
  // A PC closes a transfer that ends on a packet boundary with a zero-length one.
  void push(std::vector<uint8_t> transfer) {
    const bool boundary = !transfer.empty() && transfer.size() % Packet == 0;
    fromPc.push_back(std::move(transfer));
    if (boundary) fromPc.emplace_back();
  }
  bool startReceive(void *buf, size_t size) override {
    if (receiving) return false;
    receiving = true, into = static_cast<uint8_t *>(buf), posted = size;
    return true;
  }
  long finishReceive() override {
    receiving = false;
    if (fromPc.empty()) return -1;
    auto &front = fromPc.front();
    const size_t n = std::min(posted, front.size());
    std::copy(front.begin(), front.begin() + long(n), into);
    front.erase(front.begin(), front.begin() + long(n));
    if (front.empty() || n < posted) fromPc.pop_front();
    return long(n);
  }
  bool startSend(const void *buf, size_t size, bool last) override {
    if (inFlight) twoSends = true;
    inFlight = true;
    const auto *bytes = static_cast<const uint8_t *>(buf);
    sending.insert(sending.end(), bytes, bytes + size);
    if (last) {
      if (sending.size() % Packet == 0) ++zlps;
      toPc.push_back(std::move(sending));
      sending.clear();
    }
    return true;
  }
  bool finishSend() override { return inFlight = false, true; }
};
struct File : foil::mtp::File {
  std::fstream f;
  FakePipe *pipe = nullptr;
  long read(uint64_t offset, void *buf, size_t size) override {
    pipe->readOverlapped |= pipe->inFlight;
    f.clear();
    f.seekg(std::streamoff(offset));
    f.read(static_cast<char *>(buf), std::streamsize(size));
    return long(f.gcount());
  }
  bool write(uint64_t offset, const void *buf, size_t size) override {
    pipe->writeOverlapped |= pipe->receiving;
    f.clear();
    f.seekp(std::streamoff(offset));
    return bool(f.write(static_cast<const char *>(buf), std::streamsize(size)));
  }
};
struct Card : foil::mtp::Storage {
  fs::path root;
  FakePipe *pipe = nullptr;
  fs::path at(const std::string &path) const { return root / path.substr(1); }
  bool list(const std::string &folder, std::vector<Entry> &out) override {
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(at(folder), ec))
      out.push_back({e.path().filename().string(), e.is_directory(), e.is_directory() ? 0 : uint64_t(e.file_size())});
    std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
    return !ec;
  }
  uint64_t totalSpace() override { return 64ull << 30; }
  uint64_t freeSpace() override { return 32ull << 30; }
  bool createFile(const std::string &path, uint64_t) override { return bool(std::ofstream(at(path), std::ios::binary)); }
  bool createFolder(const std::string &path) override { return fs::create_directory(at(path)); }
  bool remove(const std::string &path, bool folder) override {
    std::error_code ec;
    return folder ? fs::remove_all(at(path), ec) > 0 : fs::remove(at(path), ec);
  }
  bool rename(const std::string &from, const std::string &to, bool) override {
    std::error_code ec;
    fs::rename(at(from), at(to), ec);
    return !ec;
  }
  std::unique_ptr<foil::mtp::File> open(const std::string &path, bool write) override {
    auto file = std::make_unique<File>();
    file->pipe = pipe;
    file->f.open(at(path), std::ios::binary | std::ios::in | (write ? std::ios::out : std::ios::openmode()));
    return file->f ? std::move(file) : nullptr;
  }
};
struct Seen : foil::mtp::Listener {
  bool open = false;
  int copies = 0;
  std::set<std::string> folders;
  void session(bool o) override { open = o; }
  void copyBegins(const std::string &, bool) override {}
  void copyMoved(uint64_t) override {}
  void copyEnds() override { ++copies; }
  void touched(const std::string &folder) override { folders.insert(folder); }
  void dropped() override { ++drops; }
  int drops = 0;
};
struct Reply {
  std::vector<uint8_t> data;
  uint16_t code = 0;
  std::vector<uint32_t> params;
};
// One operation as a PC runs it: the command, its data if any, and what the
// console answered.
struct Pc {
  FakePipe &pipe;
  foil::mtp::Responder &mtp;
  uint32_t transaction = 0;
  bool served = true;
  std::vector<uint8_t> container(uint16_t type, uint16_t code, const std::vector<uint8_t> &body, bool unknownLength = false) {
    auto out = le({unknownLength ? 0xFFFFFFFF : 12 + body.size(), type, code, transaction}, {4, 2, 2, 4});
    out.insert(out.end(), body.begin(), body.end());
    return out;
  }
  Reply call(uint16_t code, std::vector<uint32_t> params = {}, const std::vector<uint8_t> *data = nullptr,
             bool unknownLength = false) {
    ++transaction;
    std::vector<uint8_t> body;
    for (const auto p : params) body.insert(body.end(), {uint8_t(p), uint8_t(p >> 8), uint8_t(p >> 16), uint8_t(p >> 24)});
    pipe.push(container(1, code, body));
    if (data) pipe.push(container(2, code, *data, unknownLength));
    pipe.toPc.clear();
    served = mtp.serve();
    Reply reply;
    for (const auto &t : pipe.toPc) {
      if (t.size() < 12 || u32(t, 8) != transaction) continue;
      if (u16(t, 4) == 2) reply.data.assign(t.begin() + 12, t.end());
      if (u16(t, 4) == 3) {
        reply.code = u16(t, 6);
        for (size_t at = 12; at + 4 <= t.size(); at += 4) reply.params.push_back(u32(t, at));
      }
    }
    return reply;
  }
};
std::string bytes(size_t n, int seed) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = char((i * 131 + size_t(seed)) & 0xFF);
  return s;
}
void put(const fs::path &file, const std::string &content) { std::ofstream(file, std::ios::binary) << content; }
std::string get(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
// The handles under `folder`, by name.
std::map<std::string, uint32_t> names(Pc &pc, uint32_t folder) {
  std::map<std::string, uint32_t> out;
  const auto list = pc.call(0x9805, {folder, 0, 0xDC07, 0, 1});
  for (size_t at = 4; at + 8 < list.data.size();) {
    const auto handle = u32(list.data, at);
    const auto count = list.data[at + 8];
    std::string name;
    for (size_t i = 0; i + 1 < count; ++i) {
      const auto c = u16(list.data, at + 9 + 2 * i);
      name += c < 0x80 ? std::string(1, char(c)) : "?";
    }
    out[name] = handle;
    at += 9 + 2 * size_t(count);
  }
  return out;
}
std::vector<uint8_t> objectInfo(uint16_t format, uint32_t size, const std::u16string &name) {
  auto out = le({0x00010001, format, 0, size, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {4, 2, 2, 4, 2, 4, 4, 4, 4, 4, 4, 4, 2, 4, 4});
  str(out, name);
  str(out, u""), str(out, u""), str(out, u"");
  return out;
}
} // namespace
int runMtpTests() {
  int failures = 0;
  Card card;
  card.root = fs::temp_directory_path() / ("cookingfoil-mtp-" + std::to_string(getpid()));
  fs::remove_all(card.root);
  fs::create_directories(card.root / "switch");
  fs::create_directories(card.root / "Nintendo" / "Contents");
  fs::create_directories(card.root / "emuMMC");
  put(card.root / "switch" / "app.nro", bytes(1000, 1));
  put(card.root / "big.bin", bytes(3 * Chunk + 100, 2));
  put(card.root / "exact.bin", bytes(2 * Chunk - 12, 3)); // 12 + size ends on a packet
  fs::create_directories(card.root / "emuMMC" / "RAW1" / "Nintendo" / "Album" / "2026" / "10");
  put(card.root / "emuMMC" / "RAW1" / "Nintendo" / "Album" / "2026" / "10" / "shot.jpg", bytes(300, 6));

  std::vector<uint8_t> a(Chunk), b(Chunk);
  uint8_t *buffers[2] = {a.data(), b.data()};
  FakePipe pipe;
  card.pipe = &pipe;
  Seen seen;
  const auto album = foil::mtp::nintendoFolder(true, "[emummc]\nenabled=1\nnintendo_path=emuMMC/RAW1/Nintendo\n") + "/Album";
  foil::mtp::Responder mtp(pipe, card, seen, buffers, Chunk,
                           {{"SD Card", "", true, {"Nintendo", "emuMMC"}}, {"Album", album, false, {}}});
  Pc pc{pipe, mtp};

  // Before a session only the device's description answers; it names MTP.
  const auto info = pc.call(0x1001);
  CHECK(info.code == 0x2001 && u32(info.data, 2) == 6);
  CHECK(pc.call(0x1004).code == 0x2003);
  CHECK(pc.call(0x1002, {1}).code == 0x2001 && seen.open);
  // Two drives: the card, and the booted system's album.
  const auto ids = pc.call(0x1004);
  CHECK(ids.code == 0x2001 && u32(ids.data, 0) == 2 && u32(ids.data, 4) == 0x00010001 && u32(ids.data, 8) == 0x00020001);
  const auto sdInfo = pc.call(0x1005, {0x00010001});
  CHECK(sdInfo.code == 0x2001 && u16(sdInfo.data, 4) == 0);
  const auto albumInfo = pc.call(0x1005, {0x00020001});
  CHECK(albumInfo.code == 0x2001 && u16(albumInfo.data, 4) == 2 && albumInfo.data[26] == 6); // "Album" + terminator
  CHECK(pc.call(0x1005, {0x00030001}).code == 0x2008);

  // The card's top, without the system's folders.
  auto top = names(pc, 0xFFFFFFFF);
  CHECK(top.size() == 3 && top.count("switch") && top.count("big.bin") && top.count("exact.bin"));
  CHECK(!top.count("Nintendo") && !top.count("emuMMC"));
  const auto handles = pc.call(0x1007, {0x00010001, 0, 0xFFFFFFFF});
  CHECK(handles.code == 0x2001 && u32(handles.data, 0) == 3);
  // Read again, the same files keep their handles.
  CHECK(names(pc, 0xFFFFFFFF) == top);
  const auto info2 = pc.call(0x1008, {top["big.bin"]});
  CHECK(info2.code == 0x2001 && u32(info2.data, 8) == 3 * Chunk + 100);

  // Copies to the PC, across several buffers; one ending on a packet gets
  // the closing zero-length packet, the other does not.
  const auto bigFile = bytes(3 * Chunk + 100, 2);
  CHECK(pc.call(0x1009, {top["big.bin"]}).data == std::vector<uint8_t>(bigFile.begin(), bigFile.end()));
  CHECK(pipe.zlps == 0 && !pipe.twoSends && pipe.readOverlapped);
  const auto exact = pc.call(0x1009, {top["exact.bin"]});
  CHECK(exact.code == 0x2001 && exact.data.size() == 2 * Chunk - 12 && pipe.zlps == 1);
  const auto sw = names(pc, top["switch"]);
  CHECK(sw.size() == 1 && sw.count("app.nro"));
  CHECK(pc.call(0x1009, {sw.at("app.nro")}).data.size() == 1000);

  // A file from the PC: its info, then its bytes, written while the next
  // block comes in.
  const auto content = bytes(3 * Chunk + 7, 4);
  auto infoData = objectInfo(0x3000, uint32_t(content.size()), u"new.bin");
  const auto made = pc.call(0x100C, {0x00010001, top["switch"]}, &infoData);
  CHECK(made.code == 0x2001 && made.params.size() == 3 && made.params[2]);
  const std::vector<uint8_t> payload(content.begin(), content.end());
  CHECK(pc.call(0x100D, {}, &payload).code == 0x2001);
  CHECK(get(card.root / "switch" / "new.bin") == content && pipe.writeOverlapped);
  CHECK(seen.folders.count("/switch") && seen.copies >= 4);
  const auto size = pc.call(0x9803, {made.params[2], 0xDC04});
  CHECK(size.code == 0x2001 && u32(size.data, 0) == content.size());
  // SendObject with no SendObjectInfo before it: its data is read and refused.
  CHECK(pc.call(0x100D, {}, &payload).code == 0x2015 && pc.served);

  // A big file's way (SendObjectPropList, 64-bit size), its length unsaid in
  // the data: read to the PC's short packet.
  std::vector<uint8_t> props = le({1, 0, 0xDC07, 0xFFFF}, {4, 4, 2, 2});
  str(props, u"한글.bin");
  const auto big = bytes(2 * Chunk, 5);
  const auto listed = pc.call(0x9808, {0x00010001, 0xFFFFFFFF, 0x3000, 0, uint32_t(big.size())}, &props);
  CHECK(listed.code == 0x2001 && listed.params.size() == 3);
  const std::vector<uint8_t> bigData(big.begin(), big.end());
  CHECK(pc.call(0x100D, {}, &bigData, true).code == 0x2001);
  CHECK(get(card.root / "한글.bin") == big);

  // A folder, renamed, then deleted with what is in it.
  auto folderInfo = objectInfo(0x3001, 0, u"made");
  const auto folder = pc.call(0x100C, {0x00010001, 0xFFFFFFFF}, &folderInfo);
  CHECK(folder.code == 0x2001 && fs::is_directory(card.root / "made"));
  std::vector<uint8_t> rename;
  str(rename, u"renamed");
  CHECK(pc.call(0x9804, {folder.params[2], 0xDC07}, &rename).code == 0x2001);
  CHECK(fs::is_directory(card.root / "renamed") && !fs::exists(card.root / "made"));
  put(card.root / "renamed" / "inside.txt", "x");
  CHECK(pc.call(0x100B, {folder.params[2]}).code == 0x2001 && !fs::exists(card.root / "renamed"));
  CHECK(pc.call(0x1008, {folder.params[2]}).code == 0x2009);

  // The system's folders cannot be made, named or reached.
  auto nintendo = objectInfo(0x3001, 0, u"nintendo");
  CHECK(pc.call(0x100C, {0x00010001, 0xFFFFFFFF}, &nintendo).code == 0x200F);
  std::vector<uint8_t> toEmu;
  str(toEmu, u"emuMMC");
  CHECK(pc.call(0x9804, {top["switch"], 0xDC07}, &toEmu).code == 0x200F && fs::is_directory(card.root / "switch"));
  auto escape = objectInfo(0x3000, 1, u"../x");
  CHECK(pc.call(0x100C, {0x00010001, 0xFFFFFFFF}, &escape).code == 0xA806);

  // The same name again replaces the file (the PC asked first).
  auto again = objectInfo(0x3000, 3, u"new.bin");
  const auto replaced = pc.call(0x100C, {0x00010001, top["switch"]}, &again);
  const std::vector<uint8_t> three{'a', 'b', 'c'};
  CHECK(replaced.code == 0x2001 && pc.call(0x100D, {}, &three).code == 0x2001);
  CHECK(get(card.root / "switch" / "new.bin") == "abc");
  CHECK(names(pc, top["switch"]).size() == 2);

  // The album: its days and shots, copied off and deleted; nothing goes in
  // and nothing is renamed.
  const auto albumTop = pc.call(0x1007, {0x00020001, 0, 0xFFFFFFFF});
  CHECK(albumTop.code == 0x2001 && u32(albumTop.data, 0) == 1);
  const auto year = u32(albumTop.data, 4);
  CHECK(u32(pc.call(0x9803, {year, 0xDC01}).data, 0) == 0x00020001);
  const auto month = names(pc, year).at("10");
  const auto shot = names(pc, month).at("shot.jpg");
  CHECK(pc.call(0x1009, {shot}).data.size() == 300);
  auto intoAlbum = objectInfo(0x3000, 3, u"x.jpg");
  CHECK(pc.call(0x100C, {0x00020001, month}, &intoAlbum).code == 0x200F);
  std::vector<uint8_t> renameShot;
  str(renameShot, u"y.jpg");
  CHECK(pc.call(0x9804, {shot, 0xDC07}, &renameShot).code == 0x200F);
  CHECK(pc.call(0x100B, {shot}).code == 0x2001);
  CHECK(!fs::exists(card.root / "emuMMC" / "RAW1" / "Nintendo" / "Album" / "2026" / "10" / "shot.jpg"));

  CHECK(pc.call(0x1003).code == 0x2001 && !seen.open);
  CHECK(pc.call(0x1007, {0xFFFFFFFF, 0, 0xFFFFFFFF}).code == 0x2003);
  // The cable gone: serve() says so and the thread starts over.
  CHECK(!mtp.serve());

  // A copy that never finished leaves nothing on the card: cancelled after
  // its info, cut off mid-data, or the cable gone in between. Until its data
  // comes, an announced file is only a name the PC sees.
  const auto noParts = [&] {
    for (const auto &e : fs::recursive_directory_iterator(card.root))
      if (e.path().filename().string().rfind(".cookingfoil-part-", 0) == 0) return false;
    return true;
  };
  CHECK(pc.call(0x1002, {2}).code == 0x2001);
  auto gone = objectInfo(0x3000, 10, u"gone.bin");
  CHECK(pc.call(0x100C, {0x00010001, 0xFFFFFFFF}, &gone).code == 0x2001 && !fs::exists(card.root / "gone.bin"));
  CHECK(names(pc, 0xFFFFFFFF).count("gone.bin"));
  auto next = objectInfo(0x3000, 3, u"next.bin");
  CHECK(pc.call(0x100C, {0x00010001, 0xFFFFFFFF}, &next).code == 0x2001 && !names(pc, 0xFFFFFFFF).count("gone.bin"));
  CHECK(pc.call(0x100D, {}, &three).code == 0x2001 && get(card.root / "next.bin") == "abc" && noParts());
  // Short of its size, the transfer closed: refused, not kept.
  const auto sent = [&](size_t body) {
    ++pc.transaction;
    pipe.push(pc.container(1, 0x100D, {}));
    auto data = pc.container(2, 0x100D, std::vector<uint8_t>(body));
    const uint32_t whole = uint32_t(12 + 2 * Chunk);
    for (int i = 0; i < 4; ++i) data[size_t(i)] = uint8_t(whole >> (8 * i));
    pipe.fromPc.push_back(std::move(data));
    pipe.toPc.clear();
    return mtp.serve();
  };
  auto cut = objectInfo(0x3000, uint32_t(2 * Chunk), u"cut.bin");
  CHECK(pc.call(0x100C, {0x00010001, 0xFFFFFFFF}, &cut).code == 0x2001);
  const int drops = seen.drops;
  CHECK(sent(Chunk + 100) && u16(pipe.toPc.back(), 6) != 0x2001 && !fs::exists(card.root / "cut.bin") && noParts());
  CHECK(seen.drops == drops + 1 && !names(pc, 0xFFFFFFFF).count("cut.bin"));
  // The cable gone after one full block.
  CHECK(pc.call(0x100C, {0x00010001, 0xFFFFFFFF}, &cut).code == 0x2001);
  CHECK(!sent(Chunk - 12) && !fs::exists(card.root / "cut.bin") && noParts());
  mtp.reset();
  // Over a file of its name, cut off: the old file is still all there.
  CHECK(pc.call(0x1002, {3}).code == 0x2001);
  auto over = objectInfo(0x3000, uint32_t(2 * Chunk), u"next.bin");
  CHECK(pc.call(0x100C, {0x00010001, 0xFFFFFFFF}, &over).code == 0x2001 && get(card.root / "next.bin") == "abc");
  CHECK(!sent(Chunk - 12) && get(card.root / "next.bin") == "abc" && noParts());
  mtp.reset();
  CHECK(pc.call(0x1002, {4}).code == 0x2001);
  auto left = objectInfo(0x3000, 10, u"left.bin");
  CHECK(pc.call(0x100C, {0x00010001, 0xFFFFFFFF}, &left).code == 0x2001);
  mtp.reset();
  CHECK(!fs::exists(card.root / "left.bin") && get(card.root / "next.bin") == "abc");
  // What a power cut left mid-copy: gone once its folder is read, never shown.
  CHECK(pc.call(0x1002, {5}).code == 0x2001);
  put(card.root / "switch" / ".cookingfoil-part-half.nro", "half");
  const int before = seen.drops;
  const auto sw2 = names(pc, names(pc, 0xFFFFFFFF).at("switch"));
  CHECK(!sw2.count(".cookingfoil-part-half.nro") && noParts() && seen.drops == before + 1);
  auto sneaky = objectInfo(0x3000, 1, u".cookingfoil-part-x");
  CHECK(pc.call(0x100C, {0x00010001, 0xFFFFFFFF}, &sneaky).code == 0xA806);

  // Where the album is: the card's own Nintendo folder, or the emuMMC's.
  CHECK(foil::mtp::nintendoFolder(false, "[emummc]\nnintendo_path=emuMMC/SD00/Nintendo") == "/Nintendo");
  CHECK(foil::mtp::nintendoFolder(true, "[emummc]\r\nenabled=1\r\nnintendo_path = emuMMC/SD00/Nintendo/\r\n") == "/emuMMC/SD00/Nintendo");
  CHECK(foil::mtp::nintendoFolder(true, "[emummc]\npath=emuMMC/SD01\n") == "/emuMMC/SD01/Nintendo");
  CHECK(foil::mtp::nintendoFolder(true, "[other]\nnintendo_path=elsewhere\n") == "/emuMMC/RAW1/Nintendo");
  fs::remove_all(card.root);
  return failures;
}
