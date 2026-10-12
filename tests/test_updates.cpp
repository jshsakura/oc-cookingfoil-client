#include "Test.hpp"
#include "core/Updates.hpp"
int runUpdatesTests() {
  int failures = 0;
  foil::Item base;
  base.titleId = base.baseId = "0100E5E01C098000";
  base.url = "https://host/base.nsz";
  auto update = base;
  update.titleId = "0100E5E01C098800";
  update.type = "update";
  update.version = 131072;
  update.url = "https://host/update.nsz";
  auto dlc = base;
  dlc.titleId = "0100E5E01C099001";
  dlc.type = "dlc";
  dlc.version = 2;
  dlc.url = "https://host/dlc.nsp";
  auto second = dlc;
  second.titleId = "0100E5E01C099002";
  second.version = 0;
  second.url = "https://host/dlc-second.nsp";
  const auto groups = foil::groupCatalog({dlc, update, base, second});
  const foil::InstalledSnapshot current{
      true, {{base.titleId, 0}, {update.titleId, 65536}, {dlc.titleId, 1}}};
  auto rows = foil::findUpdates(groups, current);
  CHECK(rows.size() == 3);
  if (rows.size() == 3) {
    CHECK(rows[0].item.titleId == update.titleId);
    CHECK(rows[0].installedVersion == 65536);
    CHECK(rows[1].item.titleId == dlc.titleId);
    CHECK(rows[1].installedVersion == 1);
    CHECK(rows[2].item.titleId == second.titleId);
    CHECK(!rows[2].installedVersion);
  }
  CHECK(foil::findUpdates(groups, {true, {}}).empty());
  CHECK(foil::findUpdates(groups, {}).empty());
  CHECK(foil::findUpdates(groups, {false, current.titles}).empty());
  CHECK(foil::findUpdates({}, current).empty());
  const foil::InstalledSnapshot latest{
      true, {{base.titleId, 0}, {update.titleId, 131072},
             {dlc.titleId, 2}, {second.titleId, 0}}};
  CHECK(foil::findUpdates(groups, latest).empty());
  const foil::InstalledSnapshot ahead{
      true, {{base.titleId, 0}, {update.titleId, 196608},
             {dlc.titleId, 3}, {second.titleId, 1}}};
  CHECK(foil::findUpdates(groups, ahead).empty());
  // A previously installed higher version on either storage blocks downgrade.
  auto duplicate = latest;
  duplicate.titles.insert(duplicate.titles.end(), current.titles.begin(),
                          current.titles.end());
  CHECK(foil::findUpdates(groups, duplicate).empty());
  auto lowercase = latest;
  lowercase.titles[0].titleId = "0100e5e01c098000";
  lowercase.titles[1].titleId = "0100e5e01c098800";
  lowercase.titles[2].titleId = "0100e5e01c099001";
  lowercase.titles[3].titleId = "0100e5e01c099002";
  CHECK(foil::findUpdates(groups, lowercase).empty());
  // Server groups need not contain the base file if the base is installed.
  CHECK(foil::findUpdates(foil::groupCatalog({update, dlc}), current).size() == 2);
  // Patch/DLC records alone do not mean the base is installed.
  CHECK(foil::findUpdates(groups,
                         {true, {{update.titleId, 65536}, {dlc.titleId, 1}}})
            .empty());
  CHECK(foil::findUpdates(groups, {true, {{"0100000000000000", 0}}}).empty());
  // An absent patch compares against the installed base's content version.
  CHECK(foil::findUpdates(foil::groupCatalog({update}),
                         {true, {{base.titleId, 0}}}).size() == 1);
  CHECK(foil::findUpdates(foil::groupCatalog({update}),
                         {true, {{base.titleId, 131072}}}).empty());
  auto unidentified = dlc;
  unidentified.titleId.clear();
  CHECK(foil::findUpdates(foil::groupCatalog({unidentified}), current).empty());
  auto old = update;
  old.url = "https://host/update-old.nsz";
  old.version = 65536;
  auto oldDlc = dlc;
  oldDlc.url = "https://host/dlc-old.nsp";
  oldDlc.version = 1;
  rows = foil::findUpdates(foil::groupCatalog({old, oldDlc, update, dlc}), current);
  CHECK(rows.size() == 2);
  if (rows.size() == 2)
    CHECK(rows[0].item.url == update.url && rows[1].item.url == dlc.url);
  CHECK(current.titles[1].version == 65536);
  CHECK(groups.front().update->version == 131072);
  return failures;
}
