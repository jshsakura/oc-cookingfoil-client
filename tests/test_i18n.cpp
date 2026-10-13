#include "Test.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include <filesystem>
#include <set>
#include <sstream>
namespace {
bool hangul(const std::string &text) {
  // Hangul syllables are U+AC00..U+D7A3: UTF-8 lead bytes 0xEA..0xED.
  for (unsigned char c : text)
    if (c >= 0xEA && c <= 0xED)
      return true;
  return false;
}
std::set<std::string> placeholders(const char *text) {
  std::set<std::string> out;
  const std::string s = text ? text : "";
  for (size_t at = s.find('{'); at != std::string::npos; at = s.find('{', at + 1)) {
    const auto end = s.find('}', at);
    if (end != std::string::npos && end - at <= 3)
      out.insert(s.substr(at, end - at + 1));
  }
  return out;
}
// String literals on one line, outside // comments, with what precedes each.
struct Literal {
  std::string text, before;
};
std::vector<Literal> literals(const std::string &line) {
  std::vector<Literal> out;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/')
      break;
    if (line[i] == '\'' && i + 2 < line.size()) { // skip '"' char literals
      i += line[i + 1] == '\\' ? 3 : 2;
      continue;
    }
    if (line[i] != '"')
      continue;
    const auto start = i;
    std::string text;
    for (++i; i < line.size() && line[i] != '"'; ++i) {
      if (line[i] == '\\' && i + 1 < line.size()) {
        const char next = line[++i];
        text += next == 'n' ? '\n' : next == 't' ? '\t' : next;
        continue;
      }
      text += line[i];
    }
    // R"(...)" raw strings (SVG icons) carry no screen text.
    if (start > 0 && line[start - 1] == 'R')
      continue;
    out.push_back({text, line.substr(0, start)});
  }
  return out;
}
bool wrapped(std::string before) {
  while (!before.empty() && before.back() == ' ')
    before.pop_back();
  auto ends = [&](const char *tail) {
    const std::string t = tail;
    return before.size() >= t.size() && before.compare(before.size() - t.size(), t.size(), t) == 0;
  };
  return ends("tr(") || ends("trf(");
}
} // namespace
int runI18nTests() {
  int failures = 0;
  namespace fs = std::filesystem;
  // Every table row is complete and keeps the same placeholders.
  size_t rows = 0;
  for (const auto *row : foil::allTranslations()) {
    ++rows;
    const bool complete = row->key && *row->key && row->ko && *row->ko && row->ja && *row->ja && row->zh && *row->zh;
    if (!complete)
      std::cerr << "incomplete translation: " << (row->key ? row->key : "?") << '\n';
    CHECK(complete);
    const auto keys = placeholders(foil::english(row->key));
    const bool same = placeholders(row->ko) == keys && placeholders(row->ja) == keys && placeholders(row->zh) == keys;
    if (!same)
      std::cerr << "placeholder mismatch: " << row->key << '\n';
    CHECK(same);
  }
  // One row per key across every table; a second row would be ignored.
  std::set<std::string> keys;
  for (const auto *row : foil::allTranslations())
    if (!keys.insert(row->key).second)
      std::cerr << "duplicate translation key: " << row->key << '\n';
  CHECK(keys.size() == rows);
  // Screen text in the code is an English key in tr()/trf() (or a key kept
  // for later, marked "i18n: keep") with a row in the tables; Korean lives
  // only in the tables.
  int unwrapped = 0, missing = 0;
  for (const auto *root : {"source", "include"}) {
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
      const auto path = entry.path().string();
      if (!entry.is_regular_file() || path.find("source/i18n/") != std::string::npos ||
          path.find("I18n.") != std::string::npos)
        continue;
      std::istringstream lines(foil::readFile(path));
      std::string line;
      int number = 0;
      while (std::getline(lines, line)) {
        ++number;
        // "i18n: as is": text shown or matched the same in every language.
        if (line.find("i18n: as is") != std::string::npos)
          continue;
        const bool kept = line.find("i18n: keep") != std::string::npos;
        for (const auto &literal : literals(line)) {
          if (!kept && !wrapped(literal.before)) {
            if (hangul(literal.text)) {
              std::cerr << path << ':' << number << ": Korean text outside the tables: " << literal.text << '\n';
              ++unwrapped;
            }
          } else if (!literal.text.empty() && !foil::findTranslation(literal.text)) {
            std::cerr << path << ':' << number << ": no translation: " << literal.text << '\n';
            ++missing;
          }
        }
      }
    }
  }
  CHECK(unwrapped == 0);
  CHECK(missing == 0);
  // Lookups follow the chosen language and fill placeholders after it.
  const foil::Translation sample[] = {{"Sample {0}|test", "테스트 {0}개", "テスト {0}", "测试 {0}"}};
  static const foil::TranslationTable table(sample, 1);
  (void)rows;
  foil::setLanguage(foil::Language::Japanese);
  CHECK(foil::trf("Sample {0}|test", {"3"}) == "テスト 3");
  CHECK(std::string(foil::tr("Not in any table|test")) == "Not in any table");
  foil::setLanguage(foil::Language::English);
  CHECK(foil::trf("Sample {0}|test", {"3"}) == "Sample 3");
  foil::setLanguage(foil::Language::Korean);
  CHECK(foil::trf("Sample {0}|test", {"3"}) == "테스트 3개");
  CHECK(foil::languageFromCode("zh", foil::Language::Korean) == foil::Language::Chinese);
  CHECK(foil::languageFromCode("xx", foil::Language::English) == foil::Language::English);
  return failures;
}
