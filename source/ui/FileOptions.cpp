#include "ui/Application.hpp"
#include <algorithm>
namespace foil::ui {
// Asked once as the detail opens, of the game's own server and only when it
// offers file-variants; the answer counts for that game alone.
void Application::askFileOptions() {
  fileOptions_.clear();
  optionsFor_.clear();
  if (!selection_ || detailGroup_ >= groups_.size()) return;
  const auto &group = groups_[detailGroup_];
  const auto url = group.base ? group.base->server : group.representative.server;
  const auto server = url.empty() ? serverInUse() : url;
  const auto features = serverFeatures_.find(server);
  if (features == serverFeatures_.end() || !features->second.count("file-variants")) return;
  auto *session = sessionFor(url);
  if (!session) return;
  std::vector<std::string> files;
  for (const auto *list : {&selection_->parts(), &selection_->baseFiles()})
    for (const auto &f : *list)
      if (std::find(files.begin(), files.end(), f.url) == files.end()) files.push_back(f.url);
  optionsJob_.run(
      "file options",
      [this, from = session->server(), state = state_, files, id = group.id]() -> Job::Finish {
        auto options = net::fileOptions(from, state, files, &stop_);
        return [this, id, options = std::move(options)]() mutable {
          if (detailGroup_ >= groups_.size() || groups_[detailGroup_].id != id) return;
          fileOptions_ = std::move(options);
          optionsFor_ = id;
        };
      },
      [] {});
}
// The base file's first option, else the first picked file's.
const net::FileOption *Application::pickedOption() const {
  if (!selection_ || detailGroup_ >= groups_.size() || optionsFor_ != groups_[detailGroup_].id) return nullptr;
  auto files = selection_->files();
  std::stable_partition(files.begin(), files.end(), [](const Item &f) { return f.type == "base"; });
  for (const auto &f : files)
    if (const auto found = fileOptions_.find(f.url); found != fileOptions_.end() && !found->second.empty())
      return &found->second.front();
  return nullptr;
}
std::vector<Item> Application::withOptions(std::vector<Item> files) const {
  for (auto &f : files) {
    const auto found = fileOptions_.find(f.url);
    if (found == fileOptions_.end() || found->second.empty() || found->second.front().blocked) continue;
    const auto &option = found->second.front();
    f.url = net::withQuery(f.url, option.query);
    f.variant = true;
    f.hashesChange = option.hashesChange;
    f.variantProgress = option.progress;
    f.variantFailed = option.failed;
  }
  return files;
}
} // namespace foil::ui
