#pragma once
#include "core/I18n.hpp"
#include <string>
inline std::string operator""_lang(const char *text, size_t size) {
  const std::string key(text, size);
  // The engine appends the cause after it.
  if (key == "inst.net.transfer_interput")
    return std::string(foil::tr("The download stopped.")) + " ";
  return key;
}
namespace Language {
inline std::string GetRemoteHeaderLanguage() { return "ko"; }
} // namespace Language
