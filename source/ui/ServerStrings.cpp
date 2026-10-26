#include "net/ClientStringsApi.hpp"
#include "ui/Application.hpp"
#include "ui/Theme.hpp"
#include <SDL.h>
#include <algorithm>
#include <cstring>
namespace foil::ui {
namespace {
bool shipped(const std::string &code) {
  return languageCode(languageFromCode(code, Language::English)) == code;
}
constexpr uint32_t StallMs = 5000; // nothing arriving this long fails a language's download
} // namespace
// Every language kept on the card, not only the one shown: switching to
// another then shows its server text at once, with no read of the card.
void Application::useKeptStrings() {
  serverLanguages_ = store_.clientLanguages();
  for (auto &[code, texts] : net::keptStrings(store_)) {
    if (shipped(code)) {
      if (code != "en") setServerTexts(languageFromCode(code, Language::English), texts);
    } else {
      extraKept_[code] = std::move(texts);
    }
  }
}
std::optional<Server> Application::stringsSource() const {
  for (const auto &server : config_.servers)
    if (const auto features = serverFeatures_.find(server.url);
        features != serverFeatures_.end() && features->second.count("client-strings"))
      return server;
  return std::nullopt;
}
// The source server's language list replaces the card's; the current
// language is asked of it once a run.
void Application::askServerStrings() {
  const auto source = stringsSource();
  if (!source) return;
  const net::Session *told = session_ && session_->server().url == source->url ? session_.get() : nullptr;
  for (const auto &other : others_)
    if (!told && other->server().url == source->url) told = other.get();
  const auto same = [](const ExtraLanguage &a, const ExtraLanguage &b) {
    return a.code == b.code && a.name == b.name && a.base == b.base;
  };
  if (told && !told->clientLanguages().empty() &&
      !std::equal(serverLanguages_.begin(), serverLanguages_.end(), told->clientLanguages().begin(),
                  told->clientLanguages().end(), same)) {
    serverLanguages_ = told->clientLanguages();
    runOffScreen([this, languages = serverLanguages_] {
      std::string error;
      if (!store_.saveClientLanguages(languages, error)) logLine("strings: " + error);
    });
  }
  if (std::strcmp(languageCode(), "en") == 0) return;
  if (stringsJob_.busy()) {
    stringsAgain_ = true;
    return;
  }
  const std::string lang = languageCode();
  if (!stringsAsked_.insert(lang + " " + source->url).second) return;
  stringsJob_.run(
      "strings",
      [this, source, lang, state = state_]() -> Job::Finish {
        auto texts = net::serverStrings(store_, &*source, state, lang, &stop_);
        return [this, lang, texts = std::move(texts)]() mutable {
          pendingStrings_.emplace(lang, std::move(texts));
          if (std::exchange(stringsAgain_, false)) askServerStrings();
        };
      },
      [this] {
        if (std::exchange(stringsAgain_, false)) askServerStrings();
      });
}
void Application::applyServerStrings() {
  if (!pendingStrings_) return;
  auto [code, texts] = std::move(*pendingStrings_);
  pendingStrings_.reset();
  if (shipped(code)) {
    setServerTexts(languageFromCode(code, Language::English), texts);
    return;
  }
  if (texts.empty()) return; // nothing new from the server: what is in use stays
  extraKept_[code] = std::move(texts);
  if (const auto *inUse = extraLanguage(); inUse && inUse->code == code) useKeptExtra(*inUse);
}
std::vector<ExtraLanguage> Application::pickerExtras() const {
  std::vector<ExtraLanguage> out;
  for (const auto &extra : serverLanguages_)
    if (!shipped(extra.code)) out.push_back(extra);
  return out;
}
// Two columns of up to eight rows, then more columns.
int Application::languageRows() const {
  constexpr int MostRows = 8;
  return std::min(MostRows, (languageCount() + 1) / 2);
}
bool Application::useKeptExtra(const ExtraLanguage &extra) {
  const auto texts = extraKept_.find(extra.code);
  if (texts == extraKept_.end()) return false;
  const auto base = extraKept_.find(extra.base);
  static const std::map<std::string, std::string> none;
  return useExtraLanguage(extra, texts->second, base == extraKept_.end() ? none : base->second);
}
void Application::pickLanguage(int row) {
  if (row < LanguageCount) {
    if (Language(row) != language() || extraLanguage()) chooseLanguage(Language(row));
    return;
  }
  const auto extras = pickerExtras();
  if (row - LanguageCount >= int(extras.size())) return;
  const auto &extra = extras[size_t(row - LanguageCount)];
  if (const auto *inUse = extraLanguage(); inUse && inUse->code == extra.code) return;
  const bool baseKept = extra.base.empty() || shipped(extra.base) || extraKept_.count(extra.base);
  if (baseKept && useKeptExtra(extra)) {
    languageChanged();
    return;
  }
  languageGetting_ = extra;
  startLanguageGet();
}
void Application::startLanguageGet() {
  if (languageJob_.busy()) return;
  languageGet_ = LanguageGet::Getting;
  languageGetError_.clear();
  languageProgress_.received = 0;
  languageProgress_.total = 0;
  languageCancel_ = false;
  languageStalled_ = false;
  languageSeen_ = 0;
  languageSeenAt_ = SDL_GetTicks();
  const auto source = stringsSource();
  if (!source) {
    languageGet_ = LanguageGet::Failed;
    languageGetError_ = net::offlineMessage();
    return;
  }
  // The base too, one step only, when the app does not ship it and the card lacks it.
  std::set<std::string> kept;
  for (const auto &[code, texts] : extraKept_) kept.insert(code);
  languageJob_.run(
      "language",
      [this, server = *source, state = state_, extra = languageGetting_, kept]() -> Job::Finish {
        net::StringsReply reply, baseReply;
        std::string error;
        bool ok = net::downloadStrings(store_, server, state, extra.code, languageProgress_, reply, error, &languageCancel_);
        const std::string base = ok && !reply.base.empty() ? reply.base : extra.base;
        const bool wantBase = ok && !base.empty() && base != extra.code && !shipped(base) && !kept.count(base);
        if (wantBase) {
          languageProgress_.received = 0;
          languageProgress_.total = 0;
          ok = net::downloadStrings(store_, server, state, base, languageProgress_, baseReply, error, &languageCancel_);
        }
        return [this, ok, error, extra, base, wantBase, reply = std::move(reply), baseReply = std::move(baseReply)]() mutable {
          if (!ok) {
            languageGet_ = LanguageGet::Failed;
            languageGetError_ = languageStalled_ ? tr("Nothing arrived for 5 seconds. Check the connection and try again.")
                                : languageCancel_ ? std::string(tr("Cancelled"))
                                                  : error;
            return;
          }
          extraKept_[extra.code] = std::move(reply.strings);
          if (wantBase) extraKept_[base] = std::move(baseReply.strings);
          extra.base = base;
          languageGet_ = LanguageGet::None;
          if (useKeptExtra(extra)) languageChanged();
        };
      },
      [this] {
        languageGet_ = LanguageGet::Failed;
        languageGetError_.clear();
      });
}
void Application::pollLanguageGet() {
  if (languageGet_ != LanguageGet::Getting || languageCancel_) return;
  const auto received = languageProgress_.received.load();
  const auto now = SDL_GetTicks();
  if (received != languageSeen_) {
    languageSeen_ = received;
    languageSeenAt_ = now;
  } else if (now - languageSeenAt_ > StallMs) {
    languageStalled_ = true;
    languageCancel_ = true;
  }
}
// The window speaks the language in use: the one coming is not there yet,
// so its text is built in and locked (i18n::lockedKeys).
void Application::languageGetDialog() {
  if (languageGet_ == LanguageGet::Getting) {
    const auto body = modal(640, 24 + 34 + 30 + 10 + 24, tr("Downloading additional language data"), tr("Cancel"), "",
                            NoAccept, 4);
    paint_.fonts.text(tr("Getting additional language data."), body.x + 32, body.y + 24, 19, Secondary, false,
                      body.w - 64);
    const auto received = languageProgress_.received.load(), total = languageProgress_.total.load();
    paint_.fonts.text(languageGetting_.name + " · " + formatBytes(received) + (total ? " / " + formatBytes(total) : ""),
                      body.x + 32, body.y + 24 + 34, 19, Ink, true, body.w - 64);
    const float ratio = total ? std::min(1.f, float(received) / float(total)) : 0.f;
    const float barY = body.y + 24 + 34 + 30;
    paint_.rect(body.x + 32, barY, body.w - 64, 10, Line, 5);
    paint_.rect(body.x + 32, barY, std::max(10.f, (body.w - 64) * ratio), 10, Butter, 5);
    return;
  }
  const auto body = modal(640, 24 + 30 + 52 + 24, tr("Downloading additional language data"), tr("Close"), "",
                          tr("Try again"), 3);
  paint_.fonts.text(tr("Could not get the additional language data."), body.x + 32, body.y + 24, 19, Ink, true,
                    body.w - 64);
  if (!languageGetError_.empty())
    paint_.fonts.wrapped(languageGetError_, body.x + 32, body.y + 24 + 34, 18, Secondary, body.w - 64, 2);
}
void Application::languageGetAction(Action a) {
  if (languageGet_ == LanguageGet::Getting) {
    if (a == Action::Back) languageCancel_ = true;
    return;
  }
  if (a == Action::Back) languageGet_ = LanguageGet::None;
  if (a == Action::Accept && settled()) startLanguageGet();
}
// 설정 > 언어: the app's languages with their flags, then the ones only a
// server has, with their codes.
void Application::languageDialog() {
  constexpr float Row = 60, Gap = 8;
  const auto extras = pickerExtras();
  const int count = languageCount(), rows = languageRows(), columns = (count + rows - 1) / rows;
  const float width = columns <= 2 ? 820 : std::min(1240.f, 24 * 2 + columns * 300.f);
  const auto body = modal(width, 16 + rows * (Row + Gap) + 8,
                          // " · Language" stays in English, as on 설정's row.
                          std::string(tr("Language")) + (std::strcmp(languageCode(), "en") == 0 ? "" : " · Language"),
                          tr("Close"), "", "", 17);
  const float w = (body.w - 48 - Gap * 2 * (columns - 1)) / columns;
  const std::string current = languageCode();
  for (int i = 0; i < count; ++i) {
    const float x = body.x + 24 + (i / rows) * (w + Gap * 2), y = body.y + 16 + (i % rows) * (Row + Gap);
    rowHit(x, y, w, Row, 12, [this, i] {
      languageRow_ = i;
      action(Action::Accept);
    });
    if (i == languageRow_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    const bool own = i < LanguageCount;
    const std::string code = own ? languageCode(Language(i)) : extras[size_t(i - LanguageCount)].code;
    if (own) paint_.flag(i, x + 18, y + (Row - 28) / 2, 42, 28);
    else paint_.languageChip(code, x + 18, y + (Row - 28) / 2, 42, 28);
    const std::string name = own ? languageName(Language(i)) : extras[size_t(i - LanguageCount)].name;
    const bool inUse = code == current;
    paint_.fonts.middle(name, x + 78, y + Row / 2, 21, Ink, true, w - 78 - (inUse ? 52 : 12));
    if (inUse) paint_.icon(8, x + w - 44, y + (Row - 26) / 2, Good, 26);
  }
}
} // namespace foil::ui
