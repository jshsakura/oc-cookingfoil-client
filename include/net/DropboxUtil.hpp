#pragma once
// Pure Dropbox helpers, ported from oc-save-keeper (MIT,
// include/network/DropboxUtil.hpp). No networking here.
#include "core/Types.hpp"
#include <cstdint>
#include <ctime>
#include <map>
#include <string>
#include <vector>
namespace foil::net::dropbox {
// The only token endpoint a bridge may hand back.
constexpr const char *TokenEndpoint = "https://api.dropboxapi.com/oauth2/token";

bool isSuccessfulHttpStatus(long status);
std::string trimCopy(const std::string &value);
std::string percentEncode(const std::string &value);
std::string percentDecode(const std::string &value);

// PKCE. Random bytes come from the console CSRNG.
bool randomBytes(unsigned char *buffer, size_t size);
std::string generateRandomHex(size_t bytes);
// Clamped to 43..128 characters of the RFC 7636 unreserved set.
std::string generateCodeVerifier(size_t length = 64);
std::string base64UrlEncode(const std::vector<unsigned char> &bytes);
std::vector<unsigned char> sha256(const std::string &input);
std::string buildCodeChallengeS256(const std::string &verifier);

// "2026-03-11T12:34:56Z" as UTC; 0 when malformed.
std::time_t parseDropboxTime(const char *value);
std::string buildAuthorizeUrl(const std::string &baseUrl, const std::string &clientId,
                              const std::string &redirectUri, const std::string &csrfToken,
                              const std::string &codeChallenge);
// Value of key in a query string or a URL's query, percent-decoded.
std::string extractQueryParam(const std::string &input, const std::string &key);
// A pasted redirect URL, a query string or a bare code.
std::string extractAuthorizationCode(const std::string &input);
// client_id from the bridge's authorize_url; empty when it has no query.
std::string clientIdFromAuthorizeUrl(const std::string &authorizeUrl);
// Exact match with TokenEndpoint; save-keeper posted the code anywhere.
bool isAllowedTokenEndpoint(const std::string &url);

std::string escapeJsonString(const std::string &value);
std::string buildListFolderRequest(const std::string &path, bool recursive = false);
std::string buildUploadArg(const std::string &path);
std::string buildDownloadArg(const std::string &path);
std::string buildCreateFolderRequest(const std::string &path);
std::string buildDeleteRequest(const std::string &path);

std::string parentPath(const std::string &dropboxPath);
std::string fileName(const std::string &dropboxPath);
bool hasFileComponent(const std::string &dropboxPath);
bool constantTimeEqual(const std::string &a, const std::string &b);
// url starts with prefix and the prefix ends at a path, query or fragment.
bool isUrlPrefixValid(const std::string &url, const std::string &expectedPrefix);
// pollUrl when it lies under bridgeBase, otherwise the default status URL.
std::string buildSafePollUrl(const std::string &pollUrl, const std::string &bridgeBase,
                             const std::string &sessionId);

// App folder layout shared with save-keeper.
std::string titleFolder(uint64_t titleId);     // /titles/<TID>
std::string latestZipPath(uint64_t titleId);   // /titles/<TID>/latest.zip
std::string latestMetaPath(uint64_t titleId);  // /titles/<TID>/latest.meta
std::string revisionsFolder(uint64_t titleId); // /titles/<TID>/revisions
// Empty when revision is not a single safe path component.
std::string revisionZipPath(uint64_t titleId, const std::string &revision);
std::string revisionMetaPath(uint64_t titleId, const std::string &revision);
bool isSafeRevision(const std::string &revision);

// Bridge and API responses.
struct BridgeSession {
  std::string sessionId, pollToken, authorizeUrl, pollUrl;
  int retryAfter = 0; // seconds, 0 when absent
};
Parsed<BridgeSession> parseBridgeSession(const std::string &json);
// "pending", "approved" and so on; "failed" when unreadable.
std::string parseSessionStatus(const std::string &json);
struct BridgeGrant {
  std::string authorizationCode, codeVerifier, state, redirectUri, tokenEndpoint;
};
// Requires every field, and a token endpoint that isAllowedTokenEndpoint.
Parsed<BridgeGrant> parseBridgeGrant(const std::string &json);
struct Token {
  std::string accessToken, refreshToken;
  int64_t expiresIn = 0; // seconds, 0 when absent
};
// A refresh response may omit refresh_token; an authorization one may not.
Parsed<Token> parseToken(const std::string &json, bool requireRefresh);
struct Entry {
  std::string path, name, rev;
  bool isFolder = false;
  std::time_t modified = 0;
  uint64_t size = 0;
};
struct ListPage {
  std::vector<Entry> entries;
  std::string cursor;
  bool hasMore = false;
};
constexpr size_t MaxListEntries = 10000;
Parsed<ListPage> parseListFolder(const std::string &json);
// The server's copy of save-keeper's bridge: server + /api/dropbox/v1/sessions/
// start, or <id>/status and <id>/consume.
std::string bridgeUrl(const std::string &server, const std::string &sessionId, const std::string &op);

// The console's link, kept in switch/cookingfoil/dropbox.json. The app key
// (client_id) is public and comes from the bridge's authorize_url, so the
// client never carries one.
struct Credentials {
  std::string accessToken, refreshToken, clientId, account;
  int64_t expiresAt = 0; // unix seconds, 0 when the token does not expire
  bool linked() const { return !accessToken.empty() || (!refreshToken.empty() && !clientId.empty()); }
};
std::string serializeCredentials(const Credentials &credentials);
Parsed<Credentials> parseCredentials(const std::string &json);
// No access token, or within a minute of its expiry (save-keeper's margin).
bool needsRefresh(const Credentials &credentials, std::time_t now);
// A copy with the response applied; a refresh that leaves out refresh_token
// keeps the one already held.
Credentials withToken(Credentials credentials, const Token &token, std::time_t now);
std::string authorizationCodeBody(const BridgeGrant &grant, const std::string &clientId);
std::string refreshBody(const std::string &refreshToken, const std::string &clientId);
// The refresh token was revoked or expired: only linking again helps.
bool isRevoked(long status, const std::string &body);
// list_folder on a folder that is not there yet (nothing uploaded).
bool isNotFound(long status, const std::string &body);
// users/get_current_account: the email, else the display name.
std::string parseAccount(const std::string &json);

// A backup on Dropbox: /titles/<TID>/revisions/<name>.zip (and .meta).
struct Revision {
  std::string name;
  std::time_t modified = 0;
  uint64_t size = 0;
};
// From a recursive listing of /titles: each title's revisions, newest first.
// Names that are not plain ASCII path components are left out.
std::map<uint64_t, std::vector<Revision>> revisionsByTitle(const std::vector<Entry> &entries);
} // namespace foil::net::dropbox
