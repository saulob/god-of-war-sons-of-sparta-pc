#include "game_profile.h"

#include <string_view>

namespace sos {
namespace {

constexpr PackageLayer kBaseLayers[] = {{"01.00", "01.00", false}};
constexpr PackageLayer kUpdate101Layers[] = {{"01.00", "01.00", false}, {"01.01", "01.00", true}};
constexpr const char* kGameDataFiles[] = {
    "Media/globalgamemanagers.assets",
    "Media/Resources/unity default resources",
    "Media/StreamingAssets/aa/catalog.json",
};
constexpr const char* kUpdate101Files[] = {
    "sce_discmap_patch.plt",
    "Media/globalgamemanagers.assets",
    "Media/Resources/unity default resources",
    "Media/StreamingAssets/aa/catalog.json",
};

constexpr SosResolutionSite kResolutionSites[] = {
    {0x9ab0470, 12, SOS_SITE_WIDTH, {0x80, 0x07, 0x00, 0x00, 0x80, 0x07, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00}},
    {0xaa30fd, 22, SOS_SITE_HEIGHT_CALC,
     {0x43, 0x8d, 0x04, 0xff, 0x47, 0x8d, 0x64, 0xff, 0x0f, 0x44, 0x89, 0xfe, 0x85, 0xc0, 0x44, 0x0f, 0x49, 0xe0, 0x41,
      0xc1, 0xfc, 0x04}},
    {0x147c913, 20, SOS_SITE_HEIGHT_CHECK,
     {0x8d, 0x14, 0xdb, 0x8b, 0xb5, 0x44, 0xfe, 0xff, 0xff, 0xc1, 0xea, 0x04, 0x39, 0xd6, 0x0f, 0x85, 0x50, 0x01, 0x00,
      0x00}},
};

constexpr char kContentId[] = "UP9000-CUSA57767_00-SONSOFSPARTAPS40";

constexpr GameBuild kGameBuilds[] = {
    {.title_id = "CUSA57767", .content_id = kContentId, .version = "1.00", .folder = "CUSA57767-1.00",
     .eboot_sha256 = "19e1759adbb8038164bc9c4c40af876ca68fea7c3442034d2b665bbdb9be4a31",
     .regional_archive = nullptr, .extra_files = kGameDataFiles, .label = "CUSA57767 1.00 USA", .region = "USA",
     .layers = kBaseLayers, .skip_intro = true, .resolution_sites = kResolutionSites},
    {.title_id = "CUSA57767", .content_id = kContentId, .version = "1.01", .folder = "CUSA57767-1.01",
     .eboot_sha256 = "aafe8a2ab8e55e0044fe371248749026c494a07d6864fbb6a7fa89fe39999d58",
     .regional_archive = nullptr, .extra_files = kUpdate101Files, .label = "CUSA57767 1.01 USA", .region = "USA",
     .layers = kUpdate101Layers, .resolution_sites = kResolutionSites},
};

} // namespace

bool IsBuildExecutable(const GameBuild& build, std::string_view sha256) {
    if (sha256 == build.eboot_sha256) {
        return true;
    }
    for (const char* alternate : build.alternate_eboots) {
        if (sha256 == alternate) {
            return true;
        }
    }
    return false;
}

std::span<const GameBuild> SupportedBuilds() { return kGameBuilds; }

const PackageLayer& FinishedLayer(const GameBuild& build) { return build.layers.back(); }

std::string BuildMetadataText(const GameBuild& build) {
    const std::string app_ver = FinishedLayer(build).app_ver;
    return std::string("APP_VER ") + (app_ver.size() == 5 && app_ver[0] == '0' ? app_ver.substr(1) : app_ver) +
           " · VERSION " + FinishedLayer(build).sfo_version;
}

std::string BuildDisplayName(const GameBuild& build) {
    return std::string(build.title_id) + " " + build.version + " · " + build.region;
}

const GameBuild* BaseBuildOf(const GameBuild& build, std::span<const GameBuild> builds) {
    if (!IsUpdateBuild(build)) {
        return nullptr;
    }
    for (const GameBuild& candidate : builds) {
        if (!IsUpdateBuild(candidate) && std::string_view(candidate.title_id) == build.title_id) {
            return &candidate;
        }
    }
    return &builds.front();
}

std::string SupportedBuildsText() {
    std::string text;
    size_t index = 0;
    for (const GameBuild& build : kGameBuilds) {
        if (index > 0) {
            text += ++index == std::size(kGameBuilds) ? " and " : ", ";
        } else {
            ++index;
        }
        text += BuildDisplayName(build);
    }
    return text;
}

std::string CodeBuildKey(const GameBuild& build) {
    return std::string(build.title_id) + "-" + (build.code_build[0] ? build.code_build : build.version);
}

} // namespace sos
