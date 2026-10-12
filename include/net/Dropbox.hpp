#pragma once
// Dropbox for 세이브 > 드롭박스, after oc-save-keeper's network/Dropbox.cpp
// (MIT). Sign-in goes through the shop's copy of save-keeper's bridge
// (/api/dropbox), then the console trades the code with Dropbox itself
// (PKCE, no secret) and keeps the tokens in switch/cookingfoil/dropbox.json.
// Tokens and codes are never logged. One thread uses an instance at a time.
#include "core/SaveBackups.hpp"
#include "net/Api.hpp"
#include "net/DropboxUtil.hpp"
#include <atomic>
namespace foil::net {
struct DropboxStart {
  dropbox::BridgeSession session;
  bool missing = false; // 404: the server has no Dropbox app set up
  Error error;
};
using DropboxRevisions = std::map<uint64_t, std::vector<dropbox::Revision>>;
class Dropbox {
public:
  explicit Dropbox(std::string storePath, const std::atomic<bool> *cancel = nullptr);
  // Reads dropbox.json; false when this console is not linked.
  bool load();
  bool linked() const { return credentials_.linked(); }
  const std::string &account() const { return credentials_.account; }
  DropboxStart start(const Server &server, const State &state);
  // The bridge's word on the sign-in: "pending", "approved", "failed",
  // "expired" or "consumed". retryAfter is what the server asked for.
  std::string status(const Server &server, const State &state, const dropbox::BridgeSession &session,
                     int &retryAfter, Error &error);
  // Takes the approved code from the bridge, trades it for tokens with
  // Dropbox, reads the account's name and saves dropbox.json.
  Error finish(const Server &server, const State &state, const dropbox::BridgeSession &session);
  // Forgets the link on this console (dropbox.json is removed).
  Error unlink();
  // Every game's backups on Dropbox, from one listing of /titles.
  Parsed<DropboxRevisions> revisions();
  Error upload(const std::string &localPath, const std::string &remotePath);
  Error download(const std::string &remotePath, const std::string &localPath);
  // A small file's text (a backup's .meta).
  Parsed<std::string> fetch(const std::string &remotePath);
  // Set when Dropbox refused the saved link; the console must link again.
  bool revoked() const { return revoked_; }

private:
  Error fresh(bool force = false); // a usable access token
  Error store();
  // An API call with the access token; one refresh and retry on a 401.
  Response call(const std::string &url, const std::vector<std::string> &headers, const PostOptions &options,
                Error &error);
  std::string path_;
  const std::atomic<bool> *cancel_;
  dropbox::Credentials credentials_;
  bool revoked_ = false;
};
// save-keeper's upload: the backup zipped, then revisions/<name>.meta and
// .zip, then latest.meta and latest.zip, in that order. `fallback` is the
// .meta written for a backup that has none.
Error uploadBackup(Dropbox &dropbox, const std::string &root, uint64_t titleId, const saves::LocalBackup &backup,
                   const saves::BackupMeta &fallback);
// The .meta of each named revision (at most `limit`), by revision name:
// who uploaded it from which console. Unreadable ones are left out.
std::map<std::string, saves::BackupMeta> revisionMetas(Dropbox &dropbox, uint64_t titleId,
                                                       const std::vector<std::string> &names, size_t limit = 20);
// revisions/<revision>.zip into the console's backups (saves::importArchive).
Parsed<saves::LocalBackup> downloadBackup(Dropbox &dropbox, const std::string &root, uint64_t titleId,
                                          const std::string &revision, std::time_t now);
} // namespace foil::net
