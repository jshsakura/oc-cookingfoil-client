#include "ui/Runtime.hpp"
#include "ui/Theme.hpp"
#include "core/State.hpp"
#include <SDL2/SDL_image.h>
#include <cstdio>
int runConfigTests();
int runOrderedTests();
int runDownloadCleanupTests();
int runMacJunkTests();
int runNetworkTests();
int runNtpTests();
int runStateTests();
int runRebindTests();
int runSessionTokenTests();
int runVaultTests();
int runEnrollTests();
int runSessionTests();
int runCatalogTests();
int runUpdatesTests();
int runInstalledTests();
int runPairingTests();
int runPairingUiTests(SDL_Renderer *renderer);
int runSelectionTests();
int runInstallerTests();
int runPatchTests();
int runI18nTests();
int runSaveMetaTests();
int runZipTests();
int runDropboxUtilTests();
int runDropboxRowsTests();
int runAchievementsTests();
int runSaveBackupTests();
int runRetroTests();
int runLabelsTests();
int runHistoryTests();
int runStoreTests();
int runZstdResumeTests();
int runNczPointTests();
int runClientUpdateTests();
int runHomebrewTests();
int runHomebrewCardTests();
int runNacpTests();
int runResumeTests();
int runForwarderTests();
int runDirectTests();
int runCardPatchTests();
int runTunerTests();
int runAdminEditTests();
int runVotesTests();
int runSendsTests();
int runMtpTests();
int runClientStringsTests();
int runOriginTests();
int runOriginRecordTests();
int runMergeCatalogTests();
int runSegmentedTests();
int runShortcutUiTests(SDL_Renderer *renderer);
int runSendsUiTests(SDL_Renderer *renderer);
int runUiTests(SDL_Renderer *renderer);
int main() {
  foil::ui::Runtime app;
  if (!app.open(1920, 1080, true))
    return 1;
  app.render();
  char *directory = SDL_GetPrefPath("OpenCourse", "CookingFoil");
  if (!directory)
    return 1;
  const auto path = std::string(directory) + "host-smoke.png";
  SDL_free(directory);
  if (!app.screenshot(path))
    return 1;
  auto *image = IMG_Load(path.c_str());
  if (!image || image->w != 1920 || image->h != 1080)
    return 1;
  auto *pixels = static_cast<Uint32 *>(image->pixels);
  Uint8 r, g, b;
  SDL_GetRGB(pixels[0], image->format, &r, &g, &b);
  const bool ok = r == foil::ui::Ground.r && g == foil::ui::Ground.g &&
                  b == foil::ui::Ground.b;
  SDL_FreeSurface(image);
  std::remove(path.c_str());
  std::puts(ok ? "Host screenshot smoke passed"
               : "Host screenshot smoke failed");
  // A console that reads its ID, as on the Switch (test_state covers one
  // that cannot).
  foil::setDeviceIdentity(foil::deriveIdentity(0x756e697474657374ull));
  const int failures = runDownloadCleanupTests() + runMacJunkTests() + runOrderedTests() + runConfigTests() + runNetworkTests() + runNtpTests() + runStateTests() + runRebindTests() + runSessionTokenTests() + runVaultTests() + runEnrollTests() +
                       runSessionTests() + runPairingTests() + runCatalogTests() + runUpdatesTests() + runInstalledTests() +
                       runSelectionTests() + runInstallerTests() + runPatchTests() + runI18nTests() +
                       runSaveMetaTests() + runZipTests() + runDropboxUtilTests() + runSaveBackupTests() + runRetroTests() + runLabelsTests() + runHistoryTests() + runStoreTests() + runZstdResumeTests() + runNczPointTests() + runClientUpdateTests() + runHomebrewTests() + runHomebrewCardTests() + runNacpTests() + runDropboxRowsTests() + runAchievementsTests() + runResumeTests() + runForwarderTests() + runDirectTests() + runCardPatchTests() + runTunerTests() + runSegmentedTests() + runAdminEditTests() + runVotesTests() + runSendsTests() + runMtpTests() + runClientStringsTests() + runOriginTests() + runOriginRecordTests() + runMergeCatalogTests() +
                       runUiTests(app.renderer()) + runPairingUiTests(app.renderer()) + runShortcutUiTests(app.renderer()) +
                       runSendsUiTests(app.renderer());
  std::printf("Core tests: %d failures\n", failures);
  return (ok && failures == 0) ? 0 : 1;
}
