#pragma once
#include "core/I18n.hpp"
#include <string>
inline std::string operator""_lang(const char *text, size_t size) {
  const std::string key(text, size);
  // The engine appends the NCA id or the cause after these.
  if (key == "inst.nca_verify.error")
    return std::string(foil::tr("콘텐츠 서명을 확인하지 못했습니다. 설정에서 서명 검사 무시를 켜면 설치할 수 있습니다.")) + " ";
  if (key == "inst.net.transfer_interput")
    return std::string(foil::tr("다운로드가 중단됐습니다.")) + " ";
  return key;
}
namespace Language {
inline std::string GetRemoteHeaderLanguage() { return "ko"; }
} // namespace Language
