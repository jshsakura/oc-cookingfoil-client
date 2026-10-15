#pragma once
#include "core/Catalog.hpp"
#include "core/Patches.hpp"
#include <map>
namespace foil {
// What the detail page installs: the game's files, then its patches as rows
// after them. Rows count files first (parts()), then patches (patch()).
class Selection {
public:
  explicit Selection(const Group &group, std::string target = "sd");
  // A row's A: a file or a patch on or off; on the base game's row, the
  // next of its files when the game has more than one (smallest first).
  bool toggle(size_t row);
  // How many files the base game can come from, and which one is chosen
  // (0, the smallest, until changed).
  size_t baseChoices() const { return bases_.size(); }
  size_t baseChoice() const { return baseAt_; }
  const std::vector<Item> &baseFiles() const { return bases_; }
  bool enabled(size_t row) const;
  // Files and patches that are on, in bytes.
  uint64_t total() const;
  // The files to install; patches that are on come from patchesOn().
  std::vector<Item> files() const;
  const std::vector<Item> &parts() const { return parts_; }
  // The game's patches as rows: a patch the game needs starts on, the rest
  // off. Given again (the detail read anew), a patch keeps its choice. Only
  // patches for the version being installed are rows (patchFitsVersion).
  void setPatches(std::vector<UserPatch> patches);
  size_t rows() const { return parts_.size() + patches_.size(); }
  // The patch on a row, or nullptr for a file's row.
  const UserPatch *patch(size_t row) const;
  std::vector<UserPatch> patchesOn() const;
  // Files and patches that are on.
  size_t count() const;
  // The game version the chosen files leave installed: the update's when it
  // is on, else the base game's; empty when unknown.
  std::string installedVersion() const;
  std::string target;

private:
  void showPatches();
  std::vector<Item> parts_, bases_;
  size_t baseAt_ = 0;
  std::vector<bool> selected_;
  std::vector<UserPatch> offered_, patches_; // every patch, the ones shown
  std::map<std::string, bool> patchOn_;      // by patch id, kept while hidden
};
} // namespace foil
