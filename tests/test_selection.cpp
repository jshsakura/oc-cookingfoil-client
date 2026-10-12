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
  return failures;
}
