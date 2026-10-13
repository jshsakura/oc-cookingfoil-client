#pragma once
// Apps already on the SD card, whoever put them there: the .nro files under
// sdmc:/switch/ (the folder itself and one level below), each read for the
// name, version and icon it carries.
#include "core/Homebrew.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace foil::homebrew {
// What an .nro says about itself; every field may be empty.
struct NroInfo {
  std::string name, author, version;
  std::string icon; // JPEG bytes
  std::string nacp; // the whole NACP (0x4000 bytes) when it has one, for a HOME Menu shortcut
};
// Reads `size` bytes at `offset` of the file into `out`; false when it cannot.
using NroReader = std::function<bool(uint64_t offset, size_t size, std::string &out)>;
// The NACP title entries to try, best first, for an interface language
// ("ko", "en", "ja", "zh"); the English ones always follow.
std::vector<int> nacpLanguages(const std::string &code);
// False when the file is no .nro (no "NRO0", too short). A missing or broken
// asset section ("ASET") still returns true, with the fields left empty.
bool readNro(uint64_t fileSize, const NroReader &read, const std::vector<int> &languages, NroInfo &out);
bool parseNro(const std::string &bytes, const std::vector<int> &languages, NroInfo &out);
// readNro() on a file; false when it cannot be opened or is no .nro.
bool readNroFile(const std::string &path, const std::vector<int> &languages, NroInfo &out, uint64_t *size = nullptr);
// An app file on the card, its path from the SD card root.
struct CardNro {
  std::string path, name, author, version, icon;
  uint64_t size = 0;
};
// Whether a path from the SD card root is one the card scan lists:
// switch/<x>.nro or switch/<folder>/<x>.nro, not this app's own folder
// (switch/cookingfoil/) and not hbmenu.nro.
bool cardNroPath(const std::string &path);
// The listed ones of `paths`, sorted, at most MaxCardNros.
constexpr size_t MaxCardNros = 256;
std::vector<std::string> cardNroPaths(std::vector<std::string> paths);
// Every app file under <sdRoot>/switch, read; a file that is no .nro is left out.
std::vector<CardNro> scanCard(const std::string &sdRoot, const std::vector<int> &languages);
// The listed app a card file belongs to: the one installed at that path,
// else the one whose folder holds it (switch/<app>/), else the one whose file
// it is straight under switch/ (switch/<fileName>); nullptr when none.
const App *matchCard(const std::vector<App> &apps, const std::string &path);
// The picture an app's tile shows: the server's icon until it failed to
// load (a 404, a timeout), then `cardKey`, the icon in the app's file on the
// card, when there is one; else the server's, whose failure draws the
// name's initial. A card-only app goes straight to its file's icon.
std::string tileIcon(const App &app, bool serverFailed, const std::string &cardKey);
// For a listed app with no ledger entry but a file on the card: Installed
// when the versions agree ("v" in front ignored) or either is unknown.
Status cardStatus(const std::string &appVersion, const std::string &cardVersion);
// Deletes <sdRoot>/<path> and the folders it leaves empty, up to (not
// including) switch/. Only a path cardNroPath() lists is touched.
bool removeCardFile(const std::string &sdRoot, const std::string &path, std::string &error);
// The empty folders above <sdRoot>/<path>, up to (not including) switch/.
void removeEmptyFolders(const std::string &sdRoot, const std::string &path);
} // namespace foil::homebrew
