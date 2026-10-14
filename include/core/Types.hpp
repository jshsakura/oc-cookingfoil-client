#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace foil {
struct Item {
    std::string url, name, titleId, baseId, type = "base", icon, publisher, region, versionName;
    uint64_t size = 0;
    uint32_t version = 0;
    int64_t addedAt = 0;
    int releaseDate = 0;
    int rank = 0, players = 0; // eShop popularity rank (1 is top), player count
    // Optional server fields (features: item-price and friends); empty or 0
    // when the server does not send them.
    std::string priceRegular, priceDiscount, priceDiscountEnds, priceCountry, requiredFirmware;
    uint64_t installSize = 0;
    uint32_t latestVersion = 0;
    int dlcTotal = 0;
    // Review score 0-100 (feature item-score); 0 when unknown or too few votes.
    int score = 0, scoreCount = 0;
    std::string scoreLabel, scoreSource;
    int patches = 0; // user patches the server holds for this game (feature user-patches)
    // Store names per language (feature item-names): ko, en, ja, zh in that
    // order, empty when the server sends none. Left empty (no allocation)
    // for the many items without names.
    std::vector<std::string> names;
    // Cover art per language (feature item-icons), same slots as names;
    // only languages whose art differs from icon are sent.
    std::vector<std::string> icons;
    std::vector<std::string> languages, categories;
};
struct Server {
    std::string title, url, username, password, cfClientId, cfClientSecret;
    bool enabled = true;
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
