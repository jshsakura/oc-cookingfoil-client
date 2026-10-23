#include <sys/stat.h>
#include "core/Json.hpp"
#include "core/I18n.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <chrono>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
namespace foil {
static size_t invalidUtf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) { ++i; continue; }
        const size_t n = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!n || i + n > text.size()) return i;
        const auto second = static_cast<unsigned char>(text[i + 1]);
        if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0)
            || (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90)) return i;
        for (size_t j = 1; j < n; ++j) if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80) return i;
        i += n;
    }
    return text.size();
}
static std::string asciiJson(const std::string& text) {
    std::string out;
    const char* hex = "0123456789abcdef";
    auto escape = [&](uint32_t c) {
        out += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4) out += hex[(c >> shift) & 15];
    };
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) { out += first; continue; }
        const int n = first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
        uint32_t code = first & (n == 2 ? 31 : n == 3 ? 15 : 7);
        for (int j = 1; j < n; ++j) code = (code << 6) | (static_cast<unsigned char>(text[i++]) & 63);
        if (code <= 0xffff) escape(code);
        else { code -= 0x10000; escape(0xd800 + (code >> 10)); escape(0xdc00 + (code & 1023)); }
    }
    return out;
}
Json parseJson(const std::string& text, Error& error) {
    if (text.size() > 32 * 1024 * 1024) { error.message = tr("The JSON file is too large."); return {}; }
    const auto invalid = invalidUtf8(text);
    if (invalid != text.size()) {
        error = {tr("Save the file as UTF-8."), 1 + static_cast<int>(std::count(text.begin(), text.begin() + invalid, '\n'))};
        return {};
    }
    auto* tok = json_tokener_new();
    // Keep strict JSON and Hangul portable across host/console json-c builds.
    // The host's strict tokener rejects high bytes even after UTF-8 validation.
    // Unicode escapes carry the same string data and preserve line numbers.
    const auto ascii = asciiJson(text);
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    Json value(json_tokener_parse_ex(tok, ascii.c_str(), static_cast<int>(ascii.size())));
    const auto code = json_tokener_get_error(tok);
    if (code != json_tokener_success) {
        const auto end = std::min(ascii.size(), json_tokener_get_parse_end(tok));
        error.line = 1 + std::count(ascii.begin(), ascii.begin() + end, '\n');
        error.message = trf("Check the JSON syntax. {0}", {json_tokener_error_desc(code)});
        value.reset();
    }
    json_tokener_free(tok);
    return value;
}
json_object* field(json_object* obj, const char* key) {
    json_object* value = nullptr;
    return obj && json_object_object_get_ex(obj, key, &value) ? value : nullptr;
}
std::string stringField(json_object* obj, const char* key, const std::string& fallback) {
    auto* value = field(obj, key);
    return json_object_is_type(value, json_type_string) ? json_object_get_string(value) : fallback;
}
int64_t integerField(json_object* obj, const char* key, int64_t fallback) {
    auto* value = field(obj, key);
    return json_object_is_type(value, json_type_int) ? json_object_get_int64(value) : fallback;
}
bool systemJunk(const std::string& name) {
    return name.rfind("._", 0) == 0 || name == ".DS_Store" || name == ".Spotlight-V100" || name == ".Trashes" ||
           name == ".fseventsd" || name == "Thumbs.db" || name == "desktop.ini";
}
std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    // atomicWrite leaves "<path>.old" if power went out mid-replace.
    if (!input) input.open(path + ".old", std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0 || length > 32 * 1024 * 1024) return {};
    input.seekg(0);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
bool makeDirectories(const std::string& path) {
    struct stat st {};
    if (path.empty() || (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))) return true;
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && path[slash - 1] != ':' &&
        !makeDirectories(path.substr(0, slash)))
        return false;
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}
namespace {
// The file already holds exactly these bytes (read, which costs the card
// nothing, against a rewrite that wears it).
bool holds(const std::string& path, const std::string& contents) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || uint64_t(st.st_size) != contents.size()) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string have(contents.size(), '\0');
    input.read(have.data(), std::streamsize(have.size()));
    return input.gcount() == std::streamsize(have.size()) && have == contents;
}
// Saves of one file line up here. Every whole-file writer makes its temp
// beside the target and renames it over; on a card that refuses a rename
// over an existing file, two writers of the same file race between the
// remove and the rename (one says "File exists" right after removing) and
// the temp they share gets trampled. Striped by path: unrelated files do
// not wait on each other.
std::mutex &saveStripe(const std::string& path) {
    static std::mutex stripes[32];
    return stripes[std::hash<std::string>{}(path) & 31];
}
// rename() under the card's rule (nothing over an existing file), for a
// caller that already holds the file's stripe.
bool replaceHeld(const std::string& from, const std::string& to, std::string& error) {
    // A name on the card can outlive its own remove: the remove the library
    // does for remove_all says success without taking the name off (the
    // probe saw the file still opening after twenty rounds of it), while a
    // plain unlink takes it off for real. So a regular file goes through
    // unlink and the dance breathes between rounds; split files are folders
    // and keep remove_all.
    constexpr int rounds = 20;
    constexpr auto breath = std::chrono::milliseconds(250);
    const auto drop = [&to](std::string &why) {
        std::error_code ec;
        if (std::filesystem::is_directory(std::filesystem::symlink_status(to, ec))) {
            std::filesystem::remove_all(to, ec);
            if (ec) { why = ec.message(); return false; }
            return true;
        }
        if (::unlink(to.c_str()) != 0 && errno != ENOENT) { why = std::strerror(errno); return false; }
        return true;
    };
    for (int round = 0;; ++round) {
        if (::rename(from.c_str(), to.c_str()) == 0) return true;
        const int reason = errno;
        if (reason != EEXIST && reason != ENOTEMPTY) {
            error = std::strerror(reason);
            return false;
        }
        // The console refuses rename over an existing file.
        std::string why;
        if (!drop(why)) {
            error = "removing " + std::filesystem::path(to).filename().string() + ": " + why;
            return false;
        }
        if (round == rounds) {
            error = "putting " + std::filesystem::path(to).filename().string() + " in place: " + std::strerror(reason);
            return false;
        }
        std::this_thread::sleep_for(breath);
    }
}
// The card's own words for a name that would not go: what the filesystem
// says about it right then, so a report tells which of the cases it was.
std::string probeName(const std::string &to) {
    struct stat st {};
    std::string out = ::stat(to.c_str(), &st) == 0 ? (S_ISDIR(st.st_mode) ? "a folder" : "a file") : "gone";
    if (FILE *f = std::fopen(to.c_str(), "rb")) { std::fclose(f); out += ", opens"; }
    else out += std::string(", no open: ") + std::strerror(errno);
    if (::unlink(to.c_str()) == 0) out += ", unlink now works";
    else out += std::string(", unlink: ") + std::strerror(errno);
    return out;
}
// The bytes straight into the target: the last way in when the card will
// not give the name up for a rename. Not atomic (a cut leaves the file
// short); it runs only after the rename dance has failed outright, where
// the old bytes were not reachable anyway.
bool writeWhole(const std::string &path, const std::string &contents, std::string &error) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { error = std::strerror(errno); return false; }
    size_t offset = 0;
    while (offset < contents.size()) {
        const auto written = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (written <= 0) { error = std::strerror(errno); ::close(fd); return false; }
        offset += written;
    }
    const bool flushed = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!flushed || !closed) { error = std::strerror(errno); return false; }
    return true;
}
}
bool atomicWrite(const std::string& path, const std::string& contents, std::string& error) {
    // One writer at a time: the temp keeps its plain name beside the target
    // (a save cut short is finished from it), so the writers queue here.
    const std::lock_guard<std::mutex> held(saveStripe(path));
    // Unchanged: nothing to write. Every caller (ledgers, records, settings,
    // the database) saves whole files, often with nothing new in them.
    if (holds(path, contents)) return true;
    const auto parent = std::filesystem::path(path).parent_path().string();
    if (!parent.empty() && !makeDirectories(parent)) { error = std::strerror(errno); return false; }
    const std::string temp = path + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { error = std::strerror(errno); return false; }
    size_t offset = 0;
    while (offset < contents.size()) {
        const auto written = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (written <= 0) { error = std::strerror(errno); ::close(fd); ::unlink(temp.c_str()); return false; }
        offset += written;
    }
    const bool flushed = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!flushed || !closed) { error = std::strerror(errno); ::unlink(temp.c_str()); return false; }
    if (!replaceHeld(temp, path, error)) {
        // The card would not give the name up for a rename. The bytes go in
        // anyway, straight through, and the trouble is said for the log.
        const auto probe = probeName(path);
        std::string fallen;
        if (!writeWhole(path, contents, fallen)) {
            error += "; in place: " + fallen;
            ::unlink(temp.c_str());
            return false;
        }
        logLine("save: " + std::filesystem::path(path).filename().string() +
                " written in place; the card kept its name (" + probe + "): " + error);
        ::unlink(temp.c_str());
        return true;
    }
    return true;
}
bool replaceFile(const std::string& from, const std::string& to, std::string& error) {
    const std::lock_guard<std::mutex> held(saveStripe(to));
    return replaceHeld(from, to, error);
}
static std::string absoluteUrl(const std::string& value, const std::string& base) {
    if (value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0) return value;
    if (value.rfind("/", 0) == 0 && value.rfind("//", 0) != 0 && !base.empty()) return base + value;
    return "";
}
std::string urlField(json_object* obj, const char* key, const std::string& base) {
    return absoluteUrl(stringField(obj, key), base);
}
std::vector<std::string> urlList(json_object* obj, const char* key, const std::string& base, size_t most) {
    std::vector<std::string> out;
    auto* list = field(obj, key);
    if (!json_object_is_type(list, json_type_array)) return out;
    for (size_t i = 0; i < json_object_array_length(list) && out.size() < most; ++i) {
        auto* item = json_object_array_get_idx(list, i);
        if (!json_object_is_type(item, json_type_string)) continue;
        if (auto url = absoluteUrl(json_object_get_string(item), base); !url.empty()) out.push_back(std::move(url));
    }
    return out;
}
LanguageTexts languageTexts(json_object* obj, const char* key, bool (*keep)(const std::string&)) {
    LanguageTexts out;
    auto* texts = field(obj, key);
    if (!json_object_is_type(texts, json_type_object)) return out;
    for (int i = 0; i < LanguageCount; ++i) {
        const char* code = languageCode(Language(i));
        if (auto text = stringField(texts, code); !text.empty() && (!keep || keep(text)))
            out.push_back({code, std::move(text)});
    }
    return out;
}
json_object* languageTextsJson(const LanguageTexts& texts) {
    auto* object = json_object_new_object();
    for (const auto& t : texts)
        if (!t.text.empty()) json_object_object_add(object, t.code.c_str(), json_object_new_string(t.text.c_str()));
    return object;
}
void writeBuffer(std::FILE* file) {
    if (file) std::setvbuf(file, nullptr, _IOFBF, 1 << 20);
}
}
