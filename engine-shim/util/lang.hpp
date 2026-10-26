#pragma once
#include "core/I18n.hpp"
#include <string>
inline std::string operator""_lang(const char *text, size_t size) {
  const std::string key(text, size);
  // The engine appends the NCA id or the cause after these.
  if (key == "inst.nca_verify.error")
    return std::string(foil::tr("Could not verify the content signature, so it was not installed.")) + " ";
  if (key == "inst.net.transfer_interput")
    return std::string(foil::tr("The download stopped.")) + " ";
  return key;
}
namespace Language {
inline std::string GetRemoteHeaderLanguage() { return "ko"; }
} // namespace Language
