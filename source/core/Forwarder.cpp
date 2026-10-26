// Ported from sphaira's owo.cpp (https://github.com/ITotalJustice/sphaira,
// GPL-3.0, by ITotalJustice; RomFS building after hacbrewpack). The structs
// there are written here as byte offsets so the same layout is built and
// checked on the PC; the offsets are checked against libnx in
// source/platform/Shortcut.cpp.
#include "core/Forwarder.hpp"
#include "core/Sha256.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
namespace foil::forwarder {
namespace {
constexpr uint32_t IvfcBlock = 0x4000, ExefsBlock = 0x10000, MetaBlock = 0x1000, Sector = 0x200;
constexpr uint32_t RomfsDataAt = 0x200, Empty = 0xFFFFFFFF;
constexpr int IvfcLevels = 6;
// NCA header fields.
constexpr size_t NcaMagicAt = 0x200, NcaDistributionAt = 0x204, NcaTypeAt = 0x205, NcaSizeAt = 0x208,
                 NcaProgramIdAt = 0x210, NcaSdkAt = 0x21C, NcaSectionsAt = 0x240, NcaHeaderHashesAt = 0x280,
                 NcaFsHeadersAt = 0x400, FsHeaderSize = 0x200;
constexpr uint32_t Nca3 = 0x3341434E, SdkVersion = 0x000C1100;
// One FS header: version, type, hash type, encryption, then the hash data.
constexpr size_t FsTypeAt = 0x2, FsHashTypeAt = 0x3, FsEncryptionAt = 0x4, FsHashDataAt = 0x8;
constexpr uint8_t FsRomfs = 0, FsPfs0 = 1, HashSha256 = 2, HashIntegrity = 3, EncryptionNone = 1;
// The NACP fields sphaira sets (libnx NacpStruct offsets).
constexpr size_t NacpEntry = 0x300, NacpName = 0x200, NacpAuthor = 0x100, NacpEntries = 16, NacpVersionAt = 0x3060;

void put(std::string &b, size_t at, uint64_t v, int bytes) {
  if (b.size() < at + bytes) b.resize(at + bytes, '\0');
  for (int i = 0; i < bytes; ++i) b[at + i] = char(uint8_t(v >> (8 * i)));
}
uint64_t get(const std::string &b, size_t at, int bytes) {
  uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = v << 8 | uint8_t(b[at + i]);
  return v;
}
void putBytes(std::string &b, size_t at, const void *data, size_t size) {
  if (b.size() < at + size) b.resize(at + size, '\0');
  std::copy_n(static_cast<const char *>(data), size, b.begin() + std::ptrdiff_t(at));
}
uint64_t align(uint64_t v, uint64_t to) { return (v + to - 1) & ~(to - 1); }
// sphaira's padding: always adds something, a whole block when already
// aligned. Kept as is so the sizes match the layout proven on consoles.
void pad(std::string &b, uint64_t block) { b.append(size_t(block - b.size() % block), '\0'); }
uint32_t pathHash(const std::string &name) {
  uint32_t hash = 0 ^ 123456789u; // the root's offset is the parent
  for (const unsigned char c : name) {
    hash = (hash >> 5) | (hash << 27);
    hash ^= c;
  }
  return hash;
}
uint32_t hashCount(uint32_t entries) {
  if (entries < 3) return 3;
  if (entries < 19) return entries | 1;
  uint32_t count = entries;
  while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 || count % 11 == 0 || count % 13 == 0 ||
         count % 17 == 0)
    ++count;
  return count;
}
std::string hashBlocks(const std::string &data, uint32_t block) {
  std::string out;
  for (size_t at = 0; at < data.size(); at += block) {
    const auto d = sha256(data.substr(at, block));
    out.append(reinterpret_cast<const char *>(d.data()), d.size());
  }
  return out;
}
// The section table entry and, once the section is in, its FS header hash.
void section(std::string &nca, int index, uint64_t start, uint64_t end) {
  const size_t at = NcaSectionsAt + size_t(index) * 0x10;
  put(nca, at, start / Sector, 4);
  put(nca, at + 4, end / Sector, 4);
  nca[at + 8] = 1;
}
void sealFsHeader(std::string &nca, int index) {
  const auto d = sha256(nca.substr(NcaFsHeadersAt + size_t(index) * FsHeaderSize, FsHeaderSize));
  putBytes(nca, NcaHeaderHashesAt + size_t(index) * 0x20, d.data(), d.size());
}
uint64_t sectionStart(const std::string &nca, int index) {
  return index == 0 ? NcaHeaderSize : get(nca, NcaSectionsAt + size_t(index - 1) * 0x10 + 4, 4) * Sector;
}
void addPfs0(std::string &nca, int index, const std::vector<File> &files, uint32_t block) {
  const auto image = pfs0(files);
  const auto table = hashBlocks(image, block);
  const uint64_t start = sectionStart(nca, index);
  nca += table;
  const size_t before = nca.size();
  nca.append(size_t(Sector - table.size() % Sector), '\0');
  const uint64_t imageAt = table.size() + (nca.size() - before);
  nca += image;
  pad(nca, Sector);
  const size_t fs = NcaFsHeadersAt + size_t(index) * FsHeaderSize;
  put(nca, fs, 2, 2);
  nca[fs + FsTypeAt] = char(FsPfs0);
  nca[fs + FsHashTypeAt] = char(HashSha256);
  nca[fs + FsEncryptionAt] = char(EncryptionNone);
  const size_t hd = fs + FsHashDataAt;
  const auto master = sha256(table);
  putBytes(nca, hd, master.data(), master.size());
  put(nca, hd + 0x20, block, 4);
  put(nca, hd + 0x24, 2, 4);               // layer count
  put(nca, hd + 0x28, 0, 8);               // hash table at the section start
  put(nca, hd + 0x30, table.size(), 8);
  put(nca, hd + 0x38, imageAt, 8);         // then the PFS0
  put(nca, hd + 0x40, image.size(), 8);
  section(nca, index, start, nca.size());
  sealFsHeader(nca, index);
}
void addRomfs(std::string &nca, int index, const std::vector<File> &files) {
  std::string level[IvfcLevels];
  uint64_t romfsSize = 0;
  level[5] = romfs(files, &romfsSize);
  for (int b = 4; b >= 0; --b) {
    level[b] = hashBlocks(level[b + 1], IvfcBlock);
    pad(level[b], IvfcBlock);
  }
  const uint64_t start = sectionStart(nca, index);
  for (const auto &l : level) nca += l;
  pad(nca, Sector);
  const size_t fs = NcaFsHeadersAt + size_t(index) * FsHeaderSize;
  put(nca, fs, 2, 2);
  nca[fs + FsTypeAt] = char(FsRomfs);
  nca[fs + FsHashTypeAt] = char(HashIntegrity);
  nca[fs + FsEncryptionAt] = char(EncryptionNone);
  const size_t hd = fs + FsHashDataAt;
  put(nca, hd, 0x43465649, 4);       // "IVFC"
  put(nca, hd + 0x4, 0x20000, 4);
  put(nca, hd + 0x8, 0x20, 4);       // master hash size
  put(nca, hd + 0xC, 7, 4);          // max layers
  uint64_t logical = 0;
  for (int i = 0; i < IvfcLevels; ++i) {
    const size_t lv = hd + 0x10 + size_t(i) * 0x18;
    const uint64_t size = i == 5 ? romfsSize : level[i].size();
    put(nca, lv, logical, 8);
    put(nca, lv + 8, size, 8);
    put(nca, lv + 0x10, 0x0E, 4);    // log2(0x4000)
    logical += size;
  }
  const auto master = sha256(level[0]);
  putBytes(nca, hd + 0xC0, master.data(), master.size());
  section(nca, index, start, nca.size());
  sealFsHeader(nca, index);
}
Nca finish(std::string nca, NcaType type, ContentType content, uint64_t id, const HeaderCipher &cipher) {
  put(nca, NcaMagicAt, Nca3, 4);
  nca[NcaDistributionAt] = 0; // system
  nca[NcaTypeAt] = char(type);
  put(nca, NcaSizeAt, nca.size(), 8);
  put(nca, NcaProgramIdAt, id, 8);
  put(nca, NcaSdkAt, SdkVersion, 4);
  if (cipher) cipher(reinterpret_cast<uint8_t *>(nca.data()));
  Nca out;
  out.hash = sha256(nca);
  out.bytes = std::move(nca);
  out.type = content;
  return out;
}
std::string blankHeader() { return std::string(NcaHeaderSize, '\0'); }
// NcmContentInfo: content id, 40-bit size, attributes, type, id offset.
std::string contentInfo(const Digest &hash, uint64_t size, ContentType type) {
  std::string info(0x18, '\0');
  putBytes(info, 0, hash.data(), 0x10);
  put(info, 0x10, size, 5);
  info[0x16] = char(type);
  return info;
}
std::string cnmtName(uint64_t id) {
  char name[48];
  std::snprintf(name, sizeof(name), "Application_%016llX.cnmt", static_cast<unsigned long long>(id));
  return name;
}
} // namespace

Digest sha256(const std::string &data) {
  Sha256 hasher;
  hasher.update(data.data(), data.size());
  const auto hex = hasher.hex();
  Digest out{};
  for (size_t i = 0; i < out.size(); ++i) out[i] = uint8_t(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
  return out;
}
std::string loaderPath(const std::string &path) {
  if (path.rfind("sdmc:/", 0) == 0) return path.substr(5);
  if (!path.empty() && path.front() == '/') return path;
  return "/" + path;
}
std::string launchArgs(const std::string &nroPath, const std::string &extra) {
  auto args = nroPath.find(' ') == std::string::npos ? nroPath : "\"" + nroPath + "\"";
  if (!extra.empty()) args += " " + extra;
  return args;
}
uint64_t titleId(const std::string &nroPath, const std::string &extra, uint64_t prefix) {
  // sphaira hashes the arguments unquoted: path + path [+ ' ' + extra].
  const auto args = extra.empty() ? nroPath : nroPath + " " + extra;
  const auto d = sha256(nroPath + args);
  uint64_t first = 0;
  for (int i = 7; i >= 0; --i) first = first << 8 | d[size_t(i)];
  return prefix | (first & 0x00FFFFFFFFFFF000ULL);
}
std::string hexId(uint64_t id) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(id));
  return text;
}
uint64_t parseHexId(const std::string &text) {
  if (text.size() != 16 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return 0;
  return std::stoull(text, nullptr, 16);
}
bool shortcutId(uint64_t id) { return (id >> 56) == (TitleIdPrefix >> 56) && (id & 0xFFF) == 0; }
uint64_t serverShortcutId(const std::string &hex) {
  const auto id = parseHexId(hex);
  return shortcutId(id) ? id : 0;
}
std::string blankNacp(const std::string &name, const std::string &author, const std::string &version) {
  std::string nacp(NacpSize, '\0');
  for (size_t i = 0; i < NacpEntries; ++i) {
    putBytes(nacp, i * NacpEntry, name.data(), std::min(name.size(), NacpName - 1));
    putBytes(nacp, i * NacpEntry + NacpName, author.data(), std::min(author.size(), NacpAuthor - 1));
  }
  putBytes(nacp, NacpVersionAt, version.data(), std::min<size_t>(version.size(), 0xF));
  return nacp;
}
bool patchNacp(std::string &nacp, uint64_t id, const std::string &name, const std::string &author) {
  if (nacp.size() != NacpSize) return false;
  const auto text = [&](size_t at, size_t room, const std::string &value) {
    std::fill_n(nacp.begin() + std::ptrdiff_t(at), room, '\0');
    putBytes(nacp, at, value.data(), std::min(value.size(), room - 1));
  };
  for (size_t i = 0; i < NacpEntries; ++i) {
    if (!name.empty()) text(i * NacpEntry, NacpName, name);
    if (!author.empty()) text(i * NacpEntry + NacpName, NacpAuthor, author);
  }
  nacp[0x3025] = 0;    // startup_user_account: no account picker
  nacp[0x3026] = 0;    // user_account_switch_lock
  nacp[0x3027] = 1;    // add_on_content_registration_type: on demand
  nacp[0x3034] = 0;    // screenshot: allowed
  nacp[0x3035] = 2;    // video_capture: automatic
  nacp[0x3036] = 0;    // data_loss_confirmation: none, it has no save data
  nacp[0x3037] = 0;    // play_log_policy: open
  put(nacp, 0x3038, id, 8);           // presence_group_id
  put(nacp, 0x3070, id ^ 0x1000, 8);  // add_on_content_base_id
  put(nacp, 0x3078, id, 8);           // save_data_owner_id
  for (size_t at : {0x3080, 0x3088, 0x3090, 0x3098}) put(nacp, at, 0, 8); // save data sizes
  // application_error_code_category is char[8]; sphaira puts its name there.
  std::fill_n(nacp.begin() + 0x30A8, 8, '\0');
  putBytes(nacp, 0x30A8, "sphaira", 7);
  for (size_t i = 0; i < 8; ++i) put(nacp, 0x30B0 + i * 8, id, 8); // local_communication_id
  nacp[0x30F0] = 2;    // logo_type
  nacp[0x30F1] = 0;    // logo_handling: automatic
  put(nacp, 0x30F8, id, 8);           // pseudo_device_id_seed
  for (size_t at : {0x3148, 0x3150, 0x3158, 0x3160}) put(nacp, at, 0, 8); // save data maximums
  nacp[0x3210] = 0;    // play_log_query_capability
  nacp[0x3213] = 0;    // required_network_service_license_on_launch
  return true;
}
bool patchNpdm(std::string &npdm, uint64_t id, bool forceDebug, int cores) {
  if (npdm.size() < 0x80 || npdm.compare(0, 4, "META") != 0) return false;
  const uint64_t aci0 = get(npdm, 0x70, 4), acid = get(npdm, 0x78, 4);
  if (aci0 + 0x40 > npdm.size() || acid + 0x240 > npdm.size() || npdm.compare(aci0, 4, "ACI0") != 0 ||
      npdm.compare(acid + 0x200, 4, "ACID") != 0)
    return false;
  // A kernel capability whose low `bits` bits are all set (and the next
  // clear) is that kind's descriptor; `value` replaces it.
  const auto patchKc = [&](uint64_t at, uint64_t size, uint32_t bits, uint32_t value) {
    const uint32_t pattern = (1u << bits) - 1, mask = (1u << bits) | pattern;
    for (uint64_t i = 0; i + 4 <= size && at + i + 4 <= npdm.size(); i += 4)
      if ((uint32_t(get(npdm, at + i, 4)) & mask) == pattern) {
        put(npdm, at + i, value | pattern, 4);
        return true;
      }
    return false;
  };
  // Three cores, priorities 28 to 59: the kernel flags hbl.json already has,
  // written again as sphaira does so a different NPDM still comes out right.
  constexpr uint32_t ThreeCores = ((((2u << 8) | 0u) << 6 | 28u) << 6 | 59u) << 4;
  static_assert((ThreeCores | 0x7) == 0x020073B7, "sphaira's three-core kernel flags");
  // Four: cores 0 to 3, priorities 28 to 63, as sphaira's CPU Cores 4.
  constexpr uint32_t FourCores = ((((3u << 8) | 0u) << 6 | 28u) << 6 | 63u) << 4;
  static_assert((FourCores | 0x7) == 0x030073F7, "sphaira's four-core kernel flags");
  const uint32_t flags = cores == 4 ? FourCores : ThreeCores;
  const uint64_t aci0Kc = aci0 + get(npdm, aci0 + 0x30, 4), aci0KcSize = get(npdm, aci0 + 0x34, 4);
  const uint64_t acidKc = acid + get(npdm, acid + 0x230, 4), acidKcSize = get(npdm, acid + 0x234, 4);
  if (!patchKc(aci0Kc, aci0KcSize, 3, flags) || !patchKc(acidKc, acidKcSize, 3, flags)) return false;
  if (forceDebug) {
    patchKc(aci0Kc, aci0KcSize, 16, 1u << 19);
    patchKc(acidKc, acidKcSize, 16, 1u << 19);
  }
  std::fill_n(npdm.begin() + 0x20, 0x20, '\0'); // title name and product code
  putBytes(npdm, 0x20, "Application", 11);
  npdm[0xC] = char((uint8_t(npdm[0xC]) & ~0x0E) | (1 << 1)); // 36-bit address space
  put(npdm, aci0 + 0x10, id, 8);
  put(npdm, acid + 0x210, id, 8);
  put(npdm, acid + 0x218, id, 8);
  return true;
}
std::string romfs(const std::vector<File> &files, uint64_t *size) {
  // Every file in the root directory, in the given order: data at 0x10
  // steps from 0x200, then the hash and entry tables.
  std::vector<uint64_t> dataAt;
  std::vector<uint32_t> entryAt;
  uint64_t data = 0;
  uint32_t entries = 0;
  for (const auto &f : files) {
    data = align(data, 0x10);
    dataAt.push_back(data);
    data += f.data.size();
    entryAt.push_back(entries);
    entries += 0x20 + uint32_t(align(f.name.size() - 1, 4));
  }
  const uint32_t dirBuckets = hashCount(1), fileBuckets = hashCount(uint32_t(files.size()));
  std::vector<uint32_t> dirHash(dirBuckets, Empty), fileHash(fileBuckets, Empty);
  std::string fileTable(entries, '\0');
  for (size_t i = 0; i < files.size(); ++i) {
    const auto name = files[i].name.substr(1);
    const auto at = entryAt[i];
    const uint32_t bucket = pathHash(name) % fileBuckets;
    put(fileTable, at, 0, 4); // parent: the root
    put(fileTable, at + 4, i + 1 < files.size() ? entryAt[i + 1] : Empty, 4);
    put(fileTable, at + 8, dataAt[i], 8);
    put(fileTable, at + 0x10, files[i].data.size(), 8);
    put(fileTable, at + 0x18, fileHash[bucket], 4);
    fileHash[bucket] = at;
    put(fileTable, at + 0x1C, name.size(), 4);
    putBytes(fileTable, at + 0x20, name.data(), name.size());
  }
  std::string dirTable(0x18, '\0');
  const uint32_t rootBucket = pathHash("") % dirBuckets;
  put(dirTable, 0, 0, 4);
  put(dirTable, 4, Empty, 4);
  put(dirTable, 8, Empty, 4);
  put(dirTable, 0xC, files.empty() ? Empty : 0, 4);
  put(dirTable, 0x10, dirHash[rootBucket], 4);
  dirHash[rootBucket] = 0;
  put(dirTable, 0x14, 0, 4);
  std::string out;
  const uint64_t dirHashAt = align(data + RomfsDataAt, 4), dirTableAt = dirHashAt + dirBuckets * 4,
                 fileHashAt = dirTableAt + dirTable.size(), fileTableAt = fileHashAt + fileBuckets * 4;
  put(out, 0x00, 0x50, 8);
  put(out, 0x08, dirHashAt, 8);
  put(out, 0x10, dirBuckets * 4, 8);
  put(out, 0x18, dirTableAt, 8);
  put(out, 0x20, dirTable.size(), 8);
  put(out, 0x28, fileHashAt, 8);
  put(out, 0x30, fileBuckets * 4, 8);
  put(out, 0x38, fileTableAt, 8);
  put(out, 0x40, fileTable.size(), 8);
  put(out, 0x48, RomfsDataAt, 8);
  for (size_t i = 0; i < files.size(); ++i) putBytes(out, RomfsDataAt + dataAt[i], files[i].data.data(), files[i].data.size());
  for (uint32_t i = 0; i < dirBuckets; ++i) put(out, dirHashAt + i * 4, dirHash[i], 4);
  putBytes(out, dirTableAt, dirTable.data(), dirTable.size());
  for (uint32_t i = 0; i < fileBuckets; ++i) put(out, fileHashAt + i * 4, fileHash[i], 4);
  putBytes(out, fileTableAt, fileTable.data(), fileTable.size());
  if (size) *size = out.size();
  pad(out, IvfcBlock);
  return out;
}
std::string pfs0(const std::vector<File> &files) {
  std::string names;
  std::string table;
  uint64_t data = 0;
  for (const auto &f : files) {
    const size_t at = table.size();
    put(table, at, data, 8);
    put(table, at + 8, f.data.size(), 8);
    put(table, at + 0x10, names.size(), 4);
    put(table, at + 0x14, 0, 4);
    names += f.name;
    names += '\0';
    data += f.data.size();
  }
  names.resize(size_t(align(names.size(), 0x20)), '\0');
  std::string out;
  put(out, 0, 0x30534650, 4); // "PFS0"
  put(out, 4, files.size(), 4);
  put(out, 8, names.size(), 4);
  put(out, 0xC, 0, 4);
  out += table + names;
  for (const auto &f : files) out += f.data;
  return out;
}
bool build(const Input &in, const HeaderCipher &cipher, Package &out, std::string &error) {
  out = {};
  if (in.nroPath.empty() || in.nroPath.front() != '/') error = "nro path";
  else if (in.icon.empty()) error = "icon";
  else if (in.nacp.size() != NacpSize) error = "nacp";
  else if (in.hblMain.empty() || in.hblNpdm.empty()) error = "loader";
  if (!error.empty()) return false;
  const uint64_t id = titleId(in.nroPath, in.extraArgs);
  auto npdm = in.hblNpdm;
  if (!patchNpdm(npdm, id, in.forceDebug, in.cores)) {
    error = "npdm";
    return false;
  }
  const auto args = launchArgs(in.nroPath, in.extraArgs);
  std::string program = blankHeader();
  addPfs0(program, 0, {{"main", in.hblMain}, {"main.npdm", npdm}}, ExefsBlock);
  addRomfs(program, 1, {{"/nextArgv", args}, {"/nextNroPath", in.nroPath}});
  out.ncas.push_back(finish(std::move(program), NcaType::Program, ContentType::Program, id, cipher));
  auto nacp = in.nacp;
  patchNacp(nacp, id, in.name, in.author);
  std::string control = blankHeader();
  addRomfs(control, 0, {{"/control.nacp", nacp}, {"/icon_AmericanEnglish.dat", in.icon}});
  out.ncas.push_back(finish(std::move(control), NcaType::Control, ContentType::Control, id, cipher));
  // The CNMT: header, application extended header (patch id), the program
  // and control as packaged content infos (hash + info), a zero digest.
  std::string cnmt(0x30, '\0');
  put(cnmt, 0, id, 8);
  put(cnmt, 8, 0, 4);              // version
  cnmt[0xC] = char(0x80);          // application
  put(cnmt, 0xE, 0x10, 2);         // extended header size
  put(cnmt, 0x10, 2, 2);           // contents: program, control
  put(cnmt, 0x12, 1, 2);           // content meta count
  cnmt[0x14] = 0;                  // attributes
  cnmt[0x15] = char(in.storageId);
  put(cnmt, 0x20, id | 0x800, 8);  // patch id
  put(cnmt, 0x28, 0, 8);
  for (const auto &nca : out.ncas) {
    cnmt.append(reinterpret_cast<const char *>(nca.hash.data()), nca.hash.size());
    cnmt += contentInfo(nca.hash, nca.bytes.size(), nca.type);
  }
  cnmt.append(0x20, '\0');
  std::string meta = blankHeader();
  addPfs0(meta, 0, {{cnmtName(id), cnmt}}, MetaBlock);
  out.ncas.push_back(finish(std::move(meta), NcaType::Meta, ContentType::Meta, id, cipher));
  const auto &metaNca = out.ncas.back();
  out.titleId = id;
  // NcmContentMetaKey: id, version, type, install type (full).
  put(out.metaKey, 0, id, 8);
  put(out.metaKey, 8, 0, 4);
  out.metaKey.resize(0x10, '\0');
  out.metaKey[0xC] = char(0x80);
  out.metaKey[0xD] = 0;
  // NcmContentMetaHeader with the meta itself counted, then the extended
  // header, then the infos: meta, program, control. sphaira records the
  // CNMT's size for the meta; the NCA's own size is what is on the card.
  put(out.metaData, 0, 0x10, 2);
  put(out.metaData, 2, 3, 2);
  put(out.metaData, 4, 1, 2);
  put(out.metaData, 6, 0, 2);
  put(out.metaData, 8, id | 0x800, 8);
  put(out.metaData, 0x10, 0, 8);
  out.metaData += contentInfo(metaNca.hash, metaNca.bytes.size(), ContentType::Meta);
  out.metaData += contentInfo(out.ncas[0].hash, out.ncas[0].bytes.size(), ContentType::Program);
  out.metaData += contentInfo(out.ncas[1].hash, out.ncas[1].bytes.size(), ContentType::Control);
  return true;
}
bool offerSelfShortcut(bool applicationMode, bool asked, const std::string &selfPath, bool shortcutPresent) {
  const bool nro = selfPath.size() > 4 && selfPath.compare(selfPath.size() - 4, 4, ".nro") == 0;
  return !applicationMode && !asked && nro && !shortcutPresent;
}
} // namespace foil::forwarder
