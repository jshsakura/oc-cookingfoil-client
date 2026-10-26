#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "core/Tags.hpp"
#include "core/I18n.hpp"
#include "core/Labels.hpp"
namespace foil {
struct Item {
    std::string url, name, titleId, baseId, icon;
    // Words many games share, kept once for the catalog (core/Tags.hpp).
    Word type{"base"}, publisher, region, versionName;
    // The package's kind ("nsp") when its address does not end in one, as a
    // homebrew title's download does not; empty: the address's extension.
    Word format;
    uint64_t size = 0;
    uint32_t version = 0;
    int64_t addedAt = 0;
    int releaseDate = 0;
    int rank = 0, players = 0; // eShop popularity rank (1 is top), player count
    // Optional server fields (features: item-price and friends); empty or 0
    // when the server does not send them.
    Word priceRegular, priceDiscount, priceDiscountEnds, priceCountry, requiredFirmware;
    // The firmware that brought the master key its NCAs are encrypted with
    // (feature item-key-generation).
    Word keyFirmware;
    uint64_t installSize = 0;
    uint32_t latestVersion = 0;
    int dlcTotal = 0;
    // Review score 0-100 (feature item-score); 0 when unknown or too few votes.
    int score = 0, scoreCount = 0;
    Word scoreLabel, scoreSource;
    int patches = 0; // user patches the server holds for this game (feature user-patches)
    int requiredPatches = 0; // of those, the ones the game does not run without
    int koreanPatches = 0;   // of those, the ones that put the game in Korean
    // The server's check of the file (feature item-integrity): 0 fine or not
    // checked yet, 1 modified (signature off: a mod or a repack), 2 maybe
    // damaged, 3 damaged (item-integrity-detail's certain: the install
    // surely fails). Said only; installs go ahead either way.
    uint8_t integrity = 0;
    // The server found it surely unsigned (feature item-signed, signed.nca or
    // signed.ticket false): it opens only with signature patches. Not known
    // is false.
    bool unsignedFile = false;
    // Installed through a server plugin's option (net/VariantsApi): its
    // address carries the option's query, the server prepares the file
    // first (progress says so; failed when it refuses without words), and
    // hashesChange when it comes changed, its NCAs no longer hashing to
    // their ids.
    bool variant = false, hashesChange = false;
    std::string variantProgress, variantFailed;
    // The server's labels for its card, in order (core/Labels.hpp);
    // hasLabels false: an older server, the client words them itself.
    bool hasLabels = false;
    std::vector<labels::Ref> labels;
    // Store names per language (feature item-names), only those the server
    // sends; the many items without names allocate nothing.
    LanguageTexts names;
    // Cover art per language (feature item-icons); only languages whose art
    // differs from icon are sent.
    LanguageTexts icons;
    Tags languages, categories;
    // The configured server it came from (its url); empty: the one in use.
    std::string server;
    // Other configured servers that hold the same file (same title, kind and
    // version), with its address there: it can be downloaded from any of them.
    struct Copy {
        std::string server, url;
        bool operator==(const Copy& o) const { return server == o.server && url == o.url; }
    };
    std::vector<Copy> alsoOn;
};
inline constexpr int ServerColorCount = 12;
struct Server {
    std::string title, url, username, password, cfClientId, cfClientSecret;
    bool enabled = true;
    // The one-time token a client downloaded from the server's website carries
    // (cfe_…): spent once for this console's access key (POST /api/pair/enroll),
    // then taken out of config.json. Empty for a server set up by hand.
    std::string enrollToken;
    // Its dot's colour, one of the eight (ui/Theme ServerColors); -1 takes
    // the one its place in the list gives (the first green, the second blue).
    int color = -1;
};
struct Config {
    std::vector<Server> servers;
    std::string language = "ko", installTarget = "sd";
};
struct Error {
    std::string message;
    int line = 0;
    explicit operator bool() const { return !message.empty(); }
};
template<class T> struct Parsed { T value{}; Error error{}; bool created = false; };
}
