#include "core/AdminEdit.hpp"
#include "core/Json.hpp"
namespace foil::admin {
namespace {
Label readLabel(json_object *o) {
  Label label;
  if (!o || !json_object_is_type(o, json_type_object)) return label;
  json_object_object_foreach(o, code, value) if (json_object_is_type(value, json_type_string))
      label[code] = json_object_get_string(value);
  return label;
}
std::string body(const std::string &key, json_object *value) {
  Json o(json_object_new_object());
  json_object_object_add(o.get(), "key", json_object_new_string(key.c_str()));
  json_object_object_add(o.get(), "value", value);
  return json_object_to_json_string_ext(o.get(), JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
}
} // namespace
std::vector<Field> parseFields(const std::string &json) {
  std::vector<Field> fields;
  Error error;
  const auto root = parseJson(json, error);
  auto *list = root ? field(root.get(), "fields") : nullptr;
  if (!list || !json_object_is_type(list, json_type_array)) return fields;
  for (size_t i = 0; i < json_object_array_length(list); ++i) {
    auto *o = json_object_array_get_idx(list, i);
    if (!json_object_is_type(o, json_type_object)) continue;
    Field f;
    f.key = stringField(o, "key");
    f.label = readLabel(field(o, "label"));
    const auto type = stringField(o, "type");
    auto *value = field(o, "value");
    if (f.key.empty()) continue;
    if (type == "toggle") {
      if (!value || !json_object_is_type(value, json_type_boolean)) continue;
      f.on = json_object_get_boolean(value);
    } else if (type == "choice") {
      f.type = Type::Choice;
      f.value = stringField(o, "value");
      auto *options = field(o, "options");
      if (options && json_object_is_type(options, json_type_array))
        for (size_t n = 0; n < json_object_array_length(options); ++n) {
          auto *p = json_object_array_get_idx(options, n);
          if (!json_object_is_type(p, json_type_object)) continue;
          Option option{stringField(p, "value"), readLabel(field(p, "label"))};
          if (!option.value.empty()) f.options.push_back(std::move(option));
        }
      if (f.options.empty()) continue;
    } else
      continue;
    fields.push_back(std::move(f));
  }
  return fields;
}
std::string text(const Label &label, const std::string &language, const std::string &fallback) {
  for (const auto &code : {language, std::string("en")})
    if (const auto found = label.find(code); found != label.end() && !found->second.empty()) return found->second;
  return fallback;
}
int optionIndex(const Field &field) {
  for (size_t i = 0; i < field.options.size(); ++i)
    if (field.options[i].value == field.value) return int(i);
  return 0;
}
std::string toggled(const Field &field) { return body(field.key, json_object_new_boolean(!field.on)); }
std::string stepped(const Field &field, int step) {
  const int count = int(field.options.size());
  if (!count) return "";
  const int at = ((optionIndex(field) + step) % count + count) % count;
  return body(field.key, json_object_new_string(field.options[at].value.c_str()));
}
std::vector<Field> applied(std::vector<Field> fields, const std::string &body) {
  Error error;
  const auto change = parseJson(body, error);
  if (!change) return fields;
  const auto key = stringField(change.get(), "key");
  auto *value = field(change.get(), "value");
  for (auto &f : fields)
    if (f.key == key && value && f.type == Type::Toggle && json_object_is_type(value, json_type_boolean))
      f.on = json_object_get_boolean(value);
    else if (f.key == key && value && f.type == Type::Choice && json_object_is_type(value, json_type_string))
      f.value = json_object_get_string(value);
  return fields;
}
} // namespace foil::admin
