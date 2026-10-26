#pragma once
// How the engine's downloads of the file being installed go: its
// connections, piece size and headers (pause and cancel come from the
// engine's sink).
#include "net/Ordered.hpp"
namespace foil::install {
void setEngineDownload(net::OrderedOptions options);
void clearEngineDownload();
} // namespace foil::install
