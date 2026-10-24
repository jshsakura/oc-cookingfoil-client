#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace foil {
std::vector<std::pair<std::string, uint64_t>> downloadPieces(const std::string &sd);
struct DownloadCleanupResult {
  uint64_t bytes = 0;
  std::string error;
};
DownloadCleanupResult cleanDownloadPieces(const std::vector<std::pair<std::string, uint64_t>> &pieces);
}
