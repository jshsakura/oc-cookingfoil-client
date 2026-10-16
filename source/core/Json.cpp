#include <sys/stat.h>
#include "core/Json.hpp"
#include "core/I18n.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
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
bool atomicWrite(const std::string& path, const std::string& contents, std::string& error) {
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
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        // The console's SD card refuses to rename over an existing file
        // (EEXIST). Move the old file aside, rename, then drop the old one,
        // so a power cut leaves either file readable rather than neither.
        const std::string old = path + ".old";
        ::unlink(old.c_str());
        if (::rename(path.c_str(), old.c_str()) != 0 || ::rename(temp.c_str(), path.c_str()) != 0) {
            error = std::strerror(errno); ::unlink(temp.c_str()); return false;
        }
        ::unlink(old.c_str());
    }
    return true;
}
std::string urlField(json_object* obj, const char* key, const std::string& base) {
    const auto value = stringField(obj, key);
    if (value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0) return value;
    if (value.rfind("/", 0) == 0 && value.rfind("//", 0) != 0 && !base.empty()) return base + value;
    return "";
}
void writeBuffer(std::FILE* file) {
    if (file) std::setvbuf(file, nullptr, _IOFBF, 1 << 20);
}
}
