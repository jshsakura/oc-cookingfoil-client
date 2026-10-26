#pragma once
// What the engine's NSP and XCI readers ask of a download, served by the
// app's own (net/Ordered.hpp, source/platform/EngineDownload.cpp) in place
// of CyberFoil's util/network_util.
#include <switch/types.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace tin::network {
class HTTPDownload {
public:
  explicit HTTPDownload(std::string url);
  // A split file on several addresses; none are made here.
  bool IsJbod() const { return false; }
  // [offset, offset + size) into buffer; throws when it cannot.
  void BufferDataRange(void *buffer, size_t offset, size_t size, std::function<void(size_t sizeRead)> progressFunc);
  // The range to streamFunc in order: 0 when all of it went, else 1.
  // `retryConfirmFunc`, asked once the tries run out, true goes on.
  int StreamDataRange(size_t offset, size_t size, std::function<size_t(u8 *bytes, size_t size)> streamFunc,
                      std::function<bool()> retryConfirmFunc = nullptr);

private:
  std::string m_url;
};
} // namespace tin::network
