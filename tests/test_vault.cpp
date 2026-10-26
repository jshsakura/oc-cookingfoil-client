#include "Test.hpp"
#include "core/DownloadStore.hpp"
#include "core/Json.hpp"
#include "core/Sha256.hpp"
#include "core/State.hpp"
#include "core/Vault.hpp"
#include "net/Dropbox.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
// Credentials at rest: encrypted under a key only this console can make
// (ChaCha20, then HMAC-SHA256 over it), so the card read on a PC or in
// another console shows nothing usable. ChaCha20 against RFC 8439's
// published vector.
namespace {
std::string bytes(const std::string &hex) { return foil::hexBytes(hex); }
std::string hexOf(const std::string &raw) {
  static const char *digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : raw) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}
std::string fileText(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
// The whole database as it sits on the card, the journal too.
std::string onCard(const std::string &db) { return fileText(db) + fileText(db + "-wal"); }
} // namespace
int runVaultTests() {
  int failures = 0;
  using namespace foil;
  {
    // RFC 8439 2.4.2: the "sunscreen" text under key 00..1f, nonce
    // 000000000000004a00000000, counter 1.
    std::string key;
    for (int i = 0; i < 32; ++i) key += char(i);
    const auto nonce = bytes("000000000000004a00000000");
    const std::string plain = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
                              "future, sunscreen would be it.";
    const auto cipher = vault::chacha20(key, nonce, 1, plain);
    CHECK(hexOf(cipher) == "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dab"
                           "cd62b3571639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806"
                           "818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d");
    CHECK(vault::chacha20(key, nonce, 1, cipher) == plain);
    CHECK(vault::chacha20(key.substr(1), nonce, 1, plain).empty() && vault::chacha20(key, "short", 1, plain).empty());
  }
  {
    // Sealed and opened again; any change, or another key, opens nothing.
    const std::string key(32, 'k'), other(32, 'o'), nonce(12, 'n');
    const std::string secret = "refresh-token-0123456789";
    const auto text = vault::seal(key, secret, nonce);
    CHECK(vault::sealed(text) && text.find(secret) == std::string::npos);
    CHECK(vault::open(key, text) == secret);
    CHECK(vault::open(key, vault::seal(key, "", nonce)) == std::string());
    CHECK(!vault::open(other, text));
    const auto tagAt = text.rfind(':'), cipherAt = text.find(':', 7);
    for (size_t at : {size_t(8), cipherAt + 1, cipherAt + 3, tagAt + 1, text.size() - 1}) {
      auto changed = text;
      changed[at] = changed[at] == '0' ? '1' : '0';
      CHECK(!vault::open(key, changed));
    }
    CHECK(!vault::open(key, text.substr(0, text.size() - 2)) && !vault::open(key, "plain") && !vault::open(key, ""));
    CHECK(!vault::open(key, text + "00") && !vault::open(key.substr(1), text));
    // A new nonce, another text: the same value never reads the same twice.
    CHECK(vault::seal(key, secret, std::string(12, 'm')) != text);
  }
  char temporary[] = "/tmp/cookingfoil-vault-XXXXXX";
  const char *made = mkdtemp(temporary);
  CHECK(made);
  if (!made) return failures;
  const std::filesystem::path root(made);
  const auto db = (root / "cookingfoil.db").string();
  const auto here = deriveIdentity(0x1111222233334444ull), there = deriveIdentity(0x5555666677778888ull);
  const std::string Remote = "https://server.example";
  setDeviceIdentity(here);
  Error error;
  {
    // On the card the keys are ciphertext; this console reads them back.
    auto state = loadState(db).value;
    state.accessKeys[Remote] = "access-key-plain-AAAA";
    state.pairSecrets["https://other.example"] = std::string(64, 'c');
    CHECK(saveState(db, state, error));
    const auto raw = onCard(db);
    CHECK(raw.find("access-key-plain-AAAA") == std::string::npos && raw.find(std::string(64, 'c')) == std::string::npos);
    CHECK(raw.find(state.deviceKey) == std::string::npos && raw.find("enc:v1:") != std::string::npos);
    const auto back = loadState(db).value;
    CHECK(takeRebound() == Rebound::None && back.accessKeys.at(Remote) == "access-key-plain-AAAA" && back.deviceKey == state.deviceKey);
    // The Dropbox link: the same, beside them.
    CHECK(saveDropbox(db, R"({"refresh_token":"dropbox-refresh-BBBB"})", error));
    CHECK(onCard(db).find("dropbox-refresh-BBBB") == std::string::npos);
    CHECK(loadDropbox(db) == std::string(R"({"refresh_token":"dropbox-refresh-BBBB"})"));
    // A save of the other credentials keeps the link.
    CHECK(saveState(db, back, error) && loadDropbox(db).has_value());
  }
  {
    // A ciphertext changed on a PC (the seal made again to match would need
    // the console too): taken as edited, made again, never read.
    DownloadStore store;
    std::string failure;
    CHECK(store.open(db, failure));
    auto rows = store.stateRows(failure);
    CHECK(rows.has_value());
    if (rows) {
      auto &value = rows->credentials[{"access", Remote}];
      value[value.size() - 1] = value.back() == '0' ? '1' : '0';
      // Even with the seal made again (as only this console could), the
      // tag does not match: the value does not open.
      rows->credentials.erase({"seal", ""});
      std::string sealedText;
      for (const auto &[at, v] : rows->credentials) sealedText += at.first + '\0' + at.second + '\0' + v + '\n';
      rows->credentials[{"seal", ""}] = hmacSha256(here.sealKey, sealedText);
      CHECK(store.saveStateRows(*rows, failure));
    }
    const auto edited = loadState(db).value;
    CHECK(takeRebound() == Rebound::Changed && edited.accessKeys.empty() && !edited.deviceKey.empty());
  }
  {
    // The card in another console: nothing opens, nothing crashes; the
    // credentials and the Dropbox link are made again (approve again, link
    // again), and the settings stay.
    auto state = loadState(db).value;
    state.accessKeys[Remote] = "issued-here";
    state.language = "ja";
    CHECK(saveState(db, state, error) && saveDropbox(db, R"({"refresh_token":"here-only"})", error));
    setDeviceIdentity(there);
    CHECK(!loadDropbox(db));
    const auto moved = loadState(db).value;
    CHECK(takeRebound() == Rebound::Moved && moved.accessKeys.empty() && moved.language == "ja");
    CHECK(moved.deviceKey == there.deviceKey && !loadDropbox(db));
    CHECK(onCard(db).find("here-only") == std::string::npos);
    setDeviceIdentity(here);
  }
  {
    // Rows from before encryption (plain, sealed as they were): read once
    // and encrypted with the console's key.
    std::filesystem::remove(db);
    std::filesystem::remove(db + "-wal");
    std::filesystem::remove(db + "-shm");
    loadState(db);
    takeRebound();
    DownloadStore store;
    std::string failure;
    CHECK(store.open(db, failure));
    auto rows = store.stateRows(failure);
    CHECK(rows.has_value());
    if (rows) {
      rows->credentials[{"device", ""}] = here.deviceKey;
      rows->credentials[{"access", Remote}] = "legacy-plain-CCCC";
      rows->credentials.erase({"seal", ""});
      std::string sealedText;
      for (const auto &[at, v] : rows->credentials) sealedText += at.first + '\0' + at.second + '\0' + v + '\n';
      rows->credentials[{"seal", ""}] = hmacSha256(here.sealKey, sealedText);
      CHECK(store.saveStateRows(*rows, failure));
    }
    CHECK(onCard(db).find("legacy-plain-CCCC") != std::string::npos); // as an earlier build left it
    const auto read = loadState(db).value;
    CHECK(takeRebound() == Rebound::None && read.accessKeys.at(Remote) == "legacy-plain-CCCC");
    auto again = store.stateRows(failure);
    CHECK(again && vault::sealed(again->credentials[{"access", Remote}]) && vault::sealed(again->credentials[{"device", ""}]));
    CHECK(loadState(db).value.accessKeys.at(Remote) == "legacy-plain-CCCC");
    // And the plain text is gone from the card, not left in freed pages.
    CHECK(onCard(db).find("legacy-plain-CCCC") == std::string::npos);
  }
  {
    // dropbox.json of earlier builds: moved into the database encrypted, the
    // file gone, and read from there after.
    const auto legacy = (root / "dropbox.json").string();
    std::string written;
    CHECK(atomicWrite(legacy,
                      R"({"access_token":"old-access-DDDD","refresh_token":"old-refresh-EEEE","expires_at":1,"client_id":"cid","account":"me"})",
                      written));
    {
      net::Dropbox dropbox(db);
      CHECK(dropbox.load() && dropbox.account() == "me");
    }
    CHECK(!std::filesystem::exists(legacy));
    const auto raw = onCard(db);
    CHECK(raw.find("old-refresh-EEEE") == std::string::npos && raw.find("old-access-DDDD") == std::string::npos);
    {
      net::Dropbox dropbox(db);
      CHECK(dropbox.load() && dropbox.account() == "me");
      CHECK(!dropbox.unlink() && !loadDropbox(db));
    }
    net::Dropbox none(db);
    CHECK(!none.load());
  }
  std::filesystem::remove_all(root);
  setDeviceIdentity(deriveIdentity(0x756e697474657374ull));
  return failures;
}
