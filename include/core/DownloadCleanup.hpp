#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace foil {
std::vector<std::pair<std::string, uint64_t>> downloadPieces(const std::string &sd);
}
