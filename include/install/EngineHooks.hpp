#pragma once
#include "install/Installer.hpp"
namespace foil::install {
void engineSink(Installer *sink);
Installer *engineSink();
void reportBytes(const std::string &content, uint64_t received, double speed);
} // namespace foil::install
