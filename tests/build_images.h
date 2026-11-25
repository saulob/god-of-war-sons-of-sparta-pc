// Where a test finds the real boot image of a supported build, and how it reports it: the launcher's build/image, then a research copy in
// build/research/image. A build without one is NO DATA, never a failure; the test still checks all that needs no image.
//   EVIDENCE|image|CUSA57767-1.01|NO DATA|<what was not read>
// SOS_EVIDENCE_ROOT replaces the project folder as the place of build/ and game/, for the tests of this header.
#ifndef SOS_TEST_BUILD_IMAGES_H
#define SOS_TEST_BUILD_IMAGES_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../src/game_build.h"

namespace sos_test {

constexpr const char* kRealImage = "REAL IMAGE";
constexpr const char* kResearchImage = "RESEARCH IMAGE";

struct BootImage {
    std::filesystem::path path;
    const char* source = nullptr;
    bool found() const { return source != nullptr; }
};

inline std::filesystem::path EvidenceRoot(const std::filesystem::path& source_dir) {
    const char* given = std::getenv("SOS_EVIDENCE_ROOT");
    return given && *given ? std::filesystem::path(given) : source_dir;
}

inline const char* Sha256Of(const std::string& key) {
    for (const auto& build : sos_game_builds)
        if (key == std::string(build.title_id) + "-" + build.version) return build.eboot_sha256;
    return nullptr;
}

inline bool IsImageOf(const std::filesystem::path& folder, const std::string& key) {
    const char* sha = Sha256Of(key);
    std::ifstream in(folder / "image.json", std::ios::binary);
    if (!sha || !in) return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.find(std::string("\"eboot_sha256\": \"") + sha + "\"") != std::string::npos;
}

// The image of `key` ("CUSA57767-1.01"): the launcher's folder, then its legacy name, then the research folder.
inline BootImage FindBootImage(const std::filesystem::path& source_dir, const std::string& key) {
    const std::filesystem::path root = EvidenceRoot(source_dir) / "build";
    const std::string legacy = "v" + key.substr(key.find('-') + 1);
    const struct {
        const char* source;
        std::filesystem::path base;
    } places[] = {{kRealImage, root / "image"}, {kResearchImage, root / "research" / "image"}};
    for (const auto& place : places)
        for (const std::string& folder : {key, legacy}) {
            const std::filesystem::path dir = place.base / folder;
            if (std::filesystem::is_regular_file(dir / "boot.bin") && IsImageOf(dir, key)) return {dir / "boot.bin", place.source};
        }
    return {};
}

inline void Report(const char* kind, const std::string& key, const char* state, const std::string& detail) {
    std::printf("EVIDENCE|%s|%s|%s|%s\n", kind, key.c_str(), state, detail.c_str());
}

inline BootImage ReportImage(const std::filesystem::path& source_dir, const std::string& key, const char* what) {
    const BootImage boot = FindBootImage(source_dir, key);
    if (boot.found()) {
        const std::filesystem::path root = EvidenceRoot(source_dir);
        Report("image", key, boot.source, boot.path.lexically_relative(root).generic_string());
    } else {
        Report("image", key, "NO DATA", std::string("no boot image in build/image or build/research/image; not read from the real code: ") + what);
    }
    return boot;
}

inline void ReportContract(const std::string& key, const std::string& what) { Report("contract", key, "SYNTHETIC CONTRACT", what); }

// The prepared game folder of `key`, which a test may read besides the image; its absence is NOT INSTALLED, never a failure.
inline bool PreparedGame(const std::filesystem::path& source_dir, const std::string& key) {
    const std::filesystem::path base = EvidenceRoot(source_dir) / "game" / "prepared";
    return std::filesystem::is_regular_file(base / key / "eboot.bin") ||
           std::filesystem::is_regular_file(base / ("v" + key.substr(key.find('-') + 1)) / "eboot.bin");
}

inline void ReportContent(const std::filesystem::path& source_dir, const std::string& key, const char* what) {
    if (PreparedGame(source_dir, key)) Report("content", key, "AVAILABLE", "game/prepared");
    else Report("content", key, "NOT INSTALLED", std::string("no prepared game folder; not read: ") + what);
}

}  // namespace sos_test

#endif
