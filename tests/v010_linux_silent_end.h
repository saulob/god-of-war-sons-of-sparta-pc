// Reading v0.1.0 Linux logs that stop at "semaphore id=2" (issue #5 and the reports that match it).
#pragma once

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

namespace sos_silent_end {

inline int g_checks = 0;
inline int g_failures = 0;

inline void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("  [%s] %s\n", ok ? "ok" : "fail", name.c_str());
}

inline std::string Read(const char* path) {
    std::ifstream in(std::string(SOS_STRING(SOS_SOURCE_DIR)) + "/" + path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

using Lines = std::vector<std::string>;

inline Lines Split(const std::string& text) {
    Lines lines;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        at = end + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

inline std::string Join(const Lines& lines) {
    std::string text;
    for (const std::string& line : lines) text += line + "\n";
    return text;
}

inline bool StartsWith(const std::string& line, std::string_view prefix) { return line.compare(0, prefix.size(), prefix) == 0; }

inline bool AnyLine(const Lines& lines, std::string_view prefix) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return StartsWith(line, prefix); });
}

inline Lines From(const Lines& lines, std::string_view first) {
    for (size_t i = 0; i < lines.size(); ++i) {
        if (StartsWith(lines[i], first)) return {lines.begin() + std::ptrdiff_t(i), lines.end()};
    }
    return {};
}

inline std::string Value(const Lines& log, std::string_view prefix) {
    for (const std::string& line : log) {
        if (StartsWith(line, prefix)) return line.substr(prefix.size());
    }
    return {};
}

constexpr std::string_view kLastLine = "Runtime: semaphore id=2, attr=0, initial=0, max=256";
constexpr std::string_view kTailStart = "Runtime: UserService initialized";
constexpr std::string_view kGuestEntry = "Entering original x86-64 code at guest offset 0x13550";

// The silent v0.1.0 end of issue #5: the same 24 lines after UserService, ending at semaphore id=2, with no report.
inline bool IsIssue5SilentEnd(const Lines& log, const Lines& issue5) {
    const Lines tail = From(log, kTailStart);
    return !log.empty() && log.back() == kLastLine && tail.size() == 24 && tail == From(issue5, kTailStart) &&
           !AnyLine(log, "CRASH:") && !AnyLine(log, "STOP:");
}

// Guest startup from the entry point on, as a multiset: timings and the order of concurrently created threads vary.
inline Lines GuestStartup(const Lines& log) {
    Lines lines;
    for (const std::string& line : From(log, kGuestEntry)) {
        if (!StartsWith(line, "Startup: ") && !StartsWith(line, "Checkpoint: ")) lines.push_back(line);
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

} // namespace sos_silent_end
