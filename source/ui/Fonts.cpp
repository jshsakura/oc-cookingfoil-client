#include "ui/Fonts.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include "core/Json.hpp"
#ifdef __SWITCH__
#include <switch.h>
#include <zstd.h>
#endif
namespace foil::ui {
std::vector<std::string> utf8Characters(const std::string &text) {
  std::vector<std::string> result;
  for (size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    size_t n = c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4;
    n = std::min(n, text.size() - i);
    result.push_back(text.substr(i, n));
    i += n;
  }
  return result;
}
static uint32_t codepoint(const std::string &s) {
  const auto c = static_cast<unsigned char>(s[0]);
  uint32_t cp = c < 128 ? c : c < 224 ? c & 31 : c < 240 ? c & 15 : c & 7;
  for (size_t i = 1; i < s.size(); ++i)
    cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 63);
  return cp;
}
Fonts::Fonts(SDL_Renderer *renderer) : renderer_(renderer) {
  int w, h;
  SDL_GetRendererOutputSize(renderer, &w, &h);
  scale_ = w / 1280.f;
}
Fonts::~Fonts() {
  clear();
  for (auto *f : probes_)
    if (f)
      TTF_CloseFont(f);
#ifdef __SWITCH__
  if (shared_)
    plExit();
#endif
}
// Font sources in lookup order: the bundled subsets (Korean/Latin, then
// Japanese and Chinese, Chinese first for a Chinese interface), then the
// console's shared fonts for anything rarer. Nothing is opened or unpacked
// until a glyph actually needs it, so a Korean screen never pays for the
// CJK subsets and each text size holds only the faces it draws with.
namespace {
constexpr int Bundled = 3;
#ifdef __SWITCH__
const PlSharedFontType SharedTypes[] = {PlSharedFontType_Standard, PlSharedFontType_KO,
                                        PlSharedFontType_ChineseSimplified,
                                        PlSharedFontType_ChineseTraditional};
constexpr int Sources = Bundled + 4;
#else
const char *const HostFallbacks[] = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
constexpr int Sources = Bundled + 2;
#endif
const char *const FamilyFiles[] = {"CookingFoilSans", "CookingFoilSansJP", "CookingFoilSansSC"};
std::string familyFile(int family, bool bold) {
  return std::string(FamilyFiles[family]) + (bold ? "-Bold." : "-Regular.") + (family == 2 ? "ttf" : "otf");
}
} // namespace
int Fonts::family(int source) const {
  if (source == 0)
    return 0;
  return (source == 1) != chinese_ ? 1 : 2;
}
#ifdef __SWITCH__
// The bundled font data, unpacked from RomFS the first time it is used.
const std::vector<char> *Fonts::bundled(int family, bool bold) {
  const int index = family * 2 + (bold ? 1 : 0);
  auto &data = embedded_[index];
  if (!data.empty())
    return &data;
  if (unpacked_[index])
    return nullptr; // tried and failed
  unpacked_[index] = true;
  const auto compressed = readFile("romfs:/fonts/" + familyFile(family, bold) + ".zst");
  const auto size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > 8 * 1024 * 1024)
    return nullptr;
  data.resize(size);
  const auto decoded = ZSTD_decompress(data.data(), size, compressed.data(), compressed.size());
  if (ZSTD_isError(decoded) || decoded != size) {
    std::vector<char>().swap(data);
    return nullptr;
  }
  return &data;
}
#endif
TTF_Font *Fonts::load(int source, int pixels, bool bold) {
#ifdef __SWITCH__
  if (source < Bundled) {
    const auto *data = bundled(family(source), bold);
    return data ? TTF_OpenFontRW(SDL_RWFromConstMem(data->data(), data->size()), 1, pixels) : nullptr;
  }
  if (!shared_)
    return nullptr;
  PlFontData data{};
  if (R_FAILED(plGetSharedFontByType(&data, SharedTypes[source - Bundled])))
    return nullptr;
  return TTF_OpenFontRW(SDL_RWFromConstMem(data.address, data.size), 1, pixels);
#else
  const auto path = source < Bundled ? "host/fonts/" + familyFile(family(source), bold)
                                     : std::string(HostFallbacks[source - Bundled]);
  return std::filesystem::exists(path) ? TTF_OpenFont(path.c_str(), pixels) : nullptr;
#endif
}
// One small regular face per source answers "does it have this glyph";
// coverage is the same at every size and weight.
bool Fonts::covers(int source, uint32_t cp) {
  if (probes_.empty())
    probes_.assign(Sources, nullptr), probed_.assign(Sources, false);
  if (!probed_[source]) {
    probed_[source] = true;
    probes_[source] = load(source, 12, false);
  }
  return probes_[source] && TTF_GlyphIsProvided32(probes_[source], cp);
}
TTF_Font *Fonts::face(int size, bool bold, int source) {
  auto &slots = fonts_[{size, bold}];
  if (slots.fonts.empty())
    slots.fonts.assign(Sources, nullptr), slots.tried.assign(Sources, false);
  if (!slots.tried[source]) {
    slots.tried[source] = true;
    slots.fonts[source] = load(source, std::lround(size * scale_), bold);
  }
  return slots.fonts[source];
}
bool Fonts::open() {
#ifdef __SWITCH__
  shared_ = R_SUCCEEDED(plInitialize(PlServiceType_User));
  if (!shared_)
    return false;
#endif
  return face(18, false, 0) != nullptr; // the main font is required
}
void Fonts::clear() {
  for (const auto &pair : labels_)
    SDL_DestroyTexture(pair.second.texture);
  labels_.clear();
  for (auto &pair : fonts_)
    for (auto *f : pair.second.fonts)
      if (f)
        TTF_CloseFont(f);
  fonts_.clear();
  runs_.clear();
  widths_.clear();
  fitted_.clear();
  layouts_.clear();
}
void Fonts::preferChinese(bool chinese) {
  if (chinese == chinese_)
    return;
  chinese_ = chinese;
  clear();
  // Probes follow source order too: sources 1 and 2 swap families.
  for (auto *f : probes_)
    if (f)
      TTF_CloseFont(f);
  probes_.clear();
  probed_.clear();
}
// Layout coordinates were tuned with IBM Plex Sans KR, whose ascent is 1.085em.
// Every run sits on that baseline, whatever font draws it.
constexpr float LayoutAscentEm = 1.085f;
template <typename Map> static void bound(Map &map) {
  if (map.size() > 4096)
    map.clear();
}
static std::string cacheKey(const std::string &text, int size, bool bold,
                            float extra = 0, int lines = 0) {
  return std::to_string(size) + (bold ? "b" : "r") + std::to_string(extra) +
         ":" + std::to_string(lines) + ":" + text;
}
const std::vector<Fonts::Run> &Fonts::runs(const std::string &text, int size,
                                           bool bold) {
  const auto key = cacheKey(text, size, bold);
  auto found = runs_.find(key);
  if (found != runs_.end())
    return found->second;
  std::vector<Run> result;
  auto *main = face(size, bold, 0);
  if (!main)
    return runs_.emplace(key, std::move(result)).first->second;
  for (const auto &s : utf8Characters(text)) {
    const auto cp = codepoint(s);
    // Combining accents (e + U+0301) stay with the letter they modify, or they
    // render as a separate glyph from another font.
    const bool combining = (cp >= 0x0300 && cp <= 0x036F) ||
                           (cp >= 0x1AB0 && cp <= 0x1AFF) ||
                           (cp >= 0x20D0 && cp <= 0x20FF);
    if (combining && !result.empty()) {
      result.back().text += s;
      continue;
    }
    TTF_Font *font = main;
    if (!TTF_GlyphIsProvided32(main, cp))
      for (int source = 1; source < Sources; ++source)
        if (covers(source, cp)) {
          if (auto *found = face(size, bold, source))
            font = found;
          break;
        }
    if (result.empty() || result.back().font != font)
      result.push_back({font, {}});
    result.back().text += s;
  }
  bound(runs_);
  return runs_.emplace(key, std::move(result)).first->second;
}
bool Fonts::supports(uint32_t cp, int size) {
  if (auto *main = face(size, false, 0); main && TTF_GlyphIsProvided32(main, cp))
    return true;
  for (int source = 1; source < Sources; ++source)
    if (covers(source, cp))
      return true;
  return false;
}
float Fonts::centerTop(const std::string &text, int size, bool bold,
                       float centerY) {
  const auto &list = runs(text, size, bold);
  if (list.empty())
    return centerY;
  int top = 0, bottom = 0;
  bool any = false;
  for (const auto &s : utf8Characters(text)) {
    int minx, maxx, miny, maxy, advance;
    if (TTF_GlyphMetrics32(list.front().font, codepoint(s), &minx, &maxx, &miny, &maxy, &advance) != 0)
      continue;
    top = any ? std::max(top, maxy) : maxy;
    bottom = any ? std::min(bottom, miny) : miny;
    any = true;
  }
  // text() puts the baseline at y + size * LayoutAscentEm.
  const float ink = any ? (top + bottom) / 2.f / scale_ : size * 0.35f;
  return centerY + ink - size * LayoutAscentEm;
}
float Fonts::middle(const std::string &text, float x, float centerY, int size,
                     SDL_Color color, bool bold, float maxWidth) {
  return this->text(text, x, centerTop(text, size, bold, centerY), size, color,
                    bold, maxWidth);
}
float Fonts::measure(const std::string &text, int size, bool bold) {
  const auto key = cacheKey(text, size, bold);
  auto found = widths_.find(key);
  if (found != widths_.end())
    return found->second;
  float width = 0;
  for (const auto &r : runs(text, size, bold)) {
    int w = 0;
    TTF_SizeUTF8(r.font, r.text.c_str(), &w, nullptr);
    width += w / scale_;
  }
  bound(widths_);
  widths_.emplace(key, width);
  return width;
}
std::string Fonts::fit(const std::string &text, int size, bool bold,
                       float maxWidth) {
  if (maxWidth <= 0 || measure(text, size, bold) <= maxWidth)
    return text;
  const auto key = cacheKey(text, size, bold, maxWidth);
  auto found = fitted_.find(key);
  if (found != fitted_.end())
    return found->second;
  std::string result;
  for (const auto &c : utf8Characters(text)) {
    if (measure(result + c + "…", size, bold) > maxWidth)
      break;
    result += c;
  }
  result += "…";
  bound(fitted_);
  fitted_.emplace(key, result);
  return result;
}
float Fonts::text(const std::string &original, float x, float y, int size,
                  SDL_Color color, bool bold, float maxWidth) {
  const auto text = fit(original, size, bold, maxWidth);
  for (const auto &r : runs(text, size, bold)) {
    const auto key = std::to_string(reinterpret_cast<uintptr_t>(r.font)) + ":" +
                     r.text + ":" + std::to_string(color.r) + "," +
                     std::to_string(color.g) + "," + std::to_string(color.b) +
                     "," + std::to_string(color.a);
    auto found = labels_.find(key);
    if (found == labels_.end()) {
      // Text textures are the biggest GPU-side cache; past the cap the least
      // recently drawn half goes, so what is on screen is never rebuilt.
      constexpr size_t MaxLabels = 256;
      if (labels_.size() >= MaxLabels) {
        std::vector<std::pair<uint64_t, std::string>> byAge;
        for (const auto &pair : labels_)
          byAge.push_back({pair.second.used, pair.first});
        std::sort(byAge.begin(), byAge.end());
        for (size_t n = 0; n < byAge.size() / 2; ++n) {
          const auto old = labels_.find(byAge[n].second);
          SDL_DestroyTexture(old->second.texture);
          labels_.erase(old);
        }
      }
      auto *surface = TTF_RenderUTF8_Blended(r.font, r.text.c_str(), color);
      if (!surface)
        continue;
      auto *texture = SDL_CreateTextureFromSurface(renderer_, surface);
      Label label{texture, surface->w / scale_, surface->h / scale_, 0};
      SDL_FreeSurface(surface);
      found = labels_.emplace(key, label).first;
    }
    found->second.used = ++tick_;
    const auto &label = found->second;
    const float top =
        y + size * LayoutAscentEm - TTF_FontAscent(r.font) / scale_;
    // Nearest sampling at a fractional pixel drops or doubles glyph columns.
    const SDL_FRect dest{std::round(x * scale_) / scale_,
                         std::round(top * scale_) / scale_, label.width,
                         label.height};
    SDL_RenderCopyF(renderer_, label.texture, nullptr, &dest);
    x += label.width;
  }
  return x;
}
const std::vector<std::string> &Fonts::layout(const std::string &text,
                                               int size, bool bold,
                                               float width, int lines) {
  const auto key = cacheKey(text, size, bold, width, lines);
  auto found = layouts_.find(key);
  if (found != layouts_.end())
    return found->second;
  std::vector<std::string> result;
  const auto chars = utf8Characters(text);
  std::string line;
  for (size_t i = 0; i < chars.size(); ++i) {
    const auto &c = chars[i];
    if (c == "\n" || (!line.empty() && measure(line + c, size, bold) > width)) {
      if (int(result.size()) + 1 >= lines) {
        line += "…";
        break;
      }
      const auto space = line.find_last_of(' ');
      std::string rest;
      if (c != "\n" && space != std::string::npos && space > 0) {
        rest = line.substr(space + 1);
        line.erase(space);
      }
      result.push_back(line);
      line = std::move(rest);
      if (c == "\n")
        continue;
    }
    line += c;
  }
  if (!line.empty())
    result.push_back(line);
  if (result.empty())
    result.push_back("");
  bound(layouts_);
  return layouts_.emplace(key, std::move(result)).first->second;
}
float Fonts::centered(const std::string &text, float cx, float y, int size,
                      SDL_Color color, float width, int lines, bool bold,
                      float lineHeight) {
  const auto &rows = layout(text, size, bold, width, lines);
  for (size_t row = 0; row < rows.size(); ++row)
    this->text(rows[row], cx - std::min(width, measure(rows[row], size, bold)) / 2,
               y + row * size * lineHeight, size, color, bold, width);
  return rows.size() * size * lineHeight;
}
float Fonts::wrapped(const std::string &text, float x, float y, int size,
                     SDL_Color color, float width, int lines, bool bold,
                     float lineHeight) {
  const auto &rows = layout(text, size, bold, width, lines);
  for (size_t row = 0; row < rows.size(); ++row)
    this->text(rows[row], x, y + row * size * lineHeight, size, color, bold,
               width);
  return rows.size() * size * lineHeight;
}
} // namespace foil::ui
