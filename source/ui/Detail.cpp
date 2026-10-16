#include "ui/Application.hpp"
#include <cmath>
#include "platform/Storage.hpp"
#include "install/Installer.hpp"
#include "core/I18n.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <optional>
namespace foil::ui {
namespace {
// 1234 -> "1,234".
std::string countText(int count) {
  auto text = std::to_string(count);
  for (int at = int(text.size()) - 3; at > 0; at -= 3)
    text.insert(size_t(at), ",");
  return text;
}
std::string dateLabel(int date) { return dateText(date); }
} // namespace
// Only that the game needs a patch to run, ahead of its description; what
// it is shows in the install list, and the story belongs in the description.
int Application::requiredPatchesCard(int y) {
  if (detailGroup_ >= groups_.size()) return y;
  const auto mine = patchesFor(detail_.patches, groups_[detailGroup_].id);
  if (std::none_of(mine.begin(), mine.end(), [](const auto &patch) { return patch.required; })) return y;
  constexpr float X = 40, W = 750, Height = 52;
  paint_.rect(X, y, W, Height, SDL_Color{255, 241, 204, 255}, 14);
  paint_.icon(33, X + 18, y + (Height - 24) / 2, Pending, 24);
  paint_.fonts.middle(tr("This game needs a patch to run"), X + 54, y + Height / 2, 18, Ink, true, W - 72);
  return int(y + Height + 16);
}
// The operator's notes (what else to install, how big, which build): a
// card of their own under the patch notice, before the description.
int Application::notesCard(int y) {
  if (detail_.notes.empty()) return y;
  constexpr float X = 40, W = 750, Inner = X + 54, InnerW = W - 74;
  const float text = paint_.fonts.wrapped(detail_.notes, Inner, -10000, 18, Ink, InnerW, 12, false, 1.5f);
  const float height = 18 + 28 + text + 18;
  paint_.rect(X, y, W, height, Surface, 14);
  paint_.icon(33, X + 18, y + 18, Ink, 24);
  paint_.fonts.text(tr("More info"), Inner, y + 18, 18, Ink, true, InnerW);
  paint_.fonts.wrapped(detail_.notes, Inner, y + 18 + 28, 18, Ink, InnerW, 12, false, 1.5f);
  return int(y + height + 16);
}
void Application::syncDetailPatches() {
  if (!selection_ || detailGroup_ >= groups_.size()) return;
  const int before = int(selection_->rows());
  selection_->setPatches(patchesFor(detail_.patches, groups_[detailGroup_].id));
  // The cursor stays on the storage cards or the button below the rows.
  if (part_ >= before) part_ += int(selection_->rows()) - before;
}
void Application::setDetail(net::Detail detail) {
  detail_ = std::move(detail);
  syncDetailPatches();
}
void Application::openDetail(size_t group) {
  if (group >= groups_.size())
    return;
  detailGroup_ = group;
  previous_ = screen_;
  screen_ = Screen::Detail;
  detail_ = {};
  part_ = 0;
  descriptionScroll_ = 0;
  descriptionFocus_ = false;
  galleryOpen_ = false;
  screenshot_ = 0;
  selection_ =
      std::make_unique<Selection>(groups_[group], installTarget());
  // Start on the install button; Up reaches the storage cards, then the items.
  part_ = int(selection_->rows()) + 1;
  fetchDetail();
}
void Application::fetchDetail() {
  if (!session_)
    return;
  const auto found =
      details_.find(session_->server().url + groups_[detailGroup_].id);
  if (found != details_.end() && SDL_GetTicks() - found->second.at < 60000) {
    detail_ = found->second.detail;
    syncDetailPatches();
    return;
  }
  if (network_.joinable())
    return;
  requestedId_ = groups_[detailGroup_].id;
  detailLoading_ = true;
  detailRequest_ = true;
  const auto id = groups_[detailGroup_].id;
  network_ = std::thread([this, id] {
    const bool ok = guarded("detail", [this, &id] {
      auto result = session_->detail(id);
      incomingError_ = result.error;
      incomingDetail_ = std::move(result.value);
    });
    if (!ok)
      incomingError_.message = tr("Could not load the game details.");
    done_ = true;
  });
}
void Application::detail() {
  if (detailGroup_ >= groups_.size() || !selection_)
    return;
  const auto &g = groups_[detailGroup_];
  const auto &item = g.representative;
  // A localized store name wins over the detail's default one.
  const auto &source = g.base ? *g.base : item;
  const auto name = !source.names.empty() || detail_.name.empty() ? item.name : detail_.name;
  auto publisher = detail_.publisher.empty() ? item.publisher : detail_.publisher;
  // The server fills a missing publisher with its own name; that is not one.
  if (publisher == "CookingFoil")
    publisher.clear();
  paint_.rect(0, 0, 1280, 212, {10, 10, 14, 255});
  const auto banner = detail_.banner.empty() ? thumbnailUrl(item.icon) : thumbnailUrl(detail_.banner, "xl");
  // Only while it is really on its way; a picture already here draws at once.
  if (!banner.empty() && !images_->loaded(banner) && !images_->failed(banner))
    paint_.skeleton(0, 0, 1280, 212, 0, true);
  images_->draw(banner, 0, 0, 1280, 212, 0, true);
  // Back link names the screen it returns to, with room before the cover.
  const auto back = screenLabel(previous_);
  paint_.icon(7, 32, 14, Surface, 20);
  paint_.fonts.middle(back, 56, 24, 17, Surface);
  hit(24, 4, 200, 40, [this] { action(Action::Back); });
  constexpr int Top = 58;
  cover(item.icon, 40, Top, 140, false, item.name, false, true);
  paint_.fonts.text(name, 208, Top + 4, 38, Surface, true, 1010);
  paint_.fonts.text(publisher, 208, Top + 62, 19, {228, 228, 234, 255}, false, 960);
  // Pills stop before the install panel; one that would reach it is left
  // out (genres are listed again in the facts below).
  constexpr float PillsRight = 806;
  float x = 208;
  auto pill = [&](const std::string &text) {
    if (x + paint_.fonts.measure(text, 17) + 28 > PillsRight)
      return;
    x += paint_.pill(text, x, Top + 100, true, 17) + 10;
  };
  if (item.requiredPatches || (g.base && g.base->requiredPatches))
    pill(tr("Required patch"));
  if (const int integrity = groupIntegrity(g))
    pill(integrity == 2 ? tr("May be damaged") : tr("Modified file"));
  const auto &languages = item.languages;
  if (std::find(languages.begin(), languages.end(), "ko") != languages.end())
    pill(tr("Korean"));
  if (const int score = detail_.score ? detail_.score : g.base ? g.base->score : item.score)
    pill(trf("Score {0}", {std::to_string(score)}));
  if (!detail_.priceRegular.empty())
    pill(detail_.priceDiscount.empty() ? "eShop " + priceText(detail_.priceRegular)
                                       : trf("eShop sale {0}", {priceText(detail_.priceDiscount)}));
  // Genres last: the longest pill, and the first to give way.
  if (!detail_.category.empty())
    pill(detail_.category);
  // The description column scrolls in the whole band between the header and
  // the footer, with the same padding at the top and the bottom, so text
  // slides under the header's edge instead of stopping short of it.
  constexpr int BandTop = 212, BandBottom = 660, Pad = 14;
  SDL_Rect leftClip{0, BandTop, 806, BandBottom - BandTop};
  SDL_RenderSetClipRect(paint_.renderer, &leftClip);
  // The text follows its scroll steps, or the finger while it glides.
  const int offset = int(std::lround(glide("description", 0, BandTop, 806, BandBottom - BandTop,
                                           float(descriptionScroll_ * ScrollStep), float(descriptionHeight_), 1)));
  int y = BandTop + Pad - offset;
  y = requiredPatchesCard(y);
  y = notesCard(y);
  if (!detail_.description.empty()) {
    y += paint_.fonts.wrapped(detail_.description, 40, y, 20, Ink, 750, 20, false, 1.6f) + 24;
  } else if (detailLoading_) {
    // Text-shaped bars only while the store answers: the white facts card
    // would open high and then jump below the description as it arrives.
    for (int line = 0; line < 9; ++line)
      paint_.skeleton(40, y + line * 34, line % 4 == 3 ? 420 : 720, 18, 6);
    y += 9 * 34 + 20;
  } else {
    // No description from the store: a quiet card instead of a gap, so the
    // page still opens with something written.
    constexpr float Height = 92, Badge = 52;
    paint_.rect(40, y, 750, Height, Surface, 16);
    paint_.rect(64, y + (Height - Badge) / 2, Badge, Badge, {255, 241, 204, 255}, int(Badge / 2));
    paint_.icon(29, 64 + (Badge - 26) / 2, y + (Height - 26) / 2, Ink, 26);
    paint_.fonts.middle(tr("No description yet"), 136, y + 34, 20, Ink, true, 620);
    paint_.fonts.middle(tr("The publisher has not added a description. Check the details below."), 136,
                        y + 62, 16, Secondary, false, 630);
    y += Height + 16;
  }
  if (!detailLoading_ || !detail_.description.empty())
    y = infoCard(g, publisher, y) + 22;
  if (detailLoading_ && detail_.screenshots.empty()) {
    paint_.skeleton(40, y, 120, 22, 6);
    for (int i = 0; i < 3; ++i)
      paint_.skeleton(40 + i * 214, y + 40, 200, 112, 10);
    y += 174;
  }
  y = screenshotStrip(y);
  // The base game's files not chosen (A on its row in 설치할 항목 picks one).
  if (selection_ && selection_->baseChoices() > 1) {
    paint_.fonts.text(tr("Other files"), 40, y, 20, Ink, true);
    y += 38;
    const auto &files = selection_->baseFiles();
    for (size_t i = 0; i < files.size(); ++i) {
      if (i == selection_->baseChoice()) continue;
      const auto &f = files[i];
      paint_.fonts.text(f.name + " · " + formatBytes(f.size), 40, y, 17,
                        Secondary, false, 750);
      y += 32;
    }
  }
  // y is the bottom of the content; allow scrolling until it sits Pad
  // above the footer.
  descriptionHeight_ = std::max(0, y + offset - (BandBottom - Pad));
  {
    SDL_Rect full{0, 0, 1280, 660};
    SDL_RenderSetClipRect(paint_.renderer, &full);
    constexpr float View = BandBottom - BandTop;
    // One mark only: with the cursor on the description its scroll hint
    // stays up in butter.
    paint_.scrollbar(798, BandTop + Pad, View - 2 * Pad, float(offset), View,
                     View + float(descriptionHeight_), descriptionFocus_);
  }
  SDL_Rect full{0, 0, 1280, 660};
  SDL_RenderSetClipRect(paint_.renderer, &full);
  paint_.rect(818, 172, 430, 484, Surface, 20);
  // The heading in a band of its own, ruled off edge to edge.
  paint_.fonts.middle(tr("To install"), 840, 194, 22, Ink, true);
  paint_.rect(818, 216, 430, 1, Line);
  const auto &parts = selection_->parts();
  // Files, then the game's patches; the storage cards and the button follow.
  const int rows = int(selection_->rows());
  const bool panelFocus = !descriptionFocus_;
  // Rows have room above and below their two lines; everything is centered
  // in the row instead of hugging its top edge.
  constexpr float RowTop = 226, RowPitch = 56, RowHeight = 52;
  const float top = glide("parts", 840, RowTop - 4, 386, 4 * RowPitch, float(std::max(0, std::min(part_, rows - 1) - 3)),
                          float(rows - 4), RowPitch);
  const int first = int(top);
  clipTo(840, RowTop - 4, 386, 4 * RowPitch);
  for (int i = 0; i < 5 && first + i < rows; ++i) {
    const int n = first + i;
    const float y = RowTop + (n - top) * RowPitch, mid = y + RowHeight / 2;
    const auto *patch = selection_->patch(size_t(n));
    const bool checked = selection_->enabled(n), locked = !patch && parts[n].type == "base";
    hit(840, y, 386, RowHeight, [this, n] {
      part_ = n;
      descriptionFocus_ = false;
      action(Action::Accept);
    });
    if (part_ == n && panelFocus)
      paint_.focusRow(840, y, 386, RowHeight, Ground, 12);
    const SDL_Color boxEdge = !locked && checked ? Ink : SDL_Color{138, 138, 147, 255};
    paint_.rect(852, mid - 14, 28, 28, boxEdge, 8);
    paint_.rect(854, mid - 12, 24, 24,
                checked ? (locked ? SDL_Color{255, 224, 138, 255} : Butter) : Surface, 6);
    if (checked)
      paint_.icon(8, 856, mid - 10, locked ? Secondary : Ink, 20);
    std::string label, sub;
    uint64_t bytes = 0;
    if (patch) {
      // A patch: its name, then what it changes; one the game needs says so.
      label = patch->name;
      sub = patch->required ? tr("Needed to run") : tr("Patch");
      for (const auto &kind : patch->kinds) sub += std::string(" · ") + patchKindLabel(kind);
      bytes = patch->size;
    } else {
      const auto &f = parts[n];
      label = f.type == "base"     ? std::string(tr("Base game"))
              : f.type == "update" ? std::string(tr("Update|item"))
                                   : "DLC · " + f.name;
      // A game with more than one base file: its kind and which of them,
      // A going to the next.
      const auto choices = selection_->baseChoices();
      sub = f.type == "base" && n == 0 && choices > 1
                ? versionLabel(f) + " · " + fileFormat(f) + " · " + std::to_string(selection_->baseChoice() + 1) +
                      "/" + std::to_string(choices)
            : f.type == "base"     ? trf("{0} · Required", {versionLabel(f)})
            : f.type == "update" ? trf("{0} · Latest", {versionLabel(f)})
                                 : std::string(tr("Add-on content"));
      bytes = f.size;
    }
    const auto size = formatBytes(bytes);
    const float sizeWidth = paint_.fonts.measure(size, 18, true);
    paint_.fonts.middle(label, 894, mid - 10, 18, Ink, true, 300 - sizeWidth);
    paint_.fonts.middle(sub, 894, mid + 12, 15, Secondary, false, 300 - sizeWidth);
    paint_.fonts.middle(size, 1212 - sizeWidth, mid, 18, Ink, true);
  }
  unclip();
  if (rows > 4) {
    const auto at = std::to_string(std::min(rows, part_ + 1)) + " / " + std::to_string(rows);
    paint_.fonts.middle(at, 1226 - paint_.fonts.measure(at, 15), 194, 15, Secondary);
    paint_.scrollbar(1232, RowTop, 4 * RowPitch - 4, float(first), 4, float(rows));
  }
  paint_.rect(818, 454, 430, 1, Line); // edge to edge, as under the heading
  paint_.fonts.middle(tr("Total"), 852, 476, 18, Secondary);
  const auto total = formatBytes(selection_->total());
  paint_.fonts.middle(total, 1214 - paint_.fonts.measure(total, 24, true), 476,
                      24, Ink, true);
  // Two half-width cards, matching L/R: name on top, free space below, each
  // truncated to its card so longer labels cannot collide.
  struct Target {
    const char *id, *label;
    std::optional<uint64_t> free;
  };
  const Target targets[] = {{"sd", tr("SD card"), sdFree_}, {"nand", tr("Console"), nandFree_}};
  constexpr float CardWidth = (386 - 8) / 2.f, CardTop = 494, CardHeight = 60;
  for (int n = 0; n < 2; ++n) {
    const auto &t = targets[n];
    const float x = 840 + n * (CardWidth + 8);
    const bool chosen = selection_->target == t.id;
    hit(x, CardTop, CardWidth, CardHeight, [this, n, id = std::string(t.id)] {
      part_ = int(selection_->rows());
      descriptionFocus_ = false;
      storageCursor_ = n;
      selection_->target = id;
    });
    // The card under the cursor wears the focus border; the dark fill
    // marks the one chosen.
    if (n == storageCursor_ && part_ == rows && panelFocus)
      paint_.outline(x, CardTop, CardWidth, CardHeight, 12);
    paint_.rect(x, CardTop, CardWidth, CardHeight, chosen ? Ink : Ground, 12);
    const auto color = chosen ? Surface : Ink;
    // Icon centered on the card's height, with clear room before the text.
    constexpr float IconSize = 24, TextLeft = 52;
    paint_.icon(n == 0 ? 26 : 27, x + 16, CardTop + (CardHeight - IconSize) / 2,
                chosen ? Butter : Secondary, int(IconSize));
    paint_.fonts.middle(t.label, x + TextLeft, CardTop + 21, 18, color, chosen, CardWidth - TextLeft - 12);
    const auto room = t.free           ? trf("{0} free", {formatBytes(*t.free)})
                      : storageChecked_ ? std::string(tr("Space unknown"))
                                        : std::string(tr("Checking"));
    paint_.fonts.middle(room, x + TextLeft, CardTop + 41, 15,
                        chosen ? SDL_Color{200, 200, 206, 255} : Secondary, false, CardWidth - TextLeft - 12);
  }
  hit(840, 568, 386, 60, [this] {
    part_ = int(selection_->rows()) + 1;
    descriptionFocus_ = false;
    action(Action::Accept);
  });
  if (part_ == rows + 1 && panelFocus)
    paint_.ring(840, 568, 386, 60, 14);
  // Already on its way: dark while it downloads, its progress filling a
  // butter bar along the bottom; quiet gray while it waits its turn. A asks
  // whether to cancel it.
  double percent = 0;
  if (const int onTheWay = pickedOnTheWay(percent)) {
    onTheWayButton(840, 386, onTheWay == 2, percent);
  } else {
    paint_.rect(840, 568, 386, 60, Butter, 14);
    const auto install = tr("Install|action");
    paint_.keyLabel("A", install, 840 + (386 - paint_.keyLabelWidth("A", install, 22, true)) / 2, 598, 22, Ink, true);
  }
  if (galleryOpen_) gallery();
}
// Facts as a labeled grid, then languages, genres and contents as chips.
// Returns the y below the last block.
// A titled row of chips (languages, genres) in the left column; y below it.
float Application::chipRow(const std::string &title, const std::vector<std::string> &labels, float y) {
  constexpr float Left = 40, Width = 750;
  if (labels.empty())
    return y;
  paint_.fonts.text(title, Left, y, 18, Ink, true);
  float x = Left + 110;
  for (const auto &label : labels) {
    const float w = paint_.fonts.measure(label, 16, true) + 24;
    if (x + w > Left + Width) {
      x = Left + 110;
      y += 38;
    }
    paint_.rect(x, y - 3, w, 30, Surface, 15);
    paint_.fonts.middle(label, x + 12, y + 12, 16, Ink, true);
    x += w + 8;
  }
  return y + 46;
}
float Application::infoCard(const Group &g, const std::string &publisher, float y) {
  const auto &item = g.representative;
  // The base file chosen in 설치할 항목, which may not be the smallest.
  const bool chosen = selection_ && !selection_->parts().empty() && selection_->parts().front().type == "base";
  const auto &baseFile = chosen ? selection_->parts().front() : g.base ? *g.base : item;
  const auto extension = fileFormat(chosen ? baseFile : item);
  const int release = detail_.releaseDate ? detail_.releaseDate : item.releaseDate;
  const int players = detail_.players ? detail_.players : item.players;
  struct Fact {
    int icon;
    std::string label, value;
    SDL_Color color = Ink;
  };
  std::vector<Fact> facts;
  if (detail_.priceRegular.empty() && g.base && !g.base->priceRegular.empty()) {
    detail_.priceRegular = g.base->priceRegular;
    detail_.priceDiscount = g.base->priceDiscount;
    detail_.priceCountry = g.base->priceCountry;
  }
  if (!detail_.priceRegular.empty()) {
    const auto country = detail_.priceCountry == "US"   ? " " + std::string(tr("(US)"))
                         : detail_.priceCountry == "JP" ? " " + std::string(tr("(Japan)"))
                                                        : std::string();
    facts.push_back({15, tr("eShop price"), (detail_.priceDiscount.empty()
                                       ? priceText(detail_.priceRegular)
                                       : trf("{0} · was {1}", {priceText(detail_.priceDiscount),
                                                               priceText(detail_.priceRegular)})) +
                                      country});
  }
  {
    // Review score: rating and vote count over the score and its label.
    const auto &rated = g.base ? *g.base : item;
    const int score = detail_.score ? detail_.score : rated.score;
    const int votes = detail_.scoreCount ? detail_.scoreCount : rated.scoreCount;
    // The detail call names the label in Korean; the catalog sends a key.
    const auto label = !detail_.scoreLabel.empty() ? detail_.scoreLabel
                                                   : genreLabel(scoreLabels_, rated.scoreLabel);
    if (score) {
      // The vote count rides on the label so the value fits its cell.
      auto text = trf("{0}", {std::to_string(score)});
      if (!label.empty()) text += " · " + label;
      facts.push_back({31, votes ? trf("Score · {0} votes", {countText(votes)}) : std::string(tr("Score")), text});
    }
  }
  if (release) facts.push_back({11, tr("Released"), dateLabel(release)});
  if (!publisher.empty()) facts.push_back({12, tr("Publisher"), publisher});
  if (players) facts.push_back({13, tr("Players"), players == 1 ? std::string(tr("1 player")) : trf("Up to {0}", {std::to_string(players)})});
  facts.push_back({14, tr("Base size"), formatBytes(baseFile.size)});
  const auto &base = g.base ? *g.base : item;
  if (base.installSize) facts.push_back({14, tr("Install size"), formatBytes(base.installSize)});
  // The update can need newer firmware than the base; the higher one counts.
  std::vector<Item> needs{base};
  if (!detail_.requiredFirmware.empty()) needs.front().requiredFirmware = detail_.requiredFirmware;
  if (g.update) needs.push_back(*g.update);
  std::string firmware;
  for (const auto &n : needs)
    if (!n.requiredFirmware.empty() && (firmware.empty() || install::compareVersions(firmware, n.requiredFirmware) < 0))
      firmware = n.requiredFirmware;
  if (!firmware.empty()) {
    const auto console = platform::firmwareVersion();
    if (!install::missingFirmware(needs, console).empty())
      facts.push_back({27, tr("Firmware"), trf("{0}+ · console {1}", {firmware, console}), Bad});
    else
      facts.push_back({27, tr("Firmware"), firmware});
  }
  if (!detail_.ratingContent.empty()) facts.push_back({25, tr("Rating|age"), detail_.ratingContent});
  const int dlcTotal = detail_.dlcTotal ? detail_.dlcTotal : base.dlcTotal;
  if (dlcTotal) facts.push_back({22, "DLC", trf("{0} released · {1} here", {std::to_string(dlcTotal), std::to_string(g.dlc.size())})});
  facts.push_back({19, tr("Latest version"), versionLabel(g.update ? *g.update : item)});
  if (!extension.empty()) facts.push_back({16, tr("File type"), extension});
  if (!item.region.empty()) facts.push_back({17, tr("Region"), item.region});
  facts.push_back({18, tr("Title ID"), g.id});
  constexpr float Left = 40, Width = 750, Columns = 3, Cell = Width / Columns, RowHeight = 66;
  const int rows = int((facts.size() + 2) / 3);
  paint_.rect(Left, y, Width, rows * RowHeight + 24, Surface, 16);
  for (size_t n = 0; n < facts.size(); ++n) {
    const float x = Left + 20 + (n % 3) * Cell, top = y + 16 + (n / 3) * RowHeight;
    paint_.icon(facts[n].icon, x, top + 1, Secondary, 16);
    paint_.fonts.middle(facts[n].label, x + 22, top + 9, 15, Secondary);
    paint_.fonts.text(facts[n].value, x, top + 24, 18, facts[n].color, true, Cell - 30);
  }
  y += rows * RowHeight + 24 + 22;
  auto chips = [&](const std::string &title, const std::vector<std::string> &labels) { y = chipRow(title, labels, y); };
  std::vector<std::string> languages;
  for (const auto &code : item.languages) {
    const auto name = languageLabel(code);
    if (std::find(languages.begin(), languages.end(), name) == languages.end())
      languages.push_back(name);
  }
  // Labels from the detail call; otherwise the catalog's keys, translated.
  auto genres = detail_.categoryLabels;
  if (genres.empty())
    for (const auto &key : item.categories) genres.push_back(genreLabel(genreNames_, key));
  if (genres.empty() && !detail_.category.empty())
    genres.push_back(detail_.category);
  chips(tr("Languages"), languages);
  chips(tr("Genres"), genres);
  // What the game is made of shows in the install list on the right.
  return y;
}
// A pick on its way, in place of its button: dark with a butter bar while it
// downloads, quiet gray while it waits its turn. Before the first byte (the
// connection, the token, the files already there) a short bar runs back and
// forth, so the press shows at once rather than a still 0%.
void Application::onTheWayButton(float x, float w, bool downloading, double percent) {
  constexpr float Top = 568, Height = 60;
  paint_.rect(x, Top, w, Height, downloading ? Ink : SDL_Color{228, 225, 218, 255}, 14);
  const bool preparing = downloading && percent <= 0;
  if (downloading) {
    constexpr float Inset = 18, Track = 5;
    const float width = w - 2 * Inset, y = Top + Height - 11;
    paint_.rect(x + Inset, y, width, Track, {70, 70, 76, 255}, Track / 2);
    if (preparing) {
      const float run = width * 0.3f, t = float(SDL_GetTicks() % 1400) / 1400.f;
      const float at = (t < 0.5f ? t * 2 : 2 - t * 2) * (width - run);
      paint_.rect(x + Inset + at, y, run, Track, Butter, Track / 2);
    } else
      paint_.rect(x + Inset, y, std::max(Track, width * float(std::min(percent, 100.)) / 100), Track, Butter, Track / 2);
  }
  const auto label = preparing     ? std::string(tr("Preparing|download"))
                     : downloading ? trf("Downloading {0}", {std::to_string(int(percent)) + "%"})
                                   : std::string(tr("Waiting to download"));
  paint_.fonts.middle(label, x + (w - paint_.fonts.measure(label, 21, true)) / 2, downloading ? Top + 26 : Top + 30, 21,
                      downloading ? Surface : Secondary, true);
}
} // namespace foil::ui
