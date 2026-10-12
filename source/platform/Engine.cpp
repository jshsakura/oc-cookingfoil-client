#ifdef __SWITCH__
#include "core/Config.hpp"
#include "core/I18n.hpp"
#include "install/EngineHooks.hpp"
#include "install/Installer.hpp"
#include "install/http_nsp.hpp"
#include "install/http_xci.hpp"
#include "install/install_nsp.hpp"
#include "install/install_xci.hpp"
#include "nx/ipc/es.h"
#include "nx/ipc/ns_ext.h"
#include "ui/MainApplication.hpp"
#include "util/config.hpp"
#include "util/network_util.hpp"
#include "data/buffered_placeholder_writer.hpp"
#include <curl/curl.h>
#include <stdexcept>
namespace {
// 256MB and 32MB of download buffer.
constexpr int ApplicationSegments = 32, AppletSegments = 4;
} // namespace
namespace tin::network {
void SetExtraHeaders(const std::vector<std::string> &headers,
                     const std::string &origin);
}
namespace inst::ui {
static MainApplication adapter;
MainApplication *mainApp = &adapter;
} // namespace inst::ui
namespace inst::util {
void playAudio(std::string) {}
std::string formatUrlString(std::string text) {
  auto query = text.find_first_of("?#");
  if (query != std::string::npos)
    text.erase(query);
  auto slash = text.find_last_of('/');
  if (slash != std::string::npos)
    text.erase(0, slash + 1);
  int size = 0;
  char *decoded = curl_easy_unescape(nullptr, text.c_str(), text.size(), &size);
  if (!decoded)
    return text;
  std::string result(decoded, size);
  curl_free(decoded);
  return result;
}
} // namespace inst::util
namespace foil::install {
class Services {
public:
  Services() {
    check(ncmInitialize(), 0);
    check(nsextInitialize(), 1);
    check(esInitialize(), 2);
    check(splCryptoInitialize(), 3);
    check(splInitialize(), 4);
  }
  ~Services() { close(); }

private:
  int count_ = 0;
  void check(Result rc, int stage) {
    if (R_FAILED(rc)) {
      close();
      throw std::runtime_error(foil::trf("설치 서비스를 열지 못했습니다. 코드 {0} / {1}", {std::to_string(rc), std::to_string(stage)}));
    }
    ++count_;
  }
  void close() {
    if (count_ > 4)
      splExit();
    if (count_ > 3)
      splCryptoExit();
    if (count_ > 2)
      esExit();
    if (count_ > 1)
      nsextExit();
    if (count_ > 0)
      ncmExit();
    count_ = 0;
  }
};
void nativeInstall(const Item &item, const std::string &target,
                   const std::vector<std::string> &headers) {
  Services services;
  inst::ui::mainApp->ResetRetries();
  // With the check on, an NCA whose signature fails stops the install; with
  // it off (설정), the engine installs it as is and sigpatches let it run.
  auto *sink = foil::install::engineSink();
  inst::config::validateNCAs = !(sink && sink->skipSignatureCheck());
  // The download ring between curl and the placeholder writer, in 8MB
  // segments. CyberFoil sets this from its own menu, which is not built here;
  // left at 0 no data could ever be buffered and every download stalled.
  // Applet mode has a few hundred MB in all, so it gets a small ring.
  tin::data::NUM_BUFFER_SEGMENTS =
      appletGetAppletType() == AppletType_Application ? ApplicationSegments : AppletSegments;
  // Off in 설정: the required system version is cleared so the console
  // launches the game anyway (it may still fail on firmware that is too old).
  const bool ignoreFirmware = sink && sink->skipFirmwareCheck();
  tin::network::ClearBasicAuth();
  tin::network::SetExtraHeaders(headers, originOf(item.url));
  struct Headers {
    ~Headers() { tin::network::SetExtraHeaders({}, ""); }
  } clear;
  const auto storage =
      target == "nand" ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
  auto path = item.url.substr(0, item.url.find_first_of("?#"));
  for (auto &c : path)
    c = std::tolower(static_cast<unsigned char>(c));
  if (path.size() >= 4 && (path.substr(path.size() - 4) == ".xci" ||
                           path.substr(path.size() - 4) == ".xcz")) {
    auto file = std::make_shared<tin::install::xci::HTTPXCI>(item.url);
    tin::install::xci::XCIInstallTask task(storage, ignoreFirmware, file);
    task.Prepare();
    task.Begin();
  } else {
    auto file = std::make_shared<tin::install::nsp::HTTPNSP>(item.url);
    tin::install::nsp::NSPInstall task(storage, ignoreFirmware, file);
    task.Prepare();
    task.Begin();
  }
}
} // namespace foil::install
#endif
