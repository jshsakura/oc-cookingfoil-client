#pragma once
#include <cstdint>
#include <string>
namespace inst::offline {
struct TitleMetadata {
  std::string name, publisher;
};
inline bool TryGetMetadata(uint64_t, TitleMetadata &) { return false; }
} // namespace inst::offline
