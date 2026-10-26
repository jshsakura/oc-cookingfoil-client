#pragma once
// 관리자 편집: what the server lets an admin console change about a homebrew
// app, a retro game or a library game (feature admin-edit). The server decides
// the fields, their labels and their choices; the console only draws them and
// sends a change back. Long text stays on the web dashboard.
#include <map>
#include <string>
#include <vector>
namespace foil::admin {
using Label = std::map<std::string, std::string>; // language code -> text
enum class Type { Toggle, Choice };
struct Option {
  std::string value;
  Label label;
};
struct Field {
  std::string key;
  Type type = Type::Toggle;
  Label label;
  bool on = false;     // a toggle's state
  std::string value;   // a choice's value
  std::vector<Option> options;
};
// {"fields":[...]}: toggles with a boolean value and choices with options;
// anything else is left out.
std::vector<Field> parseFields(const std::string &json);
// The text in `language`, else English, else `fallback`.
std::string text(const Label &label, const std::string &language, const std::string &fallback);
// The option the choice holds; the first when its value is none of them.
int optionIndex(const Field &field);
// The body that flips a toggle, and the one that moves a choice by `step`.
std::string toggled(const Field &field);
std::string stepped(const Field &field, int step);
// `fields` with a body above applied, as the server would (the host preview).
std::vector<Field> applied(std::vector<Field> fields, const std::string &body);
} // namespace foil::admin
