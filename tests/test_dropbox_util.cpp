// Dropbox helper tests ported from oc-save-keeper (MIT): test_dropbox_util
// and test_security_dropbox_ssrf, plus the bridge checks added here.
#include "Test.hpp"
#include "net/DropboxUtil.hpp"
namespace db = foil::net::dropbox;
namespace {
const std::string Authorize = "https://www.dropbox.com/oauth2/authorize";
int urlTests() {
  int failures = 0;
  {
    const auto url = db::buildAuthorizeUrl(Authorize, "client-key", "", "csrf-token", "challenge123");
    CHECK(url.find(Authorize + "?") == 0);
    for (const char *part : {"client_id=client-key", "response_type=code", "token_access_type=offline",
                             "code_challenge_method=S256", "code_challenge=challenge123", "state=csrf-token"})
      CHECK(url.find(part) != std::string::npos);
    CHECK(url.find("redirect_uri=") == std::string::npos);
  }
  {
    const auto url = db::buildAuthorizeUrl(Authorize, "client key", "https://example.com/complete?from=console",
                                           "csrf token", "challenge+/=");
    CHECK(url.find("client_id=client%20key") != std::string::npos);
    CHECK(url.find("redirect_uri=https%3A%2F%2Fexample.com%2Fcomplete%3Ffrom%3Dconsole") != std::string::npos);
    CHECK(url.find("state=csrf%20token") != std::string::npos);
    CHECK(url.find("code_challenge=challenge%2B%2F%3D") != std::string::npos);
  }
  CHECK(db::buildAuthorizeUrl(Authorize, "k", "https://example.com/complete", "s", "c")
            .find("redirect_uri=https%3A%2F%2Fexample.com%2Fcomplete") != std::string::npos);
  CHECK(db::parseDropboxTime("2026-03-11T12:34:56Z") == 1773232496);
  CHECK(db::parseDropboxTime("1970-01-01T00:00:00Z") == 0);
  CHECK(db::parseDropboxTime(nullptr) == 0);
  CHECK(db::parseDropboxTime("") == 0);
  CHECK(db::parseDropboxTime("not-a-time") == 0);
  CHECK(db::parseDropboxTime("2026-13-11T12:34:56Z") == 0);
  CHECK(db::extractQueryParam("code=ABC123&state=xyz", "code") == "ABC123");
  CHECK(db::extractQueryParam("code=ABC123&state=xyz", "state") == "xyz");
  CHECK(db::extractQueryParam("code=ABC123#", "code") == "ABC123");
  CHECK(db::extractQueryParam("https://example.com/complete?code=ABC%20123&state=xyz%20value", "code") == "ABC 123");
  CHECK(db::extractQueryParam("https://example.com/complete?code=ABC123&state=xyz", "state") == "xyz");
  CHECK(db::extractQueryParam("foo=bar", "missing").empty());
  CHECK(db::extractQueryParam("", "code").empty());
  CHECK(db::extractAuthorizationCode("code=XYZ789&state=abc") == "XYZ789");
  CHECK(db::extractAuthorizationCode("https://example.com/complete?code=XYZ%20789&state=abc") == "XYZ 789");
  CHECK(db::extractAuthorizationCode("plain-code-value") == "plain-code-value");
  CHECK(db::extractAuthorizationCode(" plain-code-value ") == "plain-code-value");
  CHECK(db::extractAuthorizationCode("").empty());
  CHECK(db::extractAuthorizationCode("state=abc&code=FIRST") == "FIRST");
  CHECK(db::extractAuthorizationCode("state=abc").empty());
  CHECK(db::isSuccessfulHttpStatus(200) && db::isSuccessfulHttpStatus(204) && db::isSuccessfulHttpStatus(299));
  CHECK(!db::isSuccessfulHttpStatus(199) && !db::isSuccessfulHttpStatus(300));
  CHECK(!db::isSuccessfulHttpStatus(401) && !db::isSuccessfulHttpStatus(500));
  CHECK(db::percentDecode(db::percentEncode("a b/c?d=e&f")) == "a b/c?d=e&f");
  CHECK(db::constantTimeEqual("abc", "abc") && !db::constantTimeEqual("abc", "abd"));
  CHECK(!db::constantTimeEqual("abc", "ab"));
  return failures;
}
int requestTests() {
  int failures = 0;
  CHECK(db::buildListFolderRequest("/apps/oc-save-keeper") ==
        "{\"path\":\"/apps/oc-save-keeper\",\"recursive\":false,\"include_deleted\":false}");
  CHECK(db::buildListFolderRequest("") == "{\"path\":\"\",\"recursive\":false,\"include_deleted\":false}");
  CHECK(db::buildListFolderRequest("/") == "{\"path\":\"/\",\"recursive\":false,\"include_deleted\":false}");
  CHECK(db::buildListFolderRequest("/games/Quote\"Test") ==
        "{\"path\":\"/games/Quote\\\"Test\",\"recursive\":false,\"include_deleted\":false}");
  CHECK(db::buildListFolderRequest("/apps/oc-save-keeper", true) ==
        "{\"path\":\"/apps/oc-save-keeper\",\"recursive\":true,\"include_deleted\":false}");
  const std::string quoted = "/games/Quote\"Test";
  CHECK(db::buildUploadArg(quoted) ==
        "{\"path\":\"/games/Quote\\\"Test\",\"mode\":\"overwrite\",\"autorename\":false}");
  CHECK(db::buildDownloadArg(quoted) == "{\"path\":\"/games/Quote\\\"Test\"}");
  CHECK(db::buildCreateFolderRequest(quoted) == "{\"path\":\"/games/Quote\\\"Test\"}");
  CHECK(db::buildDeleteRequest(quoted) == "{\"path\":\"/games/Quote\\\"Test\"}");
  CHECK(db::escapeJsonString(std::string("a\x01" "b\n")) == "a\\u0001b\\n");
  CHECK(db::parentPath("/games/Zelda/latest.zip") == "/games/Zelda");
  CHECK(db::fileName("/games/Zelda/latest.zip") == "latest.zip");
  CHECK(db::parentPath("latest.zip").empty() && db::fileName("latest.zip") == "latest.zip");
  CHECK(db::parentPath("/latest.zip") == "/" && db::fileName("/latest.zip") == "latest.zip");
  CHECK(db::parentPath("/games/Zelda/") == "/games/Zelda" && db::fileName("/games/Zelda/").empty());
  CHECK(!db::hasFileComponent("") && !db::hasFileComponent("/") && !db::hasFileComponent("/games/Zelda/"));
  CHECK(db::hasFileComponent("/games/Zelda/latest.zip"));
  const uint64_t tid = 0x01006A800016E000ULL;
  CHECK(db::titleFolder(tid) == "/titles/01006A800016E000");
  CHECK(db::latestZipPath(tid) == "/titles/01006A800016E000/latest.zip");
  CHECK(db::latestMetaPath(tid) == "/titles/01006A800016E000/latest.meta");
  CHECK(db::revisionsFolder(tid) == "/titles/01006A800016E000/revisions");
  CHECK(db::revisionZipPath(tid, "20260311_090000") == "/titles/01006A800016E000/revisions/20260311_090000.zip");
  CHECK(db::revisionMetaPath(tid, "20260311_090000") == "/titles/01006A800016E000/revisions/20260311_090000.meta");
  for (const char *bad : {"", ".", "..", "../x", "a/b", "a\\b", "c:x", "a\nb"}) {
    CHECK(db::revisionZipPath(tid, bad).empty());
    CHECK(db::revisionMetaPath(tid, bad).empty());
  }
  return failures;
}
int pkceTests() {
  int failures = 0;
  CHECK(db::generateCodeVerifier(43).size() == 43);
  CHECK(db::generateCodeVerifier(128).size() == 128);
  CHECK(db::generateCodeVerifier(10).size() == 43);
  CHECK(db::generateCodeVerifier(200).size() == 128);
  const auto verifier = db::generateCodeVerifier(64);
  CHECK(verifier.size() == 64);
  CHECK(verifier.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~") ==
        std::string::npos);
  CHECK(db::generateRandomHex(16).size() == 32);
  const auto encoded = db::base64UrlEncode({0x03, 0x9f, 0xd7, 0x2e, 0xb3, 0xfb, 0xff});
  CHECK(encoded.find_first_of("+/=") == std::string::npos);
  CHECK(encoded == "A5_XLrP7_w");
  CHECK(db::base64UrlEncode({}).empty());
  CHECK(db::base64UrlEncode({'f'}) == "Zg" && db::base64UrlEncode({'f', 'o'}) == "Zm8");
  // RFC 7636 appendix B.
  CHECK(db::buildCodeChallengeS256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
        "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
  return failures;
}
int ssrfTests() {
  int failures = 0;
  const std::string base = "https://bridge.example.com";
  CHECK(db::isUrlPrefixValid("https://bridge.example.com/v1/sessions/abc/status", base));
  CHECK(db::isUrlPrefixValid("https://bridge.example.com/", base));
  CHECK(!db::isUrlPrefixValid("https://evil.com/v1/sessions/abc/status", base));
  CHECK(!db::isUrlPrefixValid("http://bridge.example.com/v1/sessions", base));
  CHECK(!db::isUrlPrefixValid("https://bridge.example.com.evil.com/v1", base));
  CHECK(!db::isUrlPrefixValid("", base) && !db::isUrlPrefixValid("https://bridge.example.com/v1", ""));
  CHECK(!db::isUrlPrefixValid("", ""));
  const std::string session = "session-123";
  const std::string fallback = "https://bridge.example.com/v1/sessions/session-123/status";
  CHECK(db::buildSafePollUrl(fallback, base, session) == fallback);
  CHECK(db::buildSafePollUrl("", base, session) == fallback);
  for (const char *attack :
       {"https://evil.com/v1/sessions/session-123/status", "http://localhost/admin", "http://127.0.0.1/admin",
        "http://192.168.1.1/admin", "http://10.0.0.1/admin", "http://169.254.169.254/latest/meta-data",
        "http://bridge.example.com/v1/sessions/session-123/status", "https://bridge.example.com.evil.com/v1",
        "https://bridge.example.org/v1/sessions/session-123/status",
        "bridge.example.com/v1/sessions/session-123/status"})
    CHECK(db::buildSafePollUrl(attack, base, session) == fallback);
  return failures;
}
int bridgeTests() {
  int failures = 0;
  CHECK(db::isAllowedTokenEndpoint("https://api.dropboxapi.com/oauth2/token"));
  for (const char *bad :
       {"", "http://api.dropboxapi.com/oauth2/token", "https://api.dropboxapi.com/oauth2/token/",
        "https://api.dropboxapi.com/oauth2/token?x=1", "https://api.dropboxapi.com.evil.com/oauth2/token",
        "https://evil.com/https://api.dropboxapi.com/oauth2/token", "https://API.dropboxapi.com/oauth2/token",
        " https://api.dropboxapi.com/oauth2/token"})
    CHECK(!db::isAllowedTokenEndpoint(bad));
  CHECK(db::clientIdFromAuthorizeUrl(Authorize + "?client_id=abc123&response_type=code") == "abc123");
  CHECK(db::clientIdFromAuthorizeUrl(Authorize + "?response_type=code&client_id=a%2Bb") == "a+b");
  CHECK(db::clientIdFromAuthorizeUrl(Authorize + "?response_type=code").empty());
  CHECK(db::clientIdFromAuthorizeUrl("client_id=abc").empty());
  CHECK(db::clientIdFromAuthorizeUrl("").empty());
  {
    const auto s = db::parseBridgeSession("{\"session_id\":\"s1\",\"poll_token\":\"p1\",\"authorize_url\":\"" +
                                          Authorize + "?client_id=k\",\"poll_url\":\"u\",\"retry_after\":5}");
    CHECK(!s.error && s.value.sessionId == "s1" && s.value.pollToken == "p1" && s.value.retryAfter == 5);
    CHECK(db::clientIdFromAuthorizeUrl(s.value.authorizeUrl) == "k");
    CHECK(db::parseBridgeSession("{\"session_id\":\"s1\"}").error);
    CHECK(db::parseBridgeSession("not json").error);
  }
  CHECK(db::parseSessionStatus("{\"status\":\"approved\"}") == "approved");
  CHECK(db::parseSessionStatus("{}") == "failed" && db::parseSessionStatus("x") == "failed");
  {
    const std::string fields = "\"authorization_code\":\"c\",\"code_verifier\":\"v\",\"state\":\"s\","
                               "\"redirect_uri\":\"r\",\"token_endpoint\":";
    const auto good = db::parseBridgeGrant("{" + fields + "\"https://api.dropboxapi.com/oauth2/token\"}");
    CHECK(!good.error && good.value.authorizationCode == "c" && good.value.codeVerifier == "v");
    CHECK(db::parseBridgeGrant("{" + fields + "\"https://evil.example/token\"}").error);
    CHECK(db::parseBridgeGrant("{\"authorization_code\":\"c\"}").error);
  }
  {
    const auto token = db::parseToken("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":14400}", true);
    CHECK(!token.error && token.value.accessToken == "a" && token.value.refreshToken == "r");
    CHECK(token.value.expiresIn == 14400);
    CHECK(db::parseToken("{\"access_token\":\"a\"}", true).error);
    CHECK(!db::parseToken("{\"access_token\":\"a\"}", false).error);
    CHECK(db::parseToken("{\"refresh_token\":\"r\"}", false).error);
  }
  {
    const auto page = db::parseListFolder(
        "{\"entries\":[{\".tag\":\"folder\",\"name\":\"revisions\",\"path_display\":\"/titles/X/revisions\"},"
        "{\".tag\":\"file\",\"name\":\"latest.zip\",\"path_display\":\"/titles/X/latest.zip\",\"rev\":\"015\","
        "\"server_modified\":\"2026-03-11T12:34:56Z\",\"size\":1234}],\"cursor\":\"cur\",\"has_more\":true}");
    CHECK(!page.error && page.value.entries.size() == 2);
    if (page.value.entries.size() == 2) {
      CHECK(page.value.entries[0].isFolder && !page.value.entries[1].isFolder);
      CHECK(page.value.entries[1].size == 1234 && page.value.entries[1].rev == "015");
      CHECK(page.value.entries[1].modified == 1773232496);
    }
    CHECK(page.value.cursor == "cur" && page.value.hasMore);
    CHECK(db::parseListFolder("{\"cursor\":\"c\"}").error);
  }
  return failures;
}
int linkTests() {
  int failures = 0;
  CHECK(db::bridgeUrl("https://shop.example", "", "start") == "https://shop.example/api/dropbox/v1/sessions/start");
  CHECK(db::bridgeUrl("https://shop.example/", "abc", "consume") ==
        "https://shop.example/api/dropbox/v1/sessions/abc/consume");
  CHECK(db::bridgeUrl("https://shop.example", "a/b", "status") ==
        "https://shop.example/api/dropbox/v1/sessions/a%2Fb/status");
  {
    db::Credentials c;
    c.accessToken = "a\"1";
    c.refreshToken = "r1";
    c.clientId = "key";
    c.account = "사용자@example.com";
    c.expiresAt = 1773232496;
    const auto back = db::parseCredentials(db::serializeCredentials(c));
    CHECK(!back.error && back.value.accessToken == "a\"1" && back.value.refreshToken == "r1");
    CHECK(back.value.clientId == "key" && back.value.account == c.account && back.value.expiresAt == 1773232496);
    CHECK(db::parseCredentials("{}").error && db::parseCredentials("").error);
    // A refresh token alone still links, as long as the app key came with it.
    CHECK(!db::parseCredentials("{\"refresh_token\":\"r\",\"client_id\":\"k\"}").error);
    CHECK(db::parseCredentials("{\"refresh_token\":\"r\"}").error);
  }
  {
    db::Credentials c;
    c.accessToken = "a";
    CHECK(!db::needsRefresh(c, 1000)); // no expiry: used as it is
    c.expiresAt = 2000;
    CHECK(!db::needsRefresh(c, 1000) && !db::needsRefresh(c, 1939));
    CHECK(db::needsRefresh(c, 1940) && db::needsRefresh(c, 5000));
    c.accessToken.clear();
    CHECK(db::needsRefresh(c, 0));
  }
  {
    db::Credentials c;
    c.refreshToken = "old";
    c.clientId = "k";
    db::Token t;
    t.accessToken = "new";
    t.expiresIn = 14400;
    const auto next = db::withToken(c, t, 100);
    CHECK(next.accessToken == "new" && next.refreshToken == "old" && next.expiresAt == 14500);
    CHECK(c.accessToken.empty()); // the original is untouched
    t.refreshToken = "rotated";
    t.expiresIn = 0;
    const auto rotated = db::withToken(c, t, 100);
    CHECK(rotated.refreshToken == "rotated" && rotated.expiresAt == 0);
  }
  {
    db::BridgeGrant g;
    g.authorizationCode = "c 1";
    g.codeVerifier = "v";
    g.redirectUri = "https://shop.example/api/dropbox/oauth/dropbox/callback";
    const auto body = db::authorizationCodeBody(g, "key");
    CHECK(body == "grant_type=authorization_code&code=c%201&code_verifier=v&client_id=key&redirect_uri="
                  "https%3A%2F%2Fshop.example%2Fapi%2Fdropbox%2Foauth%2Fdropbox%2Fcallback");
    CHECK(db::refreshBody("r+1", "key") == "grant_type=refresh_token&refresh_token=r%2B1&client_id=key");
  }
  CHECK(db::isRevoked(400, "{\"error\":\"invalid_grant\"}") && db::isRevoked(401, ""));
  CHECK(!db::isRevoked(400, "{\"error\":\"invalid_request\"}") && !db::isRevoked(500, "invalid_grant"));
  CHECK(db::isNotFound(409, "{\"error_summary\":\"path/not_found/..\"}"));
  CHECK(!db::isNotFound(409, "{\"error_summary\":\"other\"}") && !db::isNotFound(404, "not_found"));
  CHECK(db::parseAccount("{\"email\":\"me@example.com\",\"name\":{\"display_name\":\"Me\"}}") == "me@example.com");
  CHECK(db::parseAccount("{\"name\":{\"display_name\":\"Me\"}}") == "Me");
  CHECK(db::parseAccount("x").empty());
  return failures;
}
int layoutTests() {
  int failures = 0;
  const auto entry = [](const std::string &path, std::time_t modified, bool folder = false) {
    db::Entry e;
    e.path = path;
    e.name = db::fileName(path);
    e.modified = modified;
    e.isFolder = folder;
    e.size = 10;
    return e;
  };
  // What save-keeper's upload leaves (SaveBackendAdapter::upload), and others.
  const auto byTitle = db::revisionsByTitle({
      entry("/titles/01006A800016E000", 0, true),
      entry("/titles/01006A800016E000/latest.zip", 300),
      entry("/titles/01006A800016E000/latest.meta", 300),
      entry("/titles/01006A800016E000/revisions/20260311_090000.zip", 100),
      entry("/titles/01006A800016E000/revisions/20260311_090000.meta", 100),
      entry("/titles/01006a800016e000/Revisions/20260312_090000.ZIP", 200),
      entry("/titles/0100000000001000/revisions/a.zip", 5),
      entry("/titles/0100000000001000/revisions/..zip", 5),
      entry("/titles/0100000000001000/revisions/x/y.zip", 5),
      entry("/titles/01000000000010/revisions/short.zip", 5),
      entry("/titles/010000000000100G/revisions/bad.zip", 5),
      entry("/other/0100000000001000/revisions/b.zip", 5),
  });
  CHECK(byTitle.size() == 2);
  const auto zelda = byTitle.find(0x01006A800016E000ULL);
  CHECK(zelda != byTitle.end());
  if (zelda != byTitle.end()) {
    CHECK(zelda->second.size() == 2);
    if (zelda->second.size() == 2) {
      CHECK(zelda->second[0].name == "20260312_090000" && zelda->second[0].modified == 200);
      CHECK(zelda->second[1].name == "20260311_090000" && zelda->second[1].size == 10);
    }
  }
  const auto other = byTitle.find(0x0100000000001000ULL);
  CHECK(other != byTitle.end() && other->second.size() == 1 && other->second[0].name == "a");
  CHECK(db::revisionsByTitle({}).empty());
  return failures;
}
} // namespace
int runDropboxUtilTests() {
  return urlTests() + requestTests() + pkceTests() + ssrfTests() + bridgeTests() + linkTests() + layoutTests();
}
