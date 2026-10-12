#include "Test.hpp"
#include "core/Selection.hpp"
int runSelectionTests() {
  int failures = 0;
  foil::Group group;
  foil::Item base;
  base.type = "base";
  base.size = 4402341888ULL;
  group.base = base;
  auto update = base;
  update.type = "update";
  update.size = 1395864371;
  group.update = update;
  auto dlc = base;
  dlc.type = "dlc";
  dlc.size = 1395864371;
  group.dlc.push_back(dlc);
  dlc.size = 966367641;
  group.dlc.push_back(dlc);
  foil::Selection selection(group);
  CHECK(selection.parts().size() == 4);
  CHECK(selection.total() == 8160438271ULL);
  CHECK(!selection.toggle(0));
  CHECK(selection.enabled(0));
  CHECK(selection.toggle(3));
  CHECK(selection.total() == 7194070630ULL);
  CHECK(selection.files().size() == 3);
  CHECK(!selection.toggle(90));
  selection.target = "nand";
  CHECK(selection.target == "nand");
  group.base.reset();
  foil::Selection withoutBase(group);
  CHECK(withoutBase.toggle(0));
  CHECK(withoutBase.files().size() == 2);
  {
    // A game's patches follow its files: the one it needs starts on, the
    // other off; both count in the total and the item count once on.
    foil::Group game;
    game.base = base;
    game.update = update;
    foil::Selection withPatches(game);
    const auto files = withPatches.total();
    CHECK(withPatches.rows() == 2);
    foil::UserPatch optional, required;
    optional.id = "font";
    optional.size = 1000;
    required.id = "boot";
    required.size = 300;
    required.required = true;
    withPatches.setPatches({optional, required});
    CHECK(withPatches.rows() == 4);
    CHECK(!withPatches.enabled(2));
    CHECK(withPatches.enabled(3));
    CHECK(withPatches.patch(1) == nullptr);
    CHECK(withPatches.patch(2) && withPatches.patch(2)->id == "font");
    CHECK(withPatches.total() == files + 300);
    CHECK(withPatches.count() == 3);
    CHECK(withPatches.files().size() == 2); // patches never reach the installer
    CHECK(withPatches.toggle(2));
    CHECK(withPatches.total() == files + 1300);
    CHECK(withPatches.toggle(3)); // a needed patch can still be left out
    const auto on = withPatches.patchesOn();
    CHECK(on.size() == 1 && on.front().id == "font");
    CHECK(withPatches.count() == 3);
    CHECK(!withPatches.toggle(4));
    // The detail read again keeps what the user chose; a new patch starts
    // by its own rule.
    foil::UserPatch extra;
    extra.id = "music";
    withPatches.setPatches({optional, required, extra});
    CHECK(withPatches.enabled(2) && !withPatches.enabled(3) && !withPatches.enabled(4));
  }
  {
    // Rows show only patches for the version that ends up installed: the
    // update's while it is on, the base game's once it is off.
    foil::Group game;
    auto base10 = base, update12 = update;
    base10.versionName = "1.0.0";
    update12.versionName = "v1.2.0";
    game.base = base10;
    game.update = update12;
    foil::Selection versions(game);
    CHECK(versions.installedVersion() == "v1.2.0");
    foil::UserPatch any, forUpdate, forBase;
    any.id = "any";
    forUpdate.id = "new";
    forUpdate.gameVersion = "1.2.0";
    forUpdate.required = true;
    forUpdate.size = 5;
    forBase.id = "old";
    forBase.gameVersion = "1.0.0";
    versions.setPatches({any, forUpdate, forBase});
    CHECK(versions.rows() == 4);
    CHECK(versions.patch(3) && versions.patch(3)->id == "new");
    CHECK(versions.patchesOn().size() == 1);
    CHECK(versions.toggle(1)); // update off: the base game's version
    CHECK(versions.installedVersion() == "1.0.0");
    CHECK(versions.rows() == 4);
    CHECK(versions.patch(3) && versions.patch(3)->id == "old");
    CHECK(versions.patchesOn().empty()); // a hidden patch is not installed
    CHECK(versions.toggle(1)); // and on again, its choice kept
    CHECK(versions.patchesOn().size() == 1);
    // No version known anywhere: every patch is a row.
    foil::Group bare;
    bare.base = base;
    foil::Selection unknown(bare);
    unknown.setPatches({any, forUpdate, forBase});
    CHECK(unknown.rows() == 4);
  }
  return failures;
}
