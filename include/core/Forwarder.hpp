#pragma once
// HOME Menu shortcuts (forwarders) built on the device for any .nro: a
// small application whose program is the homebrew loader (hbl) told, through
// its RomFS, which .nro to start.
//
// The layout and every rule here are ported from sphaira's owo.cpp
// (https://github.com/ITotalJustice/sphaira, GPL-3.0, by ITotalJustice),
// itself based on hacbrewpack (RomFS) and yati (install). Only the pure data
// work lives here, so it runs and is tested on the PC: the console encrypts
// the NCA headers (HeaderCipher) and writes the result (platform/Shortcut).
//
// What comes out is three NCAs, none of them encrypted beyond the header and
// none signed, so no ticket or title key is involved; running one needs
// signature patches on the console, as sphaira's do.
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace foil::forwarder {
// sphaira's id range: 0x05 in the top byte, 44 bits of SHA-256 over the
// path and arguments, the low 12 bits zero (room for the update and DLC ids).
constexpr uint64_t TitleIdPrefix = 0x0500000000000000ULL;
// The range sphaira used before; a shortcut left there for the same path is
// the same shortcut, removed when a new one is made.
constexpr uint64_t LegacyTitleIdPrefix = 0x0100000000000000ULL;
// "switch/app.nro" (SD-root relative, as the card scan lists it) or
// "sdmc:/switch/app.nro" -> "/switch/app.nro", the form hbl and sphaira use.
std::string loaderPath(const std::string &path);
// What the started app gets as argv: its own path first (quoted when it has
// a space), then `extra` when there is any.
std::string launchArgs(const std::string &nroPath, const std::string &extra = "");
// The shortcut's id, the same for the same path and arguments every time, so
// making it again replaces it instead of adding a second one. Computed as
// sphaira does: SHA-256 over path + (path [+ ' ' + extra]), the first 8 bytes
// little-endian, masked into the prefix's range.
uint64_t titleId(const std::string &nroPath, const std::string &extra = "", uint64_t prefix = TitleIdPrefix);
std::string hexId(uint64_t id);          // 16 upper-case hex digits
uint64_t parseHexId(const std::string &); // 0 when it is no 16-digit id

constexpr size_t NacpSize = 0x4000;
// A NACP for an .nro that carries none: the name and author in every
// language entry, the version as given.
std::string blankNacp(const std::string &name, const std::string &author, const std::string &version);
// The .nro's NACP made fit for a shortcut, as sphaira does: the name and
// author (when not empty) in every language entry, the ids moved to `id`,
// no save data, screenshots and video on, no account needed to start.
// False when `nacp` is not NacpSize bytes.
bool patchNacp(std::string &nacp, uint64_t id, const std::string &name = "", const std::string &author = "");
// hbl's main.npdm set to run as `id`. `forceDebug` sets the debug flag
// capability to Atmosphere 1.8.0's ForceDebug bit (sphaira's automatic
// mode); false when the NPDM is not one (no META, ACI0, ACID or kernel flags).
bool patchNpdm(std::string &npdm, uint64_t id, bool forceDebug);

struct File {
  std::string name; // RomFS: "/name" at the root; PFS0: "name"
  std::string data;
};
// A RomFS with every file at its root, as hbl reads it. Padded to 0x4000;
// `size` gets the length before the padding.
std::string romfs(const std::vector<File> &files, uint64_t *size = nullptr);
std::string pfs0(const std::vector<File> &files);

using Digest = std::array<uint8_t, 32>;
Digest sha256(const std::string &data);

// NCA content types (in the NCA header) and NCM content types (in the
// content meta) are two different numberings.
enum class NcaType : uint8_t { Program = 0, Meta = 1, Control = 2 };
enum class ContentType : uint8_t { Meta = 0, Program = 1, Control = 3 };
constexpr size_t NcaHeaderSize = 0xC00;
// Encrypts the first NcaHeaderSize bytes in place (AES-XTS with the
// console's header key, 0x200-byte sectors from 0).
using HeaderCipher = std::function<void(uint8_t *header)>;

struct Nca {
  std::string bytes;
  ContentType type = ContentType::Program;
  Digest hash{}; // over the finished (header-encrypted) NCA; the first 16 bytes are its content id
};
struct Input {
  std::string nroPath; // as loaderPath() gives it
  std::string extraArgs;
  std::string name, author; // empty keeps the NACP's own
  std::string nacp;         // NacpSize bytes; blankNacp() for an .nro without one
  std::string icon;         // the .nro's JPEG
  std::string hblMain, hblNpdm;
  bool forceDebug = false;
  uint8_t storageId = 5; // NcmStorageId_SdCard
};
struct Package {
  uint64_t titleId = 0;
  std::vector<Nca> ncas; // program, control, meta
  // What NCM and NS get: the content meta key (0x10 bytes) and the content
  // meta data for the database (header, application extended header, then
  // the meta, program and control content infos).
  std::string metaKey, metaData;
};
// False with `error` (an internal reason, for the log) when an input is
// missing or malformed.
bool build(const Input &input, const HeaderCipher &cipher, Package &out, std::string &error);

// Signature patches are what lets an unsigned NCA run: sys-patch, or
// Atmosphere or hekate patch files on the card. A guess from the SD card,
// used only to warn.
bool signaturePatchesFound(const std::string &sdRoot);

// Whether to offer this app's own shortcut at start: opened from the album
// (applet mode), never asked before, its .nro path known and no shortcut for
// that path yet.
bool offerSelfShortcut(bool applicationMode, bool asked, const std::string &selfPath, bool shortcutPresent);
} // namespace foil::forwarder
