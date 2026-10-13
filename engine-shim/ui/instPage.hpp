#pragma once
#include "install/EngineHooks.hpp"
namespace inst::ui {
struct instPage {
  static bool isInstallCancelRequested() {
    auto *sink = foil::install::engineSink();
    return sink && sink->cancellationRequested();
  }
  // Paused from 다운로드: the engine drops its connections and waits.
  static bool isInstallPaused() {
    auto *sink = foil::install::engineSink();
    return sink && sink->paused();
  }
  static void setInstBarPerc(double percent) {
    if (auto *sink = foil::install::engineSink())
      sink->stagePercent(percent);
  }
  static void setInstInfoText(const std::string &) {}
  static void setTopInstInfoText(const std::string &) {}
  static void setProgressDetailText(const std::string &text) {
    if (auto *sink = foil::install::engineSink())
      if (text.rfind("Installing", 0) == 0 ||
          text.find("Verifying") != std::string::npos)
        sink->phase(foil::tr("콘텐츠를 확인하고 설치하고 있습니다."));
  }
};
} // namespace inst::ui
