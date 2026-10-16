#pragma once
// The server's direct download address (X-CookingFoil-Download-Origin, with
// its X-CookingFoil-Download-Token; net/Socket.hpp). A download from the
// server's own address goes there instead, the same path and query, with
// the download token as its only key: the public address may run through a
// tunnel that is far slower than the server itself. The lists keep the
// server's address (other clients read them too); the switch is made here,
// per request.
//
// Named, it is the only way: a failure there is the download's failure
// (retried there as any other), never a reason to take the public address.
#include <string>
#include <vector>
namespace foil::net {
// The server whose files may go direct ("https://host[:port]"); "" for none.
void setShopOrigin(const std::string &origin);
// Where a download of `url` is asked for now: its direct twin when the
// server named a direct address (with its token or, until one comes, none)
// and `url` is on the server's own address; else `url` itself.
std::string routeDownload(const std::string &url);
// Whether `url` is on the direct address.
bool isDirect(const std::string &url);
// The headers for a request to `requestUrl`: on the direct address the
// download token alone (the server's own keys never go there); anywhere
// else `headers` as they are.
std::vector<std::string> headersFor(const std::string &requestUrl, const std::vector<std::string> &headers);
// What to do after a request to `requestUrl`, sent with the download token
// `sent`, ended with HTTP `status` (0 for none): Again for a 401 on the
// direct address a fresh token answers (asked once, or one another request
// already got), asked again at once without counting as a try; Other for
// anything else, which the caller handles as it always has.
enum class DirectStep { Other, Again };
DirectStep afterDirectFailure(const std::string &requestUrl, long status, const std::string &sent);
// "direct" or "server": where a download of `url` goes now (the log's summary).
std::string routeName(const std::string &url);
void forgetDirect(); // tests
} // namespace foil::net
