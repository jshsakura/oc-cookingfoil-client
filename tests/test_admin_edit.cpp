// 관리자 편집: the fields the server lets an admin console change, and what
// a change sends back.
#include "Test.hpp"
#include "core/AdminEdit.hpp"
namespace ad = foil::admin;
int runAdminEditTests() {
  int failures = 0;
  const auto fields = ad::parseFields(
      R"({"fields":[)"
      R"({"key":"verified","type":"toggle","label":{"ko":"검수됨","en":"Verified"},"value":true},)"
      R"({"key":"kind","type":"choice","label":{"en":"Kind"},"value":"port",)"
      R"("options":[{"value":"tool","label":{"ko":"도구","en":"Tool"}},{"value":"port","label":{"en":"Port"}}]},)"
      R"({"key":"notes","type":"text","label":{"en":"Notes"},"value":"x"},)"
      R"({"key":"empty","type":"choice","label":{"en":"Empty"},"value":"a","options":[]},)"
      R"({"key":"","type":"toggle","label":{"en":"No key"},"value":true},)"
      R"({"key":"loose","type":"toggle","label":{"en":"Loose"},"value":"yes"}]})");
  // Only the toggles and choices a console can change; a text field (the web
  // edits those), a choice with nothing to pick and a field without a key or
  // a real boolean are left out.
  CHECK(fields.size() == 2);
  if (fields.size() == 2) {
    CHECK(fields[0].key == "verified" && fields[0].type == ad::Type::Toggle && fields[0].on);
    CHECK(fields[1].key == "kind" && fields[1].type == ad::Type::Choice && fields[1].value == "port");
    CHECK(ad::optionIndex(fields[1]) == 1);
    // The label in the console's language, else English, else the key.
    CHECK(ad::text(fields[0].label, "ko", fields[0].key) == "검수됨");
    CHECK(ad::text(fields[1].label, "ko", fields[1].key) == "Kind");
    CHECK(ad::text({}, "ko", "kind") == "kind");
    CHECK(ad::text(fields[1].options[0].label, "ja", "tool") == "Tool");
    // A toggle sends its new state; a choice the option's value, wrapping.
    CHECK(ad::toggled(fields[0]) == R"({"key":"verified","value":false})");
    CHECK(ad::stepped(fields[1], 1) == R"({"key":"kind","value":"tool"})");
    CHECK(ad::stepped(fields[1], -1) == R"({"key":"kind","value":"tool"})");
  }
  // The preview applies a change as the server would.
  if (fields.size() == 2) {
    const auto after = ad::applied(fields, ad::toggled(fields[0]));
    CHECK(!after[0].on && after[1].value == "port");
    CHECK(ad::applied(fields, ad::stepped(fields[1], 1))[1].value == "tool");
    CHECK(ad::applied(fields, "not json")[0].on);
  }
  // A value no option has: the first option stands in for it.
  const auto odd = ad::parseFields(
      R"({"fields":[{"key":"core","type":"choice","label":{"en":"Core"},"value":"gone",)"
      R"("options":[{"value":"a","label":{"en":"A"}},{"value":"b","label":{"en":"B"}}]}]})");
  CHECK(odd.size() == 1 && ad::optionIndex(odd[0]) == 0 && ad::stepped(odd[0], 1) == R"({"key":"core","value":"b"})");
  // Not JSON, or no list: nothing to edit.
  CHECK(ad::parseFields("not json").empty());
  CHECK(ad::parseFields(R"({"fields":{}})").empty());
  // Keys and values with quotes go out escaped.
  ad::Field quoted;
  quoted.key = "a\"b";
  quoted.type = ad::Type::Toggle;
  CHECK(ad::toggled(quoted) == R"({"key":"a\"b","value":true})");
  return failures;
}
