#include "net/ClientStringsApi.hpp"
#include "ui/Application.hpp"
namespace foil::ui {
// Every language kept on the card, not only the one shown: switching to
// another then shows its server text at once, with no read of the card.
void Application::useKeptStrings() {
  for (const auto &[code, texts] : net::keptStrings(store_)) {
    const auto lang = languageFromCode(code, Language::English);
    if (lang != Language::English) setServerTexts(lang, texts);
  }
}
// The first server, in the settings' order, that offers the text; each
// language is asked of it once a run.
void Application::askServerStrings() {
  if (language() == Language::English) return;
  if (stringsJob_.busy()) {
    stringsAgain_ = true;
    return;
  }
  std::optional<Server> source;
  for (const auto &server : config_.servers)
    if (const auto features = serverFeatures_.find(server.url);
        features != serverFeatures_.end() && features->second.count("client-strings")) {
      source = server;
      break;
    }
  if (!source) return;
  const std::string lang = languageCode();
  if (!stringsAsked_.insert(lang + " " + source->url).second) return;
  const auto wanted = language();
  stringsJob_.run(
      "strings",
      [this, source, lang, wanted, state = state_]() -> Job::Finish {
        auto texts = net::serverStrings(store_, &*source, state, lang, &stop_);
        return [this, wanted, texts = std::move(texts)]() mutable {
          pendingStrings_.emplace(wanted, std::move(texts));
          if (std::exchange(stringsAgain_, false)) askServerStrings();
        };
      },
      [this] {
        if (std::exchange(stringsAgain_, false)) askServerStrings();
      });
}
void Application::applyServerStrings() {
  if (!pendingStrings_) return;
  setServerTexts(pendingStrings_->first, pendingStrings_->second);
  pendingStrings_.reset();
}
} // namespace foil::ui
