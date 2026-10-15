#pragma once
// The server's direct download address (X-CookingFoil-Download-Origin, with
// its X-CookingFoil-Download-Token; net/Socket.hpp). A download from the
// server's own address goes there instead, the same path and query, with
// the download token as its only key: the public address may run through a
// tunnel that is far slower than the server itself. The lists keep the
// server's address (other clients read them too); the switch is made here,
// per request.
//
// The direct address failing (no connection, TLS, a timeout, 5xx, 403, or
// 401 still with a fresh token) sends the request again at once to the
// server's own address, from the same byte, and the direct one rests for
// DirectRestMinutes in this run.
#include <string>
#include <vector>
namespace foil::net {
constexpr int DirectRestMinutes = 10;
// The server whose files may go direct ("https://host[:port]"); "" for none.
void setShopOrigin(const std::string &origin);
// Where a download of `url` is asked for now: its direct twin when the
// server named a direct address and gave a token, `url` is on the server's
// own address, and the direct one is not resting; else `url` itself.
std::string routeDownload(const std::string &url);
// Whether `url` is on the direct address.
bool isDirect(const std::string &url);
// The headers for a request to `requestUrl`: on the direct address the
// download token alone (the server's own keys never go there); anywhere
// else `headers` as they are.
std::vector<std::string> headersFor(const std::string &requestUrl, const std::vector<std::string> &headers);
// What to do after a request to `requestUrl`, sent with the download token
// `sent`, ended with HTTP `status` (0 for none) and curl code `curlCode`:
// Other for a request not on the direct address or a failure the caller
// handles as before (a drop mid-transfer, a 404); Again for a 401 a fresh
// token answers (asked once, or one another request already got); Leave
// when the direct address is left: the request goes again at once to
// routeDownload(), the server's own address now. Neither Again nor Leave
// counts as a try.
enum class DirectStep { Other, Again, Leave };
DirectStep afterDirectFailure(const std::string &requestUrl, long status, long curlCode, const std::string &sent);
// How many times the direct address was left this run: a download that saw
// it change came partly from the server's own address.
unsigned directLeaves();
// "direct" or "server": where a download of `url` goes now (the log's summary).
std::string routeName(const std::string &url);
void forgetDirect(); // tests
} // namespace foil::net
