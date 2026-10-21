// 관리자 편집: on a console the server marks as an admin's (feature
// admin-edit), a chip at the top right of a game's or app's page opens the
// item's switches and choices; each change goes to the server at once.
#include "net/AdminEditApi.hpp"
#include "ui/Application.hpp"
namespace foil::ui {
std::optional<Application::AdminTarget> Application::adminTarget() const {
  if (!session_ && !adminPreview_) return std::nullopt;
  std::optional<AdminTarget> target;
  if (screen_ == Screen::Detail && detailGroup_ < groups_.size()) {
    const auto &group = groups_[detailGroup_];
    target = AdminTarget{"game", group.id, detail_.name.empty() ? group.representative.name : detail_.name,
                         group.base ? group.base->server : group.representative.server};
  } else if (screen_ == Screen::RetroDetail && !retroPick_.id.empty())
    target = AdminTarget{"retro", retroPick_.id, retroPick_.name, retroPick_.server};
  else if (homebrewAsk_ && (screen_ == Screen::Homebrew || screen_ == Screen::Downloads))
    if (const auto *app = homebrewPicked(); app && !app->cardOnly)
      target = AdminTarget{"homebrew", app->id, app->name, app->server};
  if (!target) return std::nullopt;
  // The item's own server must take edits from this console.
  const auto features = serverFeatures_.find(target->server.empty() ? serverInUse() : target->server);
  const bool offered = adminPreview_ ? features_.count("admin-edit") > 0
                                     : features != serverFeatures_.end() && features->second.count("admin-edit");
  return offered ? target : std::nullopt;
}
bool Application::adminFocused() const { return adminFocus_ && adminTarget(); }
// Up from the panel's first row reaches the chip, Down leaves it, A opens
// the editor. Back leaves the page as it always does.
bool Application::adminChipAction(Action a, bool atTop) {
  if (!adminTarget()) {
    adminFocus_ = false;
    return false;
  }
  if (!adminFocus_) {
    if (a != Action::Up || !atTop || descriptionFocus_) return false;
    adminFocus_ = true;
    return true;
  }
  if (a == Action::Back) {
    adminFocus_ = false;
    return false;
  }
  if (a == Action::Down) {
    adminFocus_ = false;
    if (voteTarget()) voteFocus_ = 1;
  }
  if (a == Action::Accept && settled()) openAdminEdit();
  return true;
}
// On the dark band, across from the way back.
float Application::adminChip() {
  constexpr float Right = BandRight, Height = 34, Top = ChipLine - Height / 2;
  if (!adminTarget()) return Right;
  const std::string label = tr("Admin edit");
  const float w = 14 + 18 + 8 + paint_.fonts.measure(label, 16, true) + 16, x = Right - w;
  hit(x - 4, 0, w + 8, Top + Height + 8, [this] {
    adminFocus_ = true;
    openAdminEdit();
  });
  // Under the cursor it turns butter, as a focused button does.
  const bool focused = adminFocus_;
  paint_.rect(x, Top, w, Height, focused ? Butter : SDL_Color{255, 255, 255, 41}, 17);
  paint_.icon(45, x + 14, Top + (Height - 18) / 2, focused ? Ink : Butter, 18);
  paint_.fonts.middle(label, x + 40, Top + Height / 2, 16, focused ? Ink : Surface, true);
  return x;
}
void Application::openAdminEdit() {
  const auto target = adminTarget();
  if (!target || adminJob_.busy()) return;
  adminEdit_ = {};
  adminEdit_.open = true;
  adminEdit_.target = *target;
  if (adminPreview_) {
    adminEdit_.fields = *adminPreview_;
    return;
  }
  adminEdit_.busy = true;
  const auto server = serverFor(target->server);
  const auto state = state_;
  adminJob_.run("fields", [this, server, state, target = *target]() -> Job::Finish {
    auto fields = net::adminFields(server, state, target.kind, target.id, &stop_);
    return [this, fields = std::move(fields)] {
      adminEdit_.busy = false;
      adminEdit_.error = fields.error;
      adminEdit_.fields = fields.value;
    };
  }, [this] {
    adminEdit_.busy = false;
    adminEdit_.error.message = tr("Something went wrong. Check log.txt.");
  });
}
void Application::sendAdminChange(const std::string &body) {
  if (body.empty() || adminJob_.busy()) return;
  if (adminPreview_) {
    adminEdit_.fields = *adminPreview_ = admin::applied(*adminPreview_, body);
    return;
  }
  adminEdit_.busy = true;
  const auto server = serverFor(adminEdit_.target.server);
  const auto state = state_;
  adminJob_.run("change", [this, server, state, target = adminEdit_.target, body]() -> Job::Finish {
    auto fields = net::setAdminField(server, state, target.kind, target.id, body, &stop_);
    return [this, fields = std::move(fields)] {
      adminEdit_.busy = false;
      adminEdit_.error = fields.error;
      if (fields.error) return;
      adminEdit_.fields = fields.value;
      adminEdit_.changed = true;
    };
  }, [this] {
    adminEdit_.busy = false;
    adminEdit_.error.message = tr("Something went wrong. Check log.txt.");
  });
}
void Application::adminEditAction(Action a) {
  auto &edit = adminEdit_;
  const int count = int(edit.fields.size());
  edit.row = std::clamp(edit.row, 0, std::max(0, count - 1));
  if (!edit.confirm.empty()) {
    if (a == Action::Back) edit.confirm.clear();
    else if (a == Action::Accept && settled()) sendAdminChange(std::exchange(edit.confirm, {}));
    return;
  }
  if (a == Action::Back) {
    edit.open = false;
    // What changed shows in the lists and on this page: they are read again.
    if (edit.changed) reload();
    return;
  }
  if (a == Action::Up) edit.row = std::max(0, edit.row - 1);
  if (a == Action::Down) edit.row = std::min(count - 1, edit.row + 1);
  if (edit.busy || !count || !settled()) return;
  const auto &field = edit.fields[edit.row];
  // Hiding takes the item out of every list, this console's too: it asks first.
  if (a == Action::Accept && field.type == admin::Type::Toggle && field.key == "enabled" && field.on) {
    edit.confirm = admin::toggled(field);
    popupAt_ = SDL_GetTicks();
  } else if (a == Action::Accept)
    sendAdminChange(field.type == admin::Type::Toggle ? admin::toggled(field) : admin::stepped(field, 1));
  else if (field.type == admin::Type::Choice && (a == Action::Left || a == Action::Right))
    sendAdminChange(admin::stepped(field, a == Action::Right ? 1 : -1));
}
void Application::adminEditDialog() {
  const auto &edit = adminEdit_;
  if (!edit.confirm.empty()) {
    constexpr float Width = 620, Text = Width - 64;
    const auto &name = edit.target.name;
    const std::string ask = tr("Hide it from the lists?");
    const std::string note = tr("It disappears from every console's list, this one too. Turn it back on in the web dashboard.");
    const float nameH = paint_.fonts.lineCount(name, 22, Text, 2, true) * 22 * 1.3f;
    const float askH = paint_.fonts.lineCount(ask, 19, Text, 2) * 19 * 1.3f;
    const float noteH = paint_.fonts.lineCount(note, 18, Text, 3) * 18 * 1.3f;
    const auto body = modal(Width, 22 + nameH + 10 + askH + 10 + noteH + 20, tr("Admin edit"), tr("Cancel"), "", tr("Hide|admin"), 45);
    float y = body.y + 22;
    paint_.fonts.wrapped(name, body.x + 32, y, 22, Ink, Text, 2, true);
    y += nameH + 10;
    paint_.fonts.wrapped(ask, body.x + 32, y, 19, Ink, Text, 2);
    y += askH + 10;
    paint_.fonts.wrapped(note, body.x + 32, y, 18, Secondary, Text, 3);
    return;
  }
  constexpr float Row = 58, Gap = 8;
  const int count = int(edit.fields.size());
  const float listH = count ? count * (Row + Gap) - Gap : Row;
  const auto body = modal(620, 16 + listH + 16 + 52, tr("Admin edit"), tr("Close"), "", count ? tr("Change|admin") : NoAccept, 45);
  const float x = body.x + 24, w = body.w - 48;
  float y = body.y + 16;
  const std::string language = languageCode();
  if (!count) {
    const auto note = edit.busy ? std::string(tr("Loading")) : edit.error ? edit.error.message : std::string(tr("Nothing to edit here."));
    paint_.fonts.wrapped(note, x, y + 8, 18, edit.error ? Bad : Secondary, w, 2);
  }
  for (int i = 0; i < count; ++i, y += Row + Gap) {
    const auto &field = edit.fields[i];
    hit(x, y, w, Row, [this, i] {
      adminEdit_.row = i;
      adminEditAction(Action::Accept);
    });
    if (i == edit.row) paint_.focusRow(x, y, w, Row, Ground, 14);
    else paint_.rect(x, y, w, Row, Ground, 14);
    const float mid = y + Row / 2;
    paint_.fonts.middle(admin::text(field.label, language, field.key), x + 18, mid, 19, Ink, true, w - 230);
    if (field.type == admin::Type::Toggle) {
      constexpr float SwitchW = 52, SwitchH = 30;
      const float sx = x + w - 18 - SwitchW;
      paint_.rect(sx, mid - SwitchH / 2, SwitchW, SwitchH, field.on ? Good : SDL_Color{214, 211, 204, 255}, 15);
      paint_.rect(field.on ? sx + SwitchW - 27 : sx + 3, mid - 12, 24, 24, Surface, 12);
      const std::string state = field.on ? tr("On|switch") : tr("Off|switch");
      paint_.fonts.middle(state, sx - 12 - paint_.fonts.measure(state, 15, true), mid, 15, field.on ? Good : Secondary, true);
    } else {
      const auto &option = field.options[admin::optionIndex(field)];
      const auto value = admin::text(option.label, language, option.value);
      const float valueW = std::min(150.f, paint_.fonts.measure(value, 17, true));
      const float right = x + w - 18;
      paint_.fonts.middle("›", right - 8, mid, 20, Secondary);
      paint_.fonts.middle(value, right - 22 - valueW, mid, 17, Ink, true, 150);
      paint_.fonts.middle("‹", right - 22 - valueW - 20, mid, 20, Secondary);
    }
  }
  // What happened last: saving, the server's refusal, or where long text goes.
  const auto foot = count && edit.busy ? std::string(tr("Saving"))
                    : count && edit.error ? edit.error.message
                                          : std::string(tr("Changes are saved on the server at once. Names and descriptions are edited on the web dashboard."));
  paint_.fonts.wrapped(foot, x, body.y + 16 + listH + 14, 15, count && edit.error ? Bad : Secondary, w, 2);
}
} // namespace foil::ui
