#include "log_scrubber.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <optional>

#include "utf8.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sos {
namespace {

constexpr std::string_view kHome = "<home>";
constexpr std::string_view kInstall = "<install>";
constexpr std::string_view kFolder = "<folder>";
constexpr std::string_view kNetworkPath = "<network-path>";
constexpr std::string_view kUser = "<user>";
constexpr std::string_view kComputer = "<computer>";
constexpr std::string_view kSecret = "<secret>";
constexpr std::string_view kIpv4 = "<ipv4>";

constexpr std::string_view kSecretWords[] = {
    "secret",     "password",   "passwd",      "passphrase", "pwd",    "token",  "api_key",
    "apikey",     "api-key",    "access_key",  "private_key", "cookie", "credential",
};

constexpr std::string_view kGenericNames[] = {"admin", "administrator", "deck", "default", "game", "guest", "localhost",
                                              "owner", "player",        "public", "root",  "steam", "steamdeck", "user"};

constexpr std::string_view kVersionWords[] = {"amd",   "api",  "build",  "driver", "intel", "mesa",
                                              "nvidia", "radeon", "sdk", "ver",    "version", "vulkan"};

constexpr std::string_view kProfileRoots[] = {"/var/home/", "/run/media/", "/home/", "/Users/", "/media/"};

char Folded(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsWord(char c) { return IsDigit(c) || IsLetter(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80; }
bool IsSeparator(char c) { return c == '\\' || c == '/'; }

bool StartsFolded(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) { return Folded(a) == Folded(b); });
}

bool StandsAlone(std::string_view text, size_t at, size_t size) {
    return (at == 0 || !IsWord(text[at - 1])) && (at + size >= text.size() || !IsWord(text[at + size]));
}

bool InsidePlaceholder(std::string_view text, size_t at, size_t size) {
    return at > 0 && text[at - 1] == '<' && at + size < text.size() && text[at + size] == '>';
}

std::string ReplaceWord(std::string_view line, std::string_view word, std::string_view with) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        if (StartsFolded(line.substr(i), word) && StandsAlone(line, i, word.size()) && !InsidePlaceholder(line, i, word.size())) {
            out += with;
            i += word.size();
        } else {
            out += line[i++];
        }
    }
    return out;
}

size_t FolderLength(std::string_view text, std::string_view folder) {
    size_t t = 0, f = 0;
    while (f < folder.size()) {
        if (t >= text.size()) return 0;
        if (IsSeparator(folder[f])) {
            if (!IsSeparator(text[t])) return 0;
            while (t < text.size() && IsSeparator(text[t])) ++t;
            while (f < folder.size() && IsSeparator(folder[f])) ++f;
        } else {
            if (Folded(text[t]) != Folded(folder[f])) return 0;
            ++t;
            ++f;
        }
    }
    return t < text.size() && IsWord(text[t]) ? 0 : t;
}

struct KnownFolder {
    std::string path;
    std::string_view token;
};

std::vector<KnownFolder> KnownFolders(const PrivateFacts& facts) {
    std::vector<KnownFolder> folders;
    const auto add = [&folders](const std::vector<std::string>& paths, std::string_view token) {
        for (std::string path : paths) {
            while (!path.empty() && IsSeparator(path.back())) path.pop_back();
            if (path.size() >= 3) folders.push_back({std::move(path), token});
        }
    };
    add(facts.profile_folders, kHome);
    add(facts.install_folders, kInstall);
    std::stable_sort(folders.begin(), folders.end(),
                     [](const KnownFolder& a, const KnownFolder& b) { return a.path.size() > b.path.size(); });
    return folders;
}

std::string ReplaceKnownFolders(std::string_view line, const std::vector<KnownFolder>& folders) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        size_t matched = 0;
        std::string_view token;
        if (i == 0 || !IsWord(line[i - 1])) {
            for (const KnownFolder& folder : folders) {
                matched = FolderLength(line.substr(i), folder.path);
                if (matched) {
                    token = folder.token;
                    break;
                }
            }
        }
        if (matched) {
            out += token;
            i += matched;
        } else {
            out += line[i++];
        }
    }
    return out;
}

size_t NameEnd(std::string_view line, size_t at) {
    size_t i = at;
    while (i < line.size() && !IsSeparator(line[i]) && line[i] != '"' && line[i] != ':' && line[i] != ',' &&
           line[i] != ')' && line[i] != '<' && line[i] != '>' && line[i] != ' ' && line[i] != '\t') {
        ++i;
    }
    return i;
}

size_t SkipSeparators(std::string_view line, size_t at) {
    while (at < line.size() && IsSeparator(line[at])) ++at;
    return at;
}

std::optional<size_t> WindowsProfileEnd(std::string_view line, size_t at) {
    if (at + 3 >= line.size() || !IsLetter(line[at]) || line[at + 1] != ':' || !IsSeparator(line[at + 2])) return std::nullopt;
    if (at > 0 && IsWord(line[at - 1])) return std::nullopt;
    const size_t users = SkipSeparators(line, at + 2);
    if (!StartsFolded(line.substr(users), "users") || users + 5 >= line.size() || !IsSeparator(line[users + 5])) return std::nullopt;
    return NameEnd(line, SkipSeparators(line, users + 5));
}

std::optional<size_t> PosixProfileEnd(std::string_view line, size_t at) {
    if (line[at] != '/' || (at > 0 && (IsWord(line[at - 1]) || line[at - 1] == '.'))) return std::nullopt;
    for (const std::string_view root : kProfileRoots) {
        if (line.substr(at).starts_with(root) && at + root.size() < line.size()) return NameEnd(line, at + root.size());
    }
    return std::nullopt;
}

std::optional<size_t> WindowsDrivePathEnd(std::string_view line, size_t at) {
    if (at + 3 >= line.size() || !IsLetter(line[at]) || line[at + 1] != ':' || !IsSeparator(line[at + 2])) return std::nullopt;
    if (at > 0 && IsWord(line[at - 1])) return std::nullopt;
    size_t i = SkipSeparators(line, at + 2);
    size_t last_sep = 0;
    while (i < line.size() && line[i] != '"' && line[i] != ',' && line[i] != ')' && line[i] != ';' && line[i] != '<' &&
           line[i] != '>' && line[i] != ' ' && line[i] != '\t') {
        if (IsSeparator(line[i])) last_sep = i;
        ++i;
    }
    return last_sep > at + 2 ? std::optional<size_t>(last_sep) : std::nullopt;
}

std::optional<size_t> UncEnd(std::string_view line, size_t at) {
    if (at + 3 >= line.size() || line[at] != '\\' || line[at + 1] != '\\') return std::nullopt;
    if (at > 0 && (IsWord(line[at - 1]) || line[at - 1] == '>' || line[at - 1] == ':')) return std::nullopt;
    size_t i = at + 2;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '"' && line[i] != ',' && line[i] != ')') ++i;
    return i > at + 3 ? std::optional<size_t>(i) : std::nullopt;
}

std::string ScrubPaths(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        if (const auto end = WindowsProfileEnd(line, i); end && *end > i) {
            out += kHome;
            i = *end;
        } else if (const auto end = PosixProfileEnd(line, i); end && *end > i) {
            out += kHome;
            i = *end;
        } else if (const auto end = UncEnd(line, i); end && *end > i) {
            out += kNetworkPath;
            i = *end;
        } else if (const auto end = WindowsDrivePathEnd(line, i); end && *end > i) {
            out += kFolder;
            i = *end;
        } else {
            out += line[i++];
        }
    }
    return out;
}

bool PrecededByVersionWord(std::string_view line, size_t at) {
    size_t end = at;
    while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == ':' || line[end - 1] == '=' || line[end - 1] == 'v' ||
                       line[end - 1] == 'V')) {
        --end;
    }
    size_t start = end;
    while (start > 0 && IsWord(line[start - 1])) --start;
    const std::string_view word = line.substr(start, end - start);
    // "file_version=27.20.100.9466" is a version, not an address.
    constexpr std::string_view kVersionSuffix = "_version";
    if (word.size() > kVersionSuffix.size() && StartsFolded(word.substr(word.size() - kVersionSuffix.size()), kVersionSuffix))
        return true;
    for (const std::string_view v : kVersionWords) {
        if (word.size() == v.size() && StartsFolded(word, v)) return true;
    }
    return false;
}

std::string ScrubIpv4(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        if (IsDigit(line[i]) && (i == 0 || !IsWord(line[i - 1])) && !PrecededByVersionWord(line, i)) {
            size_t j = i;
            int octets = 0;
            bool ok = true;
            while (octets < 4) {
                int value = 0, digits = 0;
                while (j < line.size() && IsDigit(line[j]) && digits < 4) {
                    value = value * 10 + (line[j++] - '0');
                    ++digits;
                }
                if (digits == 0 || digits > 3 || value > 255) {
                    ok = false;
                    break;
                }
                ++octets;
                if (octets < 4) {
                    if (j >= line.size() || line[j] != '.') {
                        ok = false;
                        break;
                    }
                    ++j;
                }
            }
            if (ok && octets == 4 && (j == line.size() || (!IsWord(line[j]) && line[j] != '.'))) {
                out += kIpv4;
                i = j;
                continue;
            }
        }
        out += line[i++];
    }
    return out;
}

std::string ScrubSecrets(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        bool matched = false;
        if (i == 0 || !IsWord(line[i - 1])) {
            for (const std::string_view key : kSecretWords) {
                if (!StartsFolded(line.substr(i), key)) continue;
                size_t after = i + key.size();
                if (after < line.size() && IsWord(line[after])) continue;
                while (after < line.size() && (line[after] == ' ' || line[after] == '\t' || line[after] == '"')) ++after;
                if (after >= line.size() || (line[after] != '=' && line[after] != ':')) continue;
                ++after;
                while (after < line.size() && (line[after] == ' ' || line[after] == '\t' || line[after] == '"')) ++after;
                out.append(line.substr(i, after - i));
                out += kSecret;
                while (after < line.size() && line[after] != ' ' && line[after] != '\t' && line[after] != ',' &&
                       line[after] != ';' && line[after] != '"' && line[after] != ')') {
                    ++after;
                }
                i = after;
                matched = true;
                break;
            }
        }
        if (!matched) out += line[i++];
    }
    return out;
}

bool IsGenericName(std::string_view name) {
    if (name.size() < 3) return true;
    for (const std::string_view generic : kGenericNames) {
        if (name.size() == generic.size() && StartsFolded(name, generic)) return true;
    }
    return false;
}

void AddUnique(std::vector<std::string>& list, std::string value) {
    if (value.empty()) return;
    if (std::find(list.begin(), list.end(), value) == list.end()) list.push_back(std::move(value));
}

void AddEnv(std::vector<std::string>& list, const char* name) {
    if (const char* v = std::getenv(name); v && *v) AddUnique(list, v);
}

std::string SanitizeUtf8AndLines(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
            out += '\n';
            ++i;
        } else if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f)) {
            out += char(c);
            ++i;
        } else if (c >= 0x80) {
            size_t len = 0;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            bool valid = len >= 2 && i + len <= raw.size();
            for (size_t k = 1; valid && k < len; ++k) {
                if ((static_cast<unsigned char>(raw[i + k]) & 0xC0) != 0x80) valid = false;
            }
            if (valid) {
                out.append(raw.substr(i, len));
                i += len;
            } else {
                out += '?';
                ++i;
            }
        } else {
            ++i;
        }
    }
    return out;
}

} // namespace

PrivateFacts GatherPrivateFacts(const std::vector<std::filesystem::path>& install_folders) {
    PrivateFacts facts;
    for (const std::filesystem::path& folder : install_folders) {
        if (!folder.empty()) AddUnique(facts.install_folders, Utf8(folder));
    }
#ifdef _WIN32
    AddEnv(facts.profile_folders, "USERPROFILE");
    AddEnv(facts.user_names, "USERNAME");
    AddEnv(facts.machine_names, "COMPUTERNAME");
    wchar_t wbuf[256];
    DWORD len = DWORD(std::size(wbuf));
    if (GetUserNameW(wbuf, &len) && len > 1) AddUnique(facts.user_names, Utf8(std::filesystem::path(wbuf)));
    len = DWORD(std::size(wbuf));
    if (GetComputerNameW(wbuf, &len) && len > 0) AddUnique(facts.machine_names, Utf8(std::filesystem::path(wbuf)));
#else
    AddEnv(facts.profile_folders, "HOME");
    AddEnv(facts.user_names, "USER");
    AddEnv(facts.user_names, "LOGNAME");
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) == 0 && host[0]) AddUnique(facts.machine_names, host);
#endif
    return facts;
}

std::string ScrubText(std::string_view text, const PrivateFacts& facts) {
    const std::string clean = SanitizeUtf8AndLines(text);
    const std::vector<KnownFolder> folders = KnownFolders(facts);
    std::string out;
    out.reserve(clean.size());
    for (size_t at = 0; at < clean.size();) {
        const size_t end = clean.find('\n', at);
        const bool has_nl = end != std::string::npos;
        std::string line = clean.substr(at, has_nl ? end - at : std::string::npos);
        line = ReplaceKnownFolders(line, folders);
        line = ScrubPaths(line);
        line = ScrubSecrets(line);
        line = ScrubIpv4(line);
        for (const std::string& user : facts.user_names) {
            if (!IsGenericName(user)) line = ReplaceWord(line, user, kUser);
        }
        for (const std::string& machine : facts.machine_names) {
            if (!IsGenericName(machine)) line = ReplaceWord(line, machine, kComputer);
        }
        out += line;
        if (has_nl) out += '\n';
        if (!has_nl) break;
        at = end + 1;
    }
    return out;
}

} // namespace sos
