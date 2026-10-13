#include "install/Installer.hpp"
#include "core/I18n.hpp"
#ifndef __SWITCH__
#include <functional>
#include <mutex>
#include <stdexcept>
namespace foil::install {
namespace {
std::mutex hostLock;
std::function<void(const Item &)> hostInstall;
} // namespace
// Off the console an install does what a test says (wait, then succeed or
// throw); with nothing set it fails, as there is nothing to install to.
void setHostInstall(std::function<void(const Item &)> run) {
  std::lock_guard<std::mutex> held(hostLock);
  hostInstall = std::move(run);
}
void nativeInstall(const Item &item, const std::string &,
                   const std::vector<std::string> &) {
  std::function<void(const Item &)> run;
  {
    std::lock_guard<std::mutex> held(hostLock);
    run = hostInstall;
  }
  if (!run)
    throw std::runtime_error(tr("Installing works on the console."));
  run(item);
}
} // namespace foil::install
#endif
