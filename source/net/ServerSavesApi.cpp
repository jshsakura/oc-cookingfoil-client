#include "platform/FileSystem.hpp"
#include <cerrno>
#include <cstring>
#include "net/ServerSavesApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Patches.hpp"
#include "net/Api.hpp"
#include "net/Ranged.hpp"
#include <filesystem>
namespace foil::net {
Parsed<std::vector<serversaves::ServerSave>> serverSaves(const Server &server, const State &state, ServerSavesReply &reply,
                                                    const std::atomic<bool> *cancel) {
  // Names and descriptions come in the app's language (Korean or English).
  const auto url = server.url + "/api/saves?lang=" + languageCode();
  const auto response = get(url, authHeaders(server, state, url), "", cancel);
  if (response.status == 404) {
    reply.missing = true;
    reply.error.message = tr("This server offers no saves.");
  } else if (!response.error.empty() || response.status != 200) {
    reply.error.message = tr("Could not load the server saves.");
  }
  if (reply.error) return {{}, reply.error};
  return {serversaves::parse(response.body, server.url), {}};
}
bool downloadServerSave(const Server &server, const State &state, const serversaves::ServerSave &save,
                      const std::string &dir, std::atomic<uint64_t> &received, Error &error,
                      const std::atomic<bool> *cancel) {
  // An app's save may only land below its switch/<app>/ folder. A drop
  // resumes at the last whole file; a restart begins with an empty folder.
  const auto allow = [&save](const std::string &path) {
    return save.homebrewApp.empty() ? serversaves::pathAllowed(path) : serversaves::homebrewPathAllowed(save, path);
  };
  std::string prepareError;
  const auto fetch = rangedTar(save.url, downloadHeaders(server, state, save.url), 0, "",
                               [&](uint64_t at) -> std::unique_ptr<ArchiveWriter> {
                                 std::error_code ec;
                                 if (!at) {
                                   if (!platform::removePath(dir, ec)) {
                                     prepareError = ec.message();
                                     return {};
                                   }
                                   if (!makeDirectories(dir)) {
                                     prepareError = std::strerror(errno);
                                     return {};
                                   }
                                 }
                                 return std::make_unique<TarWriter>(dir, allow);
                               },
                               nullptr, received, cancel);
  if (!prepareError.empty()) {
    error.message = std::string(tr("Could not apply the save")) + " (" + prepareError + ")";
    return false;
  }
  if (!fetch.tarError.empty()) { error.message = fetch.tarError; return false; }
  if (!fetch.response.error.empty()) { error.message = fetch.response.error; return false; }
  if (!rangedOk(fetch.response)) {
    error.message = trf("Could not get the save. HTTP {0}", {std::to_string(fetch.response.status)});
    return false;
  }
  return fetch.complete;
}
} // namespace foil::net
