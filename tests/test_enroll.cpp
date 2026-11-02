#include "Test.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "core/State.hpp"
#include "net/Api.hpp"
#include "net/Socket.hpp"
#include "MockServer.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <algorithm>
#include <utility>
#include <vector>
// A client downloaded from the website carries a one-time enroll token in
// config.json: spent once (POST /api/pair/enroll) for this console's access
// key before anything else, then taken out of the file. Refused, it says
// why and stays; a console with a key spends it too (to be the account's);
// a server that does not know enroll ignores it.
namespace {
bool has(const std::string &request, const std::string &text) { return request.find(text) != std::string::npos; }
int count(const std::vector<std::string> &requests, const std::string &text) {
  int n = 0;
  for (const auto &r : requests) n += has(r, text);
  return n;
}
} // namespace
int runEnrollTests() {
  int failures = 0;
  using namespace foil;
  Mock mock;
  int enrollStatus = 200, sectionsStatus = 200;
  std::string pairStatus = "approved", enrollReason = "invalid", pairLimit, enrollExtra;
  bool approveEnrolled = false; // the server approves the key it gave by enroll
  bool sessionTokens = false;   // the server offers session tokens
  bool oldEnded = false;        // the enroll ended the old key's token
  mock.answer = [&](const std::string &request) -> Reply {
    if (request.rfind("POST /api/pair/enroll", 0) == 0) {
      if (enrollStatus == 401 || enrollStatus == 403)
        return {enrollStatus, "X-CookingFoil-Enroll: " + enrollReason + "\r\n", R"({"error":"x"})"};
      if (enrollStatus != 200) return {enrollStatus, "", R"({"error":"x"})"};
      return {200, "", R"({"status":"approved","accessKey":"enrolled-key","libraryUrl":null)" + enrollExtra + "}"};
    }
    // The enroll ends the old key's session token.
    if (sessionTokens && has(request, "POST /api/pair/enroll")) oldEnded = true;
    if (oldEnded && has(request, "Bearer cfs_old") && !has(request, "/api/pair/enroll"))
      return {401, "X-CookingFoil-Auth: session-revoked\r\n", "{}"};
    if (request.rfind("GET /api/session", 0) == 0)
      return has(request, "X-Access-Key: enrolled-key") ? Reply{200, "", R"({"token":"cfs_new","expiresAt":0})"}
                                                        : Reply{200, "", R"({"token":"cfs_old","expiresAt":0})"};
    if (request.rfind("GET /api/library/info", 0) == 0)
      return {200, "", sessionTokens ? R"({"features":["enroll","session-token"]})" : R"({"features":["enroll"]})"};
    if (request.rfind("GET /api/pair/status", 0) == 0 && approveEnrolled && has(request, "X-Access-Key: enrolled-key"))
      return {200, "", R"({"status":"approved"})"};
    if (request.rfind("GET /api/pair/status", 0) == 0)
      return {200, "", R"({"status":")" + pairStatus + R"(")" + (pairLimit.empty() ? "" : R"(,"limit":")" + pairLimit + "\"") + "}"};
    if (request.rfind("GET /api/library/sections", 0) == 0)
      return sectionsStatus == 200 ? Reply{200, "", R"({"sections":[]})"} : Reply{sectionsStatus, "", R"({"error":"denied"})"};
    return {200, "", "{}"};
  };
  const auto original = mock.answer;
  char temporary[] = "/tmp/cookingfoil-enroll-XXXXXX";
  const char *made = mkdtemp(temporary);
  CHECK(made);
  if (!made) return failures;
  const std::filesystem::path root(made);
  const auto db = (root / "cookingfoil.db").string();
  const auto configPath = (root / "config.json").string();
  const std::string url = "http://127.0.0.1:" + std::to_string(mock.port);
  const std::string token = "cfe_" + std::string(43, 'a');
  const auto writeConfig = [&] {
    std::string written;
    CHECK(atomicWrite(configPath,
                      R"({"servers":[{"title":"Main","url":")" + url + R"(","username":"","password":"","enrollToken":")" + token +
                          R"(","cfClientId":"","cfClientSecret":"","enabled":true}],"language":"ko","installTarget":"sd"})",
                      written));
  };
  const auto server = [&] {
    auto config = loadConfig(configPath);
    CHECK(!config.error && config.value.servers.size() == 1);
    return config.value.servers.empty() ? Server{} : config.value.servers.front();
  };
  Error error;
  {
    // Token, no key: spent once, first; the key saved; the token out of
    // config.json; after it, no Basic (no username) and the access key.
    writeConfig();
    CHECK(server().enrollToken == token);
    net::Session::forgetEnrollAttempts();
    const auto state = loadState(db).value;
    const auto before = server();
    net::Session session(server(), state, db);
    session.setConfigPath(configPath);
    CHECK(session.refresh(error));
    const auto sent = mock.take();
    CHECK(!sent.empty() && sent.front().rfind("POST /api/pair/enroll", 0) == 0);
    CHECK(count(sent, "POST /api/pair/enroll") == 1);
    if (!sent.empty()) {
      CHECK(has(sent.front(), "X-Device-Key: " + state.derivedKey) && has(sent.front(), "\"token\":\"" + token + "\""));
      CHECK(has(sent.front(), "Content-Type: application/json"));
    }
    CHECK(loadState(db).value.accessKeys.at(url) == "enrolled-key");
    CHECK(server().enrollToken.empty() && readFile(configPath).find(token) == std::string::npos);
    for (size_t i = 1; i < sent.size(); ++i)
      CHECK(!has(sent[i], "Authorization: Basic") && has(sent[i], "X-Access-Key: enrolled-key"));
    {
      // A session made from the files as they were before (read while the
      // first was enrolling): the token is not sent twice, which the server
      // would refuse as used; the key it gave goes on.
      net::Session stale(before, state, db);
      stale.setConfigPath(configPath);
      CHECK(stale.refresh(error));
      const auto again = mock.take();
      CHECK(count(again, "/api/pair/enroll") == 0 && count(again, "X-Access-Key: enrolled-key") >= 1);
      CHECK(loadState(db).value.accessKeys.at(url) == "enrolled-key");
    }
    // With the key, never again (even with the token back in the file).
    writeConfig();
    net::Session again(server(), loadState(db).value, db);
    again.setConfigPath(configPath);
    CHECK(again.refresh(error));
    CHECK(count(mock.take(), "/api/pair/enroll") == 0);
    // A list refused with none of the server's own headers: the screen names
    // the host it went to, the log how it signed in and what came back.
    const auto logged = (root / "log.txt").string();
    openLog(logged);
    sectionsStatus = 401;
    net::Session refused(server(), loadState(db).value, db);
    refused.setConfigPath(configPath);
    CHECK(!refused.refresh(error));
    CHECK(error.message.find("401") != std::string::npos && error.message.find("127.0.0.1") != std::string::npos);
    flushLog();
    const auto line = readFile(logged);
    CHECK(line.find("list refused: HTTP 401, signed in by") != std::string::npos);
    CHECK(line.find("session header none, enroll header none, auth header none, error denied") != std::string::npos);
    sectionsStatus = 200;
    mock.take();
  }
  {
    // A list refused because the key is no good (X-CookingFoil-Auth): the
    // key goes and approval is asked again (the QR code), not "try again".
    // A refusal without that reason keeps the key: it may be something in
    // front of the server.
    const auto kept = mock.answer;
    std::string reason = "access-key-wrong";
    mock.answer = [&](const std::string &request) -> Reply {
      if (request.rfind("GET /api/pair/status", 0) == 0) return {200, "", R"({"status":")" + pairStatus + R"("})"};
      if (request.rfind("GET /api/library/sections", 0) == 0)
        return {401, reason.empty() ? "" : "X-CookingFoil-Auth: " + reason + "\r\n", "no"};
      return {200, "", "{}"};
    };
    auto plain = server();
    plain.enrollToken.clear();
    for (const std::string why : {"access-key-wrong", "device-not-approved"}) {
      auto state = loadState(db).value;
      state.accessKeys = {{url, "stale-key"}};
      CHECK(saveState(db, state, error));
      reason = why;
      pairStatus = "pending";
      net::Session session(plain, loadState(db).value, db);
      Error waiting;
      CHECK(!session.refresh(waiting) && session.pairingPending());
      CHECK(loadState(db).value.accessKeys.empty());
      CHECK(count(mock.take(), "POST /api/pair/request") == 1);
    }
    {
      // A session refused (its renewal failed too): the key signs in once
      // more and is kept; only what the key's own refusal says drops it.
      std::vector<std::string> reasons;
      mock.answer = [&](const std::string &request) -> Reply {
        if (request.rfind("GET /api/pair/status", 0) == 0) return {200, "", R"({"status":")" + pairStatus + R"("})"};
        if (request.rfind("GET /api/library/sections", 0) == 0) {
          if (reasons.empty()) return {200, "", R"({"sections":[]})"};
          const auto why = reasons.front();
          reasons.erase(reasons.begin());
          return {401, "X-CookingFoil-Auth: " + why + "\r\n", "no"};
        }
        return {200, "", "{}"};
      };
      auto state = loadState(db).value;
      state.accessKeys = {{url, "stale-key"}};
      CHECK(saveState(db, state, error));
      pairStatus = "approved";
      reasons = {"session-revoked"};
      {
        net::Session session(plain, loadState(db).value, db);
        CHECK(session.refresh(error) && !session.pairingPending());
        CHECK(loadState(db).value.accessKeys.at(url) == "stale-key");
        CHECK(count(mock.take(), "GET /api/library/sections") == 2);
      }
      reasons = {"session-revoked", "access-key-wrong"};
      pairStatus = "pending";
      {
        net::Session session(plain, loadState(db).value, db);
        Error waiting;
        CHECK(!session.refresh(waiting) && session.pairingPending());
        CHECK(loadState(db).value.accessKeys.empty());
        mock.take();
      }
      mock.answer = [&](const std::string &request) -> Reply {
        if (request.rfind("GET /api/pair/status", 0) == 0) return {200, "", R"({"status":")" + pairStatus + R"("})"};
        if (request.rfind("GET /api/library/sections", 0) == 0)
          return {401, reason.empty() ? "" : "X-CookingFoil-Auth: " + reason + "\r\n", "no"};
        return {200, "", "{}"};
      };
    }
    {
      auto state = loadState(db).value;
      state.accessKeys = {{url, "stale-key"}};
      CHECK(saveState(db, state, error));
      reason.clear();
      pairStatus = "approved";
      net::Session session(plain, loadState(db).value, db);
      CHECK(!session.refresh(error) && !session.pairingPending());
      CHECK(loadState(db).value.accessKeys.at(url) == "stale-key");
      mock.take();
    }
    pairStatus = "approved";
    auto restored = loadState(db).value;
    restored.accessKeys = {{url, "enrolled-key"}};
    restored.pairSecrets.clear();
    CHECK(saveState(db, restored, error));
    mock.answer = kept;
  }
  {
    // A key kept from before that the server no longer approves (the console
    // was removed): the token approves it again in place of a new request.
    auto state = loadState(db).value;
    state.accessKeys[url] = "removed-key";
    CHECK(saveState(db, state, error));
    writeConfig();
    net::Session::forgetEnrollAttempts();
    pairStatus = "pending";
    approveEnrolled = true;
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    CHECK(session.refresh(error));
    const auto sent = mock.take();
    CHECK(count(sent, "POST /api/pair/enroll") == 1 && count(sent, "/api/pair/request") == 0);
    CHECK(count(sent, "/api/library/sections") == 1 && !session.pairingPending());
    CHECK(loadState(db).value.accessKeys.at(url) == "enrolled-key");
    CHECK(server().enrollToken.empty());
    pairStatus = "approved";
    approveEnrolled = false;
  }
  {
    // A console the server approved on its own (auto, an admin, Telegram)
    // holds a key that is no one's: a download's token put in config.json
    // is spent all the same, so the console becomes that account's (the
    // website lists it and can send to it). Once, then out of the file.
    auto state = loadState(db).value;
    state.accessKeys[url] = "auto-key";
    CHECK(saveState(db, state, error));
    writeConfig();
    net::Session::forgetEnrollAttempts();
    const auto logged = (root / "bound.txt").string();
    openLog(logged);
    setLogLevel(LogLevel::All); // the lines below are detail
    enrollExtra = R"(,"bound":true,"owner":{"kind":"user","displayName":"Someone"})";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    CHECK(session.refresh(error));
    const auto sent = mock.take();
    CHECK(count(sent, "POST /api/pair/enroll") == 1 && !sent.empty() && has(sent.front(), "X-Access-Key: auto-key"));
    CHECK(loadState(db).value.accessKeys.at(url) == "enrolled-key" && server().enrollToken.empty());
    CHECK(session.enrollRefusal().empty());
    CHECK(session.refresh(error) && count(mock.take(), "/api/pair/enroll") == 0);
    flushLog();
    CHECK(readFile(logged).find("enroll: linked to the download's account") != std::string::npos);
    CHECK(readFile(logged).find("Someone") == std::string::npos);
    setLogLevel(LogLevel::Errors);
    enrollExtra.clear();
  }
  {
    // Another account's console, with a key held: it keeps connecting with
    // that key, the token leaves config.json, and the refusal is kept for
    // the screen to say why the website's sends will not come.
    auto state = loadState(db).value;
    state.accessKeys[url] = "auto-key";
    CHECK(saveState(db, state, error));
    writeConfig();
    net::Session::forgetEnrollAttempts();
    enrollStatus = 409;
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    CHECK(session.refresh(error) && !session.pairingPending());
    CHECK(count(mock.take(), "POST /api/pair/enroll") == 1);
    CHECK(loadState(db).value.accessKeys.at(url) == "auto-key" && server().enrollToken.empty());
    CHECK(session.enrollRefusal() == "other-account");
    enrollStatus = 200;
  }
  {
    // The server asks for the key held (403 needs-key): the token is not
    // spent and stays in config.json; the console goes on with its key,
    // the refusal kept for the screen, and it is not sent again this run.
    auto state = loadState(db).value;
    state.accessKeys[url] = "auto-key";
    CHECK(saveState(db, state, error));
    writeConfig();
    net::Session::forgetEnrollAttempts();
    enrollStatus = 403;
    enrollReason = "needs-key";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    CHECK(session.refresh(error) && !session.pairingPending());
    CHECK(count(mock.take(), "POST /api/pair/enroll") == 1);
    CHECK(loadState(db).value.accessKeys.at(url) == "auto-key" && server().enrollToken == token);
    CHECK(session.enrollRefusal() == "needs-key");
    CHECK(session.refresh(error) && count(mock.take(), "POST /api/pair/enroll") == 0);
    enrollStatus = 200;
    enrollReason = "invalid";
  }
  const auto withoutKey = [&] {
    auto state = loadState(db).value;
    state.accessKeys.clear();
    CHECK(saveState(db, state, error));
    writeConfig();
    net::Session::forgetEnrollAttempts();
  };
  {
    // 401 (expired, spent, unknown alike): the token is out of config.json
    // and the console asks for approval (the QR code) without it.
    withoutKey();
    enrollStatus = 401;
    pairStatus = "pending";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    Error waiting;
    CHECK(!session.refresh(waiting) && session.pairingPending());
    CHECK(server().enrollToken.empty() && loadState(db).value.accessKeys.empty());
    const auto sent = mock.take();
    CHECK(count(sent, "/api/pair/enroll") == 1 && count(sent, "POST /api/pair/request") == 1);
    for (const auto &request : sent)
      if (request.rfind("POST /api/pair/enroll", 0) != 0) CHECK(!has(request, token));
    CHECK(!session.refresh(waiting) && count(mock.take(), "/api/pair/enroll") == 0);
    pairStatus = "approved";
  }
  {
    // Past the account's limits: by download this hour (401 limit), said on
    // the approval screen; the account full (403 limit), where the approval
    // check says waiting will not do. Both ask for approval by QR code.
    for (const int status : {401, 403}) {
      withoutKey();
      enrollStatus = status;
      enrollReason = "limit";
      pairStatus = "pending";
      pairLimit = status == 403 ? "device" : "";
      net::Session session(server(), loadState(db).value, db);
      session.setConfigPath(configPath);
      Error waiting;
      CHECK(!session.refresh(waiting) && session.pairingPending());
      CHECK(server().enrollToken.empty());
      CHECK(session.pairingInfo().refusal == (status == 401 ? "limit" : ""));
      CHECK(session.pairingInfo().deviceLimit == (status == 403));
      mock.take();
    }
    enrollReason = "invalid";
    pairLimit.clear();
    pairStatus = "approved";
  }
  {
    // Another account's console (409): the QR code, and the token is out of
    // config.json, so a restart does not send it again.
    withoutKey();
    enrollStatus = 409;
    pairStatus = "pending";
    {
      net::Session session(server(), loadState(db).value, db);
      session.setConfigPath(configPath);
      Error waiting;
      CHECK(!session.refresh(waiting) && session.pairingPending());
      CHECK(session.pairingInfo().refusal == "other-account");
      CHECK(server().enrollToken.empty() && readFile(configPath).find(token) == std::string::npos);
      CHECK(count(mock.take(), "/api/pair/enroll") == 1);
    }
    net::Session::forgetEnrollAttempts(); // a new run remembers nothing it tried
    net::Session restarted(server(), loadState(db).value, db);
    restarted.setConfigPath(configPath);
    Error again;
    CHECK(!restarted.refresh(again) && restarted.pairingPending());
    CHECK(count(mock.take(), "/api/pair/enroll") == 0);
    pairStatus = "approved";
  }
  {
    // A server error of its own: the QR code shows all the same, never a
    // dead end. The token stays for the next start and is not sent again
    // this run.
    // Busy (429, 5xx) is asked again at the next refreshes, three times in
    // all; any other error once.
    for (const int status : {500, 503, 429, 400}) {
      withoutKey();
      enrollStatus = status;
      pairStatus = "pending";
      net::Session session(server(), loadState(db).value, db);
      session.setConfigPath(configPath);
      Error waiting;
      const int tries = status == 400 ? 1 : 3;
      for (int i = 0; i < tries; ++i) {
        CHECK(!session.refresh(waiting) && session.pairingPending());
        CHECK(session.pairingInfo().refusal.empty());
        CHECK(server().enrollToken == token);
        CHECK(count(mock.take(), "/api/pair/enroll") == 1);
      }
      CHECK(!session.refresh(waiting) && session.pairingPending());
      CHECK(count(mock.take(), "/api/pair/enroll") == 0);
    }
    pairStatus = "approved";
  }
  {
    // A server that does not know enroll: the token is ignored and the
    // console goes on as before.
    withoutKey();
    enrollStatus = 404;
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    Error none;
    CHECK(session.refresh(none));
    const auto sent = mock.take();
    CHECK(count(sent, "/api/pair/enroll") == 1 && count(sent, "/api/library/sections") == 1);
    CHECK(server().enrollToken == token);
    // Still pending there: the request carries the token, so the server
    // can approve it under the download's account.
    pairStatus = "pending";
    Error waiting;
    CHECK(!session.refresh(waiting) && session.pairingPending());
    bool carried = false;
    for (const auto &request : mock.take())
      if (request.rfind("POST /api/pair/request", 0) == 0) carried = has(request, "\"enrollToken\":\"" + token + "\"");
    CHECK(carried);
    pairStatus = "approved";
  }
  {
    // A console the server took back (its account off, its password
    // changed), with no enroll token: the kept key goes, and approval is
    // asked again with a new pair secret, so the key the server issues next
    // goes only to this console. Then the QR code shows.
    std::string written;
    CHECK(atomicWrite(configPath,
                      R"({"servers":[{"title":"Main","url":")" + url +
                          R"(","username":"","password":"","enrollToken":"","cfClientId":"","cfClientSecret":"","enabled":true}],"language":"ko","installTarget":"sd"})",
                      written));
    auto state = loadState(db).value;
    state.accessKeys[url] = "revoked-key";
    state.pairSecrets[url] = std::string(64, 'c'); // one the server no longer holds
    CHECK(saveState(db, state, error));
    pairStatus = "pending";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    Error waiting;
    CHECK(!session.refresh(waiting) && session.pairingPending());
    const auto kept = loadState(db).value;
    CHECK(!kept.accessKeys.count(url));
    const auto secret = kept.pairSecrets.count(url) ? kept.pairSecrets.at(url) : std::string();
    CHECK(secret.size() == 64 && secret != std::string(64, 'c'));
    const auto sent = mock.take();
    int asked = 0;
    for (const auto &request : sent)
      if (request.rfind("GET /api/pair/status", 0) == 0 && has(request, "X-Pair-Secret: " + secret)) {
        ++asked;
        CHECK(!has(request, "revoked-key"));
      }
    CHECK(asked == 1);
    CHECK(count(sent, "POST /api/pair/request") == 1);
    pairStatus = "approved";
  }
  {
    // An ID and password are for approval on a server that says
    // pair-password-only: sent to /api/pair/* alone, and once a key came,
    // taken out of config.json, so they no longer cross the network or sit
    // on the card. A server without the mark gets them as before.
    const auto withPassword = [&](bool only) {
      std::string written;
      CHECK(atomicWrite(configPath,
                        R"({"servers":[{"title":"Main","url":")" + url +
                            R"(","username":"alice","password":"secret-pass","enrollToken":"","cfClientId":"","cfClientSecret":"","enabled":true}],"language":"ko","installTarget":"sd"})",
                        written));
      auto state = loadState(db).value;
      state.accessKeys.clear();
      state.pairSecrets.clear();
      CHECK(saveState(db, state, error));
      mock.answer = [only](const std::string &request) -> Reply {
        if (request.rfind("GET /api/pair/status", 0) == 0)
          return only ? Reply{200, "", R"({"status":"approved","accessKey":"password-key","passwordOnly":true})"}
                      : Reply{404, "", "{}"};
        if (request.rfind("GET /api/library/info", 0) == 0)
          return {200, "", only ? R"({"features":["pair-password-only"]})" : R"({"features":[]})"};
        if (request.rfind("GET /api/library/sections", 0) == 0) return {200, "", R"({"sections":[]})"};
        return {200, "", "{}"};
      };
    };
    const auto basic = std::string("Authorization: Basic ");
    withPassword(true);
    {
      net::Session session(server(), loadState(db).value, db);
      session.setConfigPath(configPath);
      CHECK(session.refresh(error));
      const auto sent = mock.take();
      // The approval says passwordOnly itself: nothing after it carries
      // the password, /api/library/info included.
      for (const auto &request : sent) {
        const bool pairing = request.find(" /api/pair/") != std::string::npos;
        CHECK(has(request, basic) == pairing);
      }
      CHECK(count(sent, "GET /api/pair/status") >= 1);
      CHECK(loadState(db).value.accessKeys.at(url) == "password-key");
      CHECK(server().password.empty() && readFile(configPath).find("secret-pass") == std::string::npos);
      CHECK(server().username == "alice"); // kept: the form shows whose it was
      CHECK(session.server().password.empty());
      // Next time no password goes anywhere, the pairing paths included.
      CHECK(session.refresh(error));
      for (const auto &request : mock.take()) CHECK(!has(request, basic));
    }
    withPassword(false);
    {
      net::Session session(server(), loadState(db).value, db);
      session.setConfigPath(configPath);
      CHECK(session.refresh(error));
      mock.take();
      CHECK(session.refresh(error));
      CHECK(count(mock.take(), basic) >= 3); // a server with no pairing: Basic everywhere, as before
      CHECK(server().password == "secret-pass");
    }
  }
  {
    // The server says so on any reply: once, for the screen to refresh.
    mock.answer = [](const std::string &) -> Reply {
      return {401, "X-CookingFoil-Session: revoked\r\n", R"({"error":"session revoked"})"};
    };
    net::takeConsoleRevoked(url); // what tests before this one left
    net::get(url + "/api/library/sections", {}, "", nullptr);
    CHECK(net::takeConsoleRevoked(url) && !net::takeConsoleRevoked(url));
    mock.answer = [](const std::string &) -> Reply {
      return {401, "X-CookingFoil-Session: expired\r\n", "{}"};
    };
    net::get(url + "/api/library/sections", {}, "", nullptr);
    CHECK(!net::takeConsoleRevoked(url)); // expired is renewed, not a take-back
    mock.take();
  }
  {
    // The key the server gives once could not be written (another write
    // holds cookingfoil.db past every try): the console connects with it
    // all the same, and the next refresh writes it. Before, the key was
    // dropped and the server, having given it, left the console stuck.
    const auto kept = mock.answer;
    int keysGiven = 0;
    mock.answer = [&](const std::string &request) -> Reply {
      if (request.rfind("POST /api/pair/enroll", 0) == 0)
        return {200, "", R"({"status":"approved","accessKey":"enrolled-key","libraryUrl":null)" + enrollExtra + "}"};
      if (request.rfind("GET /api/library/info", 0) == 0) return {200, "", R"({"features":["enroll"]})"};
      if (request.rfind("GET /api/pair/status", 0) == 0)
        return keysGiven++ == 0 ? Reply{200, "", R"({"status":"approved","accessKey":"issued-key"})"}
                                : Reply{200, "", R"({"status":"approved"})"};
      if (request.rfind("GET /api/library/sections", 0) == 0) return {200, "", R"({"sections":[]})"};
      return {200, "", "{}"};
    };
    const auto lockPath = db + ".lock";
    const auto holdLock = [&] {
      std::string written;
      CHECK(atomicWrite(lockPath, std::to_string((long long)std::time(nullptr)) + " 1 1\n", written));
    };
    const auto keyless = [&] {
      auto state = loadState(db).value;
      state.accessKeys.erase(url);
      state.pairSecrets[url] = "secret-made-before"; // no secret to write first
      Error saved;
      CHECK(saveState(db, state, saved));
      return state;
    };
    const auto noToken = [&] {
      std::string written;
      CHECK(atomicWrite(configPath,
                        R"({"servers":[{"title":"Main","url":")" + url +
                            R"(","username":"","password":"","enrollToken":"","cfClientId":"","cfClientSecret":"","enabled":true}],"language":"ko","installTarget":"sd"})",
                        written));
    };
    const auto logged = (root / "log.txt").string();
    openLog(logged);
    setLogLevel(LogLevel::All); // the held key's lines are detail
    noToken();
    {
      net::Session session(server(), keyless(), db);
      session.setConfigPath(configPath);
      mock.take();
      holdLock();
      CHECK(session.refresh(error));
      CHECK(!error);
      const auto sent = mock.take();
      CHECK(count(sent, "GET /api/library/sections") == 1 && count(sent, "X-Access-Key: issued-key") >= 1);
      CHECK(!loadState(db).value.accessKeys.count(url));
      flushLog();
      CHECK(readFile(logged).find("key not saved (try 3/3)") != std::string::npos);
      CHECK(readFile(logged).find("key held for this run") != std::string::npos);
      // The lock let go: the next refresh writes the held key.
      std::filesystem::remove(lockPath);
      CHECK(session.refresh(error));
      CHECK(loadState(db).value.accessKeys.count(url) && loadState(db).value.accessKeys.at(url) == "issued-key");
      CHECK(!loadState(db).value.pairSecrets.count(url));
      flushLog();
      CHECK(readFile(logged).find("the held key is written now") != std::string::npos);
      setLogLevel(LogLevel::Errors);
      CHECK(keysGiven == 2);
    }
    {
      // Let go between tries: written by a later try of the same refresh.
      keysGiven = 0;
      net::Session session(server(), keyless(), db);
      session.setConfigPath(configPath);
      holdLock();
      std::thread freeing([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        std::filesystem::remove(lockPath);
      });
      CHECK(session.refresh(error));
      freeing.join();
      CHECK(loadState(db).value.accessKeys.count(url) && loadState(db).value.accessKeys.at(url) == "issued-key");
      mock.take();
    }
    {
      // From the enroll token: the token stays in config.json until the key
      // is written, then leaves; it is not sent twice meanwhile.
      keysGiven = 1; // approval gives none here: the key comes from the token
      writeConfig();
      net::Session::forgetEnrollAttempts();
      net::Session session(server(), keyless(), db);
      session.setConfigPath(configPath);
      mock.take();
      holdLock();
      CHECK(session.refresh(error));
      auto sent = mock.take();
      CHECK(count(sent, "POST /api/pair/enroll") == 1 && count(sent, "X-Access-Key: enrolled-key") >= 1);
      CHECK(!loadState(db).value.accessKeys.count(url));
      CHECK(server().enrollToken == token);
      std::filesystem::remove(lockPath);
      CHECK(session.refresh(error));
      sent = mock.take();
      CHECK(count(sent, "POST /api/pair/enroll") == 0);
      CHECK(loadState(db).value.accessKeys.count(url) && loadState(db).value.accessKeys.at(url) == "enrolled-key");
      CHECK(server().enrollToken.empty());
    }
    std::filesystem::remove(lockPath);
    mock.answer = kept;
    mock.take();
  }
  {
    // A username still signs in with Basic, as before; none sends none.
    Server upstream;
    upstream.url = url;
    State state;
    state.deviceKey = std::string(64, 'a');
    const auto basic = [&](const Server &s) {
      for (const auto &h : net::authHeaders(s, state, url + "/api/library/sections"))
        if (h.rfind("Authorization: Basic", 0) == 0) return true;
      return false;
    };
    upstream.password = "only-a-password";
    CHECK(!basic(upstream));
    upstream.username = "user";
    CHECK(basic(upstream));
  }
  {
    // Enrolled while a session token of the old key is held: that token and
    // the ask behind it go at once, so nothing after the enroll is sent with
    // them (a 401 session-revoked, then access-key-wrong, right after it).
    auto state = loadState(db).value;
    state.accessKeys[url] = "auto-key";
    CHECK(saveState(db, state, error));
    mock.answer = original;
    enrollStatus = sectionsStatus = 200;
    pairStatus = "approved";
    approveEnrolled = false;
    sessionTokens = true;
    {
      net::Session before(server(), loadState(db).value, db);
      CHECK(before.refresh(error) && before.refresh(error));
      CHECK(count(mock.take(), "Bearer cfs_old") > 0);
    }
    writeConfig();
    net::Session::forgetEnrollAttempts();
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    CHECK(session.refresh(error) && session.refresh(error));
    const auto sent = mock.take();
    const auto enrolled = std::find_if(sent.begin(), sent.end(), [](const std::string &r) { return has(r, "POST /api/pair/enroll"); });
    CHECK(enrolled != sent.end());
    for (auto after = enrolled == sent.end() ? sent.end() : enrolled + 1; after != sent.end(); ++after)
      CHECK(!has(*after, "Bearer cfs_old") && !has(*after, "X-Access-Key: auto-key"));
    CHECK(count(sent, "Bearer cfs_new") > 0);
    sessionTokens = oldEnded = false;
  }
  std::filesystem::remove_all(root);
  return failures;
}
