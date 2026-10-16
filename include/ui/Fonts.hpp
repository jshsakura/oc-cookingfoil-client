#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>
#include <array>
namespace foil::ui {
class Fonts {
public:
  explicit Fonts(SDL_Renderer *renderer);
  ~Fonts();
  bool open();
  float measure(const std::string &text, int size, bool bold = false);
  float text(const std::string &text, float x, float y, int size,
             SDL_Color color, bool bold = false, float maxWidth = 0);
  // Like wrapped, but each line is centered on cx. "\n" breaks lines.
  float centered(const std::string &text, float cx, float y, int size,
                 SDL_Color color, float width, int lines, bool bold = false,
                 float lineHeight = 1.4f);
  float wrapped(const std::string &text, float x, float y, int size,
                SDL_Color color, float width, int lines, bool bold = false,
                float lineHeight = 1.3f);
  bool supports(uint32_t codepoint, int size = 18);
  // How many lines wrapped() would take for `text` (at most `lines`).
  int lineCount(const std::string &text, int size, float width, int lines, bool bold = false) {
    return int(layout(text, size, bold, width, lines).size());
  }
  // Top y that puts the text's ink (not its line box) on centerY.
  float centerTop(const std::string &text, int size, bool bold, float centerY);
  // Draws text whose ink is vertically centered on centerY (labels in shapes).
  float middle(const std::string &text, float x, float centerY, int size,
               SDL_Color color, bool bold = false, float maxWidth = 0);
  // Kanji and hanzi share code points; the interface language decides which
  // subset draws them first (Chinese glyph shapes for Chinese, else Japanese).
  void preferChinese(bool chinese);

private:
  struct Run {
    TTF_Font *font;
    std::string text;
  };
  struct Label {
    SDL_Texture *texture;
    float width, height;
    float top; // from the layout baseline to the texture's top
    size_t bytes;
    uint64_t used = 0;
  };
  uint64_t tick_ = 0;
  size_t labelBytes_ = 0;
  int family(int source) const;
  TTF_Font *load(int source, int pixels, bool bold);
  // One open face per source and weight, set to the size asked each time it
  // is used: a face per text size held dozens of copies of each font.
  TTF_Font *opened(int source, bool bold);
  TTF_Font *face(int size, bool bold, int source);
  TTF_Font *at(TTF_Font *font, int size);
  void makeRoomForLabel(size_t adding);
  bool covers(int source, uint32_t codepoint);
  const std::vector<char> *bundled(int family, bool bold);
  const std::vector<Run> &runs(const std::string &text, int size, bool bold);
  std::string fit(const std::string &text, int size, bool bold, float maxWidth);
  const std::vector<std::string> &layout(const std::string &text, int size,
                                         bool bold, float width, int lines);
  SDL_Renderer *renderer_;
  float scale_ = 1;
  bool shared_ = false;
  // Bundled subsets, family * 2 + bold: Korean/Latin, Japanese, Chinese.
  std::array<std::vector<char>, 6> embedded_;
  std::array<bool, 6> unpacked_{};
  bool chinese_ = false;
  void clear();
  std::vector<TTF_Font *> faces_; // source * 2 + bold
  std::vector<bool> tried_;
  std::unordered_map<TTF_Font *, int> pixels_; // the size each face is set to
  std::unordered_map<std::string, Label> labels_;
  // Layout is recomputed every frame, so TTF measurements are memoized.
  std::unordered_map<std::string, std::vector<Run>> runs_;
  std::unordered_map<std::string, float> widths_;
  std::unordered_map<std::string, std::string> fitted_;
  std::unordered_map<std::string, std::vector<std::string>> layouts_;
  std::unordered_map<std::string, float> inks_; // centerTop's ink middle
  // Bytes each memo above holds, roughly; past its share it starts over.
  std::array<size_t, 5> memoBytes_{};
};
std::vector<std::string> utf8Characters(const std::string &text);
} // namespace foil::ui
