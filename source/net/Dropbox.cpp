// After oc-save-keeper (MIT), source/network/Dropbox.cpp and
// source/ui/saves/SaveBackendAdapter.cpp. Copyright (c) jshsakura.
#include "net/Dropbox.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "core/SaveArchive.hpp"
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <thread>
namespace foil::net {
namespace {
namespace db = dropbox;
constexpr const char *Api = "https://api.dropboxapi.com/2";
constexpr const char *Content = "https://content.dropboxapi.com/2";
constexpr const char *Json = "Content-Type: application/json";
// Big enough for a save on a slow line; a stall still ends it after 30 s.
constexpr long TransferTimeout = 0;
std::vector<std::string> shopHeaders(const Server &server, const State &state, const std::string &url) {
  auto headers = authHeaders(server, state, url);
  headers.push_back(Json);
  return headers;
}
std::string pollBody(const db::BridgeSession &session) {
  return "{\"poll_token\":\"" + db::escapeJsonString(session.pollToken) + "\"}";
}
// Dropbox's own words for a failed call, for the log; never a token.
std::string summary(const Response &response) {
  Error error;
  const auto root = parseJson(response.body, error);
  return error ? "" : stringField(root.get(), "error_summary");
}
Error httpError(const char *what, const Response &response) {
  if (!response.error.empty()) return {response.error};
  logLine(std::string("dropbox: ") + what + " HTTP " + std::to_string(response.status) + " " + summary(response));
  if (response.status == 507 || response.body.find("insufficient_space") != std::string::npos)
    return {tr("Dropbox is out of space.")};
  return {trf("The Dropbox request failed. HTTP {0}", {std::to_string(response.status)})};
}
} // namespace

Dropbox::Dropbox(std::string dbPath, const std::atomic<bool> *cancel)
    : dbPath_(std::move(dbPath)), cancel_(cancel) {
  legacyPath_ = (std::filesystem::path(dbPath_).parent_path() / "dropbox.json").string();
}

bool Dropbox::load() {
  credentials_ = {};
  if (const auto kept = loadDropbox(dbPath_)) {
    auto parsed = db::parseCredentials(*kept);
    if (!parsed.error) credentials_ = parsed.value;
  }
  std::error_code ec;
  if (linked() || !std::filesystem::exists(legacyPath_, ec)) return linked();
  // Earlier builds kept the tokens in plain text: moved in, encrypted, and
  // the file goes once that is saved.
  auto parsed = db::parseCredentials(readFile(legacyPath_));
  if (parsed.error) return false;
  credentials_ = parsed.value;
  if (store()) return linked(); // not saved: the file stays for the next try
  std::filesystem::remove(legacyPath_, ec);
  logLine("dropbox: dropbox.json moved into cookingfoil.db, encrypted");
  return linked();
}

Error Dropbox::store() {
  Error error;
  if (!saveDropbox(dbPath_, db::serializeCredentials(credentials_), error))
    return {trf("Could not save the Dropbox link. {0}", {error.message})};
  return {};
}

DropboxStart Dropbox::start(const Server &server, const State &state) {
  DropboxStart result;
  const auto url = db::bridgeUrl(server.url, "", "start");
  PostOptions options;
  options.body = "{\"device_id\":\"switch\"}";
  const auto response = post(url, shopHeaders(server, state, url), options, cancel_);
  if (response.status == 404 && response.error.empty()) {
    result.missing = true;
    return result;
  }
  if (!response.error.empty() || response.status != 200) {
    result.error = response.error.empty()
                       ? Error{trf("Could not start connecting Dropbox. HTTP {0}", {std::to_string(response.status)})}
                       : Error{response.error};
    return result;
  }
  auto parsed = db::parseBridgeSession(response.body);
  if (parsed.error || parsed.value.authorizeUrl.empty()) {
    logLine("dropbox: bridge start: " + parsed.error.message);
    result.error.message = tr("Could not read the server's Dropbox reply.");
    return result;
  }
  result.session = std::move(parsed.value);
  return result;
}

std::string Dropbox::status(const Server &server, const State &state, const db::BridgeSession &session,
                            int &retryAfter, Error &error) {
  // The poll URL the server gave, only when it is the server's own.
  const auto base = server.url.substr(0, server.url.find_last_not_of('/') + 1) + "/api/dropbox";
  const auto url = db::buildSafePollUrl(session.pollUrl, base, db::percentEncode(session.sessionId));
  PostOptions options;
  options.body = pollBody(session);
  const auto response = post(url, shopHeaders(server, state, url), options, cancel_);
  retryAfter = 0;
  if (!response.error.empty()) {
    error.message = response.error;
    return "pending"; // a dropped poll is asked again
  }
  if (response.status == 404) return "expired";
  if (response.status != 200) return "failed";
  Error parseError;
  const auto root = parseJson(response.body, parseError);
  retryAfter = int(std::clamp<int64_t>(integerField(root.get(), "retry_after"), 0, 60));
  return db::parseSessionStatus(response.body);
}

Error Dropbox::finish(const Server &server, const State &state, const db::BridgeSession &session) {
  const auto url = db::bridgeUrl(server.url, session.sessionId, "consume");
  PostOptions options;
  options.body = pollBody(session);
  options.timeout = 30;
  // The approval is taken once: a connection that never opened sent
  // nothing, so asking again is safe, and giving up would leave the phone's
  // sign-in done and this console never told.
  constexpr int Tries = 4;
  Response consumed;
  for (int attempt = 1; attempt <= Tries; ++attempt) {
    consumed = post(url, shopHeaders(server, state, url), options, cancel_);
    if (consumed.curl != 7 || attempt == Tries || (cancel_ && cancel_->load())) break;
    logLine("dropbox: consume could not connect; trying again (" + std::to_string(attempt) + ")");
    std::this_thread::sleep_for(std::chrono::seconds(attempt));
  }
  if (!consumed.error.empty()) return {consumed.error};
  if (consumed.status != 200) {
    logLine("dropbox: bridge consume HTTP " + std::to_string(consumed.status));
    return {tr("Could not complete the Dropbox approval. Connect again from the start.")};
  }
  const auto grant = db::parseBridgeGrant(consumed.body);
  if (grant.error) {
    logLine("dropbox: bridge consume: " + grant.error.message);
    return {tr("Could not read the server's Dropbox reply.")};
  }
  // The code must belong to the sign-in this console started.
  if (!db::constantTimeEqual(grant.value.state, db::extractQueryParam(session.authorizeUrl, "state"))) {
    logLine("dropbox: state mismatch");
    return {tr("Could not complete the Dropbox approval. Connect again from the start.")};
  }
  // The public app key is in the address the phone opened.
  const auto clientId = db::clientIdFromAuthorizeUrl(session.authorizeUrl);
  if (clientId.empty()) return {tr("Could not read the server's Dropbox reply.")};
  PostOptions exchange;
  exchange.body = db::authorizationCodeBody(grant.value, clientId);
  const auto response =
      post(grant.value.tokenEndpoint, {"Content-Type: application/x-www-form-urlencoded"}, exchange, cancel_);
  if (!response.error.empty()) return {response.error};
  if (response.status != 200) {
    logLine("dropbox: code exchange HTTP " + std::to_string(response.status));
    return {tr("Could not complete the Dropbox approval. Connect again from the start.")};
  }
  const auto token = db::parseToken(response.body, true);
  if (token.error) {
    logLine("dropbox: code exchange: " + token.error.message);
    return {tr("Could not complete the Dropbox approval. Connect again from the start.")};
  }
  db::Credentials linked;
  linked.clientId = clientId;
  credentials_ = db::withToken(linked, token.value, std::time(nullptr));
  revoked_ = false;
  // The name shown on the tab; a link without it still works.
  PostOptions none;
  none.body = "null";
  const auto account = post(std::string(Api) + "/users/get_current_account",
                            {Json, "Authorization: Bearer " + credentials_.accessToken}, none, cancel_);
  if (account.status == 200) credentials_.account = db::parseAccount(account.body);
  return store();
}

Error Dropbox::unlink() {
  credentials_ = {};
  Error error;
  std::error_code ec;
  std::filesystem::remove(legacyPath_, ec);
  if (!saveDropbox(dbPath_, "", error) || std::filesystem::exists(legacyPath_, ec))
    return {tr("Could not remove the Dropbox link.")};
  return {};
}

Error Dropbox::fresh(bool force) {
  if (!linked()) return {tr("Dropbox is not connected.")};
  if (!force && !db::needsRefresh(credentials_, std::time(nullptr))) return {};
  // A token without expiry or refresh token (older links) is used as it is.
  if (credentials_.refreshToken.empty() || credentials_.clientId.empty())
    return force ? Error{tr("The Dropbox link has ended. Connect again.")} : Error{};
  PostOptions options;
  options.body = db::refreshBody(credentials_.refreshToken, credentials_.clientId);
  const auto response = post(db::TokenEndpoint, {"Content-Type: application/x-www-form-urlencoded"}, options, cancel_);
  if (!response.error.empty()) return {response.error};
  if (response.status == 200) {
    const auto token = db::parseToken(response.body, false);
    if (!token.error) {
      credentials_ = db::withToken(credentials_, token.value, std::time(nullptr));
      if (auto error = store()) logLine("dropbox: " + error.message);
      return {};
    }
  }
  logLine("dropbox: refresh HTTP " + std::to_string(response.status));
  if (response.status != 200 && !db::isRevoked(response.status, response.body))
    return {trf("The Dropbox request failed. HTTP {0}", {std::to_string(response.status)})};
  // save-keeper's reconnect: the link is gone, and so is its file.
  revoked_ = true;
  unlink();
  return {tr("The Dropbox link has ended. Connect again.")};
}

Response Dropbox::call(const std::string &url, const std::vector<std::string> &headers, const PostOptions &options,
                       Error &error) {
  Response response;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if ((error = fresh(attempt > 0))) return response;
    auto withToken = headers;
    withToken.push_back("Authorization: Bearer " + credentials_.accessToken);
    response = post(url, withToken, options, cancel_);
    if (response.status != 401 || !response.error.empty()) break;
  }
  if (response.status == 401) {
    revoked_ = true;
    unlink();
    error.message = tr("The Dropbox link has ended. Connect again.");
  }
  return response;
}

Parsed<DropboxRevisions> Dropbox::revisions() {
  Parsed<DropboxRevisions> result;
  std::vector<db::Entry> entries;
  PostOptions options;
  options.body = db::buildListFolderRequest("/titles", true);
  auto response = call(std::string(Api) + "/files/list_folder", {Json}, options, result.error);
  for (int pages = 0; !result.error; ++pages) {
    if (db::isNotFound(response.status, response.body)) return result; // nothing uploaded yet
    if (!response.error.empty() || response.status != 200) {
      result.error = httpError("list_folder", response);
      return result;
    }
    auto page = db::parseListFolder(response.body);
    if (page.error) {
      result.error.message = tr("Could not read the Dropbox list.");
      return result;
    }
    entries.insert(entries.end(), page.value.entries.begin(), page.value.entries.end());
    if (!page.value.hasMore || page.value.cursor.empty() || entries.size() >= db::MaxListEntries || pages > 100)
      break;
    options.body = "{\"cursor\":\"" + db::escapeJsonString(page.value.cursor) + "\"}";
    response = call(std::string(Api) + "/files/list_folder/continue", {Json}, options, result.error);
  }
  if (!result.error) result.value = db::revisionsByTitle(entries);
  return result;
}

Error Dropbox::upload(const std::string &localPath, const std::string &remotePath) {
  PostOptions options;
  options.uploadFile = localPath;
  options.timeout = TransferTimeout;
  Error error;
  const auto response = call(std::string(Content) + "/files/upload",
                             {"Dropbox-API-Arg: " + db::buildUploadArg(remotePath),
                              "Content-Type: application/octet-stream"},
                             options, error);
  if (error) return error;
  if (!response.error.empty() || response.status != 200) return httpError("upload", response);
  return {};
}

Error Dropbox::download(const std::string &remotePath, const std::string &localPath) {
  PostOptions options;
  options.saveTo = localPath;
  options.timeout = TransferTimeout;
  Error error;
  // A content-download call takes no body and no Content-Type.
  const auto response = call(std::string(Content) + "/files/download",
                             {"Dropbox-API-Arg: " + db::buildDownloadArg(remotePath), "Content-Type:"}, options,
                             error);
  if (error) return error;
  if (!response.error.empty() || response.status != 200) return httpError("download", response);
  return {};
}

Parsed<std::string> Dropbox::fetch(const std::string &remotePath) {
  Parsed<std::string> result;
  PostOptions options;
  const auto response = call(std::string(Content) + "/files/download",
                             {"Dropbox-API-Arg: " + db::buildDownloadArg(remotePath), "Content-Type:"}, options,
                             result.error);
  if (result.error) return result;
  if (!response.error.empty() || response.status != 200) result.error = httpError("download", response);
  else result.value = response.body;
  return result;
}

std::map<std::string, saves::BackupMeta> revisionMetas(Dropbox &dropbox, uint64_t titleId,
                                                       const std::vector<std::string> &names, size_t limit) {
  std::map<std::string, saves::BackupMeta> out;
  for (size_t i = 0; i < names.size() && i < limit; ++i) {
    const auto path = db::revisionMetaPath(titleId, names[i]);
    if (path.empty()) continue;
    const auto text = dropbox.fetch(path);
    if (text.error) {
      if (dropbox.revoked()) break;
      continue;
    }
    auto meta = saves::parseMeta(text.value);
    if (!meta.error) out[names[i]] = std::move(meta.value);
  }
  return out;
}

Error uploadBackup(Dropbox &dropbox, const std::string &root, uint64_t titleId, const saves::LocalBackup &backup,
                   const saves::BackupMeta &fallback) {
  const auto zip = db::revisionZipPath(titleId, backup.name), meta = db::revisionMetaPath(titleId, backup.name);
  if (zip.empty()) return {tr("This backup's name cannot be uploaded.")};
  const auto archive = root + "/temp/upload_" + saves::titleHex(titleId) + "_" + backup.name + ".zip";
  if (auto error = saves::exportArchive(backup, fallback, archive)) return error;
  const auto localMeta = backup.path + ".meta";
  Error error = dropbox.upload(localMeta, meta);
  if (!error) error = dropbox.upload(archive, zip);
  if (!error) error = dropbox.upload(localMeta, db::latestMetaPath(titleId));
  if (!error) error = dropbox.upload(archive, db::latestZipPath(titleId));
  std::remove(archive.c_str());
  return error;
}

Parsed<saves::LocalBackup> downloadBackup(Dropbox &dropbox, const std::string &root, uint64_t titleId,
                                          const std::string &revision, std::time_t now) {
  Parsed<saves::LocalBackup> result;
  const auto zip = db::revisionZipPath(titleId, revision);
  if (zip.empty()) {
    result.error.message = tr("This backup's name cannot be downloaded.");
    return result;
  }
  std::error_code ec;
  makeDirectories(std::filesystem::path(root + "/temp").string());
  const auto archive = root + "/temp/download_" + saves::titleHex(titleId) + "_" + revision + ".zip";
  if ((result.error = dropbox.download(zip, archive))) return result;
  result = saves::importArchive(root, titleId, archive, revision, now);
  std::remove(archive.c_str());
  return result;
}
} // namespace foil::net
