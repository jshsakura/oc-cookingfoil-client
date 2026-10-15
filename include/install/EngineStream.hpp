#pragma once
// For the engine's NSP and XCI installs only (it needs the engine's types):
// one NCA from `download` at [ncaStart, ncaStart + ncaSize) into its
// placeholder, an NCZ decompressed as it comes, resumed from its last point
// when an earlier try was cut off (source/platform/EngineStream.cpp).
#include <memory>
#include <string>
#include <switch.h>
#include "nx/ncm.hpp"
#include "util/network_util.hpp"
namespace foil::install {
void streamNcz(tin::network::HTTPDownload& download, std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
               NcmContentId placeholderId, const std::string& displayFileName, u64 ncaStart, size_t ncaSize);
}
