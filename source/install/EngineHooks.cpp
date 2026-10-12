#include "install/EngineHooks.hpp"
#include "core/I18n.hpp"
namespace foil::install {
static std::atomic<Installer *> sink{nullptr};
void engineSink(Installer *value) { sink = value; }
Installer *engineSink() { return sink.load(); }
void reportBytes(const std::string &content, uint64_t received, double speed) {
  if (auto *s = engineSink()) {
    s->phase(tr("다운로드하고 있습니다."));
    s->bytes(content, received, speed);
  }
}
} // namespace foil::install
