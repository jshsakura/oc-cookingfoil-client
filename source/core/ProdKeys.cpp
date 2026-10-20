#include "core/ProdKeys.hpp"
#include "core/Achievements.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "platform/Seal.hpp"
#include <cctype>
#include <sstream>
namespace foil::prodkeys {
namespace {
std::string base64Url(const std::string &bytes) {
  static const char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  uint32_t bits = 0;
  int held = 0;
  for (const unsigned char c : bytes) {
    bits = (bits << 8) | c;
    held += 8;
    while (held >= 6) {
      held -= 6;
      out += Alphabet[(bits >> held) & 63];
    }
  }
  if (held) out += Alphabet[(bits << (6 - held)) & 63];
  return out;
}
} // namespace
Status parseStatus(const std::string &json) {
  Status s;
  Error ignored;
  const auto root = parseJson(json, ignored);
  if (!root) return s;
  auto *present = field(root.get(), "present");
  s.present = present && json_object_get_boolean(present);
  s.source = stringField(root.get(), "source");
  s.uploadedAt = integerField(root.get(), "uploadedAt", 0);
  s.keyCount = int(integerField(root.get(), "keyCount", 0));
  s.titleKekCount = int(integerField(root.get(), "titleKekCount", 0));
  auto *header = field(root.get(), "hasHeaderKey");
  s.hasHeaderKey = header && json_object_get_boolean(header);
  return s;
}
Offer parseOffer(const std::string &json) {
  Offer o;
  Error ignored;
  const auto root = parseJson(json, ignored);
  if (!root) return o;
  o.publicKey = stringField(root.get(), "publicKey");
  o.nonce = stringField(root.get(), "nonce");
  o.expiresAt = integerField(root.get(), "expiresAt", 0);
  return o;
}
Parsed<std::string> sealBody(const Offer &offer, const std::string &keysText) {
  const Error broken{tr("The server's key for prod.keys was not valid. Try again.")};
  seal::Key mine;
  if (offer.nonce.empty() || !seal::generate(mine)) return {{}, broken};
  const auto shared = seal::sharedX(mine.secret, achievements::decodeBase64Url(offer.publicKey));
  const auto iv = seal::random(12);
  std::string sealed;
  if (shared.empty() || iv.size() != 12 ||
      !seal::close(achievements::hkdf(shared, "cookingfoil-prod-keys"), iv, offer.nonce, keysText, sealed))
    return {{}, broken};
  Json body(json_object_new_object());
  json_object_object_add(body.get(), "nonce", json_object_new_string(offer.nonce.c_str()));
  json_object_object_add(body.get(), "epk", json_object_new_string(base64Url(mine.point).c_str()));
  json_object_object_add(body.get(), "iv", json_object_new_string(base64Url(iv).c_str()));
  json_object_object_add(body.get(), "ct", json_object_new_string(base64Url(sealed).c_str()));
  return {json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN), {}};
}
bool looksLikeKeys(const std::string &text) {
  std::istringstream lines(text);
  std::string line;
  bool header = false;
  int keys = 0;
  while (std::getline(lines, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    auto name = line.substr(0, eq);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    ++keys;
    if (name == "header_key") header = true;
  }
  return header && keys >= 2;
}
} // namespace foil::prodkeys
