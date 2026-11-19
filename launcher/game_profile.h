// The supported game builds. scripts/game_profiles.py holds the same identity fields.
#pragma once

#include <span>
#include <string>
#include <string_view>

namespace sos {

inline constexpr char kGameTitle[] = "God of War: Sons of Sparta";
inline constexpr char kPortRepository[] = "github.com/saulob/god-of-war-sons-of-sparta-pc";
inline constexpr char kIssuesUrl[] = "https://github.com/saulob/god-of-war-sons-of-sparta-pc/issues";
inline constexpr char kAuthorUrl[] = "https://github.com/saulob";
inline constexpr char kSponsorUrl[] = "https://github.com/sponsors/saulob";

// A package of a build, by its param.sfo APP_VER and VERSION. An update (CATEGORY gp) is applied over the packages
// before it.
struct PackageLayer {
    const char* app_ver;
    const char* sfo_version;
    bool update;
};

struct GameBuild {
    const char* title_id;
    const char* content_id;
    const char* version;
    const char* folder;       // under game\prepared and game\image
    const char* eboot_sha256;
    const char* regional_archive;
    std::span<const char* const> extra_files;
    const char* label;
    const char* region;
    std::span<const PackageLayer> layers;
    std::span<const char* const> alternate_eboots = {};
    const char* code_build = "";
};

bool IsBuildExecutable(const GameBuild& build, std::string_view sha256);
std::span<const GameBuild> SupportedBuilds();
const PackageLayer& FinishedLayer(const GameBuild& build);
std::string BuildMetadataText(const GameBuild& build);
inline bool IsUpdateBuild(const GameBuild& build) { return build.layers.size() > 1; }
const GameBuild* BaseBuildOf(const GameBuild& build, std::span<const GameBuild> builds = SupportedBuilds());
std::string SupportedBuildsText();
std::string CodeBuildKey(const GameBuild& build);

inline constexpr const char* kSharedGameFiles[] = {
    "eboot.bin",
    "sce_discmap.plt",
    "sce_module/libc.prx",
    "sce_module/libSceFios2.prx",
    "Media/Modules/PS4Util.prx",
    "Media/Modules/Il2cppUserAssemblies.prx",
    "sce_sys/param.sfo",
};

// Linked after the eboot in this order.
inline constexpr const char* kBundledModules[] = {
    "sce_module/libc.prx",
    "sce_module/libSceFios2.prx",
    "Media/Modules/PS4Util.prx",
    "Media/Modules/Il2cppUserAssemblies.prx",
};

inline constexpr char kPreparedMarker[] = ".sos_view.json";

} // namespace sos
