#include "install/Installer.hpp"
#include "core/I18n.hpp"
#ifndef __SWITCH__
#include <stdexcept>
namespace foil::install {
void nativeInstall(const Item &, const std::string &,
                   const std::vector<std::string> &) {
  throw std::runtime_error(tr("본체에서 설치할 수 있습니다."));
}
} // namespace foil::install
#endif
