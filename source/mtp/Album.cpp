#include "mtp/Mtp.hpp"
#include <sstream>
namespace foil::mtp {
namespace {
std::string trim(const std::string &text) {
  const auto first = text.find_first_not_of(" \t\r");
  if (first == std::string::npos) return "";
  return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}
// "emuMMC/RAW1/Nintendo/" and "/emuMMC/RAW1/Nintendo" alike.
std::string rooted(std::string path) {
  while (!path.empty() && path.back() == '/') path.pop_back();
  return path.empty() || path.front() == '/' ? path : "/" + path;
}
} // namespace
std::string nintendoFolder(bool emummc, const std::string &ini) {
  if (!emummc) return "/Nintendo";
  std::istringstream lines(ini);
  std::string line, section, path, nintendo, id;
  while (std::getline(lines, line)) {
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[') {
      section = trim(line.substr(1, line.find(']') - 1));
      continue;
    }
    const auto eq = line.find('=');
    if (section != "emummc" || eq == std::string::npos) continue;
    const auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    if (key == "nintendo_path") nintendo = value;
    else if (key == "path") path = value;
  }
  // hekate always writes nintendo_path; an older file-based one has only
  // its folder, the Nintendo folder inside it.
  if (!nintendo.empty()) return rooted(nintendo);
  if (!path.empty()) return rooted(path) + "/Nintendo";
  return "/emuMMC/RAW1/Nintendo";
}
} // namespace foil::mtp
