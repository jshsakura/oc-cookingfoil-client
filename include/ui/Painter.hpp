#pragma once
#include "ui/Fonts.hpp"
#include "ui/Theme.hpp"
#include <map>
namespace foil::ui {
class Painter {
public:
  explicit Painter(SDL_Renderer *renderer)
      : fonts(renderer), renderer(renderer) {}
  ~Painter();
  Painter(const Painter &) = delete;
  Painter &operator=(const Painter &) = delete;
  Fonts fonts;
  SDL_Renderer *renderer;
  void rect(float x, float y, float w, float h, SDL_Color color,
            int radius = 0);
  void line(float x, float y, float xx, float yy, SDL_Color color,
            int width = 2);
  // Diagonal stripes `period` apart inside a rounded rectangle already
  // filled, kept off its curved corners.
  void stripes(float x, float y, float w, float h, SDL_Color color,
               int radius = 0, float period = 28);
  // Placeholder block that pulses while content loads.
  // A waiting place that breathes in the shades of what it sits on: light
  // greys on the page, dark ones on a dark backdrop (gallery, banners).
  // `on`: the backdrop it sits on; the breath is the same against any of them.
  void skeleton(float x, float y, float w, float h, int radius = 10, bool dark = false, SDL_Color on = Ground);
  void skeletonCards(float x, float y, int count, int columns = 6);
  // Vertical position hint: appears when the list moves (or the screen
  // opens) and fades out shortly after; never shown when everything fits.
  // `active`: the cursor is on this area, so the hint stays up in butter as
  // the one mark of focus.
  void scrollbar(float x, float y, float height, float offset, float view,
                 float content, bool active = false);
  // Thin focus border for tightly packed rows, where ring() would be covered
  // by the next row. Draw it before the row's own fill.
  void outline(float x, float y, float w, float h, int radius = 10);
  // Row inside a panel: a 5px butter border drawn inside the row's own
  // bounds, so focus never spills onto headings or panel edges.
  void focusRow(float x, float y, float w, float h, SDL_Color fill, int radius = 12);
  void ring(float x, float y, float w, float h, int radius = 14);
  void icon(int kind, float x, float y, SDL_Color color, int size = 26);
  void logo(float x, float y);
  // A language's flag (foil::Language order), drawn into w x h (Flags.cpp).
  void flag(int language, float x, float y, float w, float h);
  // A language with no flag here (one only a server has): its code on a
  // dark chip the flag's size.
  void languageChip(const std::string &code, float x, float y, float w, float h);
  // A game's region as a small flag (kr jp us eu br; a globe for world, a
  // label for asia), w x h at x, y. Nothing for an unknown code.
  void region(const std::string &code, float x, float y, float w, float h);
  // An on/off switch, the knob on the right and green when on.
  void toggle(float x, float y, bool on);
  // Loading screen, presented at once: the mark and wordmark centered on
  // the ground, with a caption under them once fonts are open.
  // `progress` 0..1 fills the bar under the logo. `seconds` since the
  // screen came up makes the butter bounce; the mark and wordmark are made
  // once and kept until splashDone().
  void splash(const std::string &caption = "", float progress = 0, float seconds = 0);
  void splashDone();
  // Controller key badge: an ink circle (a pill for "L R"/"ZR") with the key
  // centered. Every key shown on screen uses this. Returns its width.
  float key(const std::string &name, float x, float centerY, bool enabled = true);
  // Key badge followed by its label. Returns the right edge.
  // Disabled draws the badge and label grey so the action reads as unavailable.
  float keyLabel(const std::string &name, const std::string &label, float x,
                 float centerY, int size, SDL_Color color, bool bold = false,
                 bool enabled = true);
  float keyWidth(const std::string &name);
  float keyLabelWidth(const std::string &name, const std::string &label,
                      int size, bool bold = false);
  // Small capsule holding a mini key badge and its label, for hints that
  // sit inside content (a row's "R See all") rather than in the footer.
  // Draws right-aligned to `right`; returns its width.
  float keyChip(const std::string &name, const std::string &label, float right, float centerY);
  float keyChipWidth(const std::string &name, const std::string &label);
  // The icon set by name, for icons the server names (labels, rows); -1
  // for a name the client does not know.
  static int iconNamed(const std::string &name);
  float pill(const std::string &text, float x, float y, bool dark = false,
             int size = 15);
  // The green 검수됨 capsule (a shield check and the word), as the web library
  // draws it; `dark` on the detail's dark band. Returns its width.
  float verifiedPill(float x, float y, bool dark = false, int size = 15);

private:
  struct ScrollSeen {
    float offset;
    Uint32 movedAt, drawnAt;
  };
  std::map<std::pair<int, int>, ScrollSeen> scrolls_;
  std::map<int, SDL_Texture *> icons_;
  std::map<int, SDL_Texture *> corners_;
  SDL_Texture *cornerTexture(int r);
  SDL_Texture *logo_ = nullptr, *wordmark_ = nullptr;
  SDL_Texture *splashMark_ = nullptr, *splashWord_ = nullptr;
  std::map<std::string, SDL_Texture *> flags_;
  SDL_Texture *renderWordmark(int size = 27);
  SDL_Texture *renderMark(int size);
};
} // namespace foil::ui
