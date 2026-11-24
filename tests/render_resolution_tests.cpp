// Focused checks of the render resolution and aspect ratio support: the choice tables, the render target the game's
// screen manager accepts, the verified patch sites (synthetic images and, when installed, the real boot images of both
// builds) and the wiring from the settings file to the loader and the video out. No window, no game.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "dcp_present.h"
#include "game_profile.h"
#include "sos_resolution.h"

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

const fs::path kRoot = SOS_SOURCE_DIR;

long Step(long width, bool neo) {
    if (width < 1920) return 32;
    return neo && width >= 3200 ? 160 : 320;
}

void TestTables() {
    Check(SOS_RESOLUTION_COUNT == 5 && SOS_ASPECT_COUNT == 6, "five resolutions and six aspect ratios");
    const char* keys[] = {"native", "720p", "1440p", "1800p", "4K"};
    for (int i = 0; i < SOS_RESOLUTION_COUNT; ++i) {
        Check(sos_resolution_index(keys[i]) == i, std::string("resolution key parses: ") + keys[i]);
    }
    Check(sos_resolution_index("480p") == 0 && sos_resolution_index("") == 0 && sos_resolution_index(nullptr) == 0,
          "480p and unknown keys are native");
    const char* aspects[] = {"16:9", "16:10", "21:9", "32:9", "4:3", "5:4"};
    for (int i = 0; i < SOS_ASPECT_COUNT; ++i) {
        Check(sos_aspect_index(aspects[i]) == i, std::string("aspect key parses: ") + aspects[i]);
    }
    Check(sos_aspect_index("2:1") == 0 && sos_aspect_index(nullptr) == 0, "unknown aspect is 16:9");
}

void TestTargets() {
    const SosRenderTarget native = sos_native_render_target();
    Check(native.width == 1920 && native.height == 1080 && !native.neo && native.display_width == 1920 &&
              native.display_height == 1080,
          "native target");
    Check(sos_render_target_is_native(&native) && sos_budget_growth(&native) == 0, "native adds no memory");

    struct Expected {
        int width, height, neo;
    };
    const Expected wide[SOS_RESOLUTION_COUNT] = {{1920, 1080, 0}, {1280, 720, 0}, {2560, 1440, 1}, {3200, 1800, 1}, {3840, 2160, 1}};
    for (int i = 0; i < SOS_RESOLUTION_COUNT; ++i) {
        const SosRenderTarget target = sos_render_target(&sos_resolutions[i], &sos_aspect_ratios[0]);
        const std::string label = sos_resolutions[i].key;
        Check(target.width == wide[i].width && target.height == wide[i].height && target.neo == wide[i].neo,
              "16:9 target of " + label);
        Check(!sos_target_needs_height_sites(&target), "16:9 needs no height patch: " + label);
        Check(target.display_width == (target.neo ? 3840 : 1920) && target.display_height == (target.neo ? 2160 : 1080),
              "video out display of " + label);
    }

    uint64_t last_growth = 0;
    for (int i = 0; i < SOS_RESOLUTION_COUNT; ++i) {
        const SosRenderTarget target = sos_render_target(&sos_resolutions[i], &sos_aspect_ratios[0]);
        const uint64_t growth = sos_budget_growth(&target);
        Check(growth % (2u << 20) == 0 && growth >= last_growth, std::string("budget grows with the target: ") + sos_resolutions[i].key);
        last_growth = growth;
    }
    const SosRenderTarget four_k = sos_render_target(&sos_resolutions[4], &sos_aspect_ratios[0]);
    Check(sos_budget_growth(&four_k) == ((uint64_t(3840) * 2160 - 1920ull * 1080) * 96 + (2u << 20) - 1) / (2u << 20) * (2u << 20),
          "4K budget growth");
    const SosRenderTarget small = sos_render_target(&sos_resolutions[1], &sos_aspect_ratios[0]);
    Check(sos_budget_growth(&small) == 0, "720p adds no memory");

    for (int r = 0; r < SOS_RESOLUTION_COUNT; ++r) {
        for (int a = 0; a < SOS_ASPECT_COUNT; ++a) {
            const SosRenderTarget t = sos_render_target(&sos_resolutions[r], &sos_aspect_ratios[a]);
            const std::string label = std::string(sos_resolutions[r].key) + " " + sos_aspect_ratios[a].key;
            const bool neo = t.width > 1920 || t.height > 1080;
            Check(t.neo == (neo ? 1 : 0), "Pro mode only above native: " + label);
            Check(t.width >= (t.neo ? 1920 : 1280) && t.width <= 3840, "width within the screen manager's range: " + label);
            Check(t.width % Step(t.width, t.neo) == 0, "width in the screen manager's step: " + label);
            Check(t.height > 0 && t.height % 2 == 0, "even height: " + label);
            const double ratio = double(t.width) / t.height, want = double(sos_aspect_ratios[a].width) / sos_aspect_ratios[a].height;
            Check(std::abs(ratio - want) / want < (t.neo && t.width == 1920 ? 0.08 : 0.04), "shape kept: " + label);
            Check(t.display_height == (t.neo ? 2160 : 1080), "display follows Pro mode: " + label);
            if (a == 0) Check(!sos_target_needs_height_sites(&t), "16:9 keeps the game's own height rule: " + label);
            if (a != 0 && t.height != (t.width * 9 >> 4)) Check(sos_target_needs_height_sites(&t), "other shapes patch the height: " + label);
        }
    }
    const SosRenderTarget ultrawide = sos_render_target(&sos_resolutions[0], &sos_aspect_ratios[2]);
    Check(ultrawide.width == 2560 && ultrawide.neo == 1 && sos_target_needs_height_sites(&ultrawide), "21:9 1080p");
    const SosRenderTarget classic = sos_render_target(&sos_resolutions[0], &sos_aspect_ratios[4]);
    Check(classic.width == 1440 && classic.height == 1080 && !classic.neo, "4:3 1080p stays in base mode");
    const SosRenderTarget super_wide = sos_render_target(&sos_resolutions[0], &sos_aspect_ratios[3]);
    Check(super_wide.width == 3840 && super_wide.height == 1080 && super_wide.neo == 1, "32:9 1080p");
}

std::vector<SosResolutionSite> Sites() {
    const std::span<const sos::GameBuild> builds = sos::SupportedBuilds();
    return {builds[0].resolution_sites.begin(), builds[0].resolution_sites.end()};
}

std::vector<unsigned char> SyntheticImage(const std::vector<SosResolutionSite>& sites, size_t size) {
    std::vector<unsigned char> image(size, 0xCC);
    for (const SosResolutionSite& site : sites) std::memcpy(image.data() + site.offset, site.original, site.length);
    return image;
}

uint32_t Le32(const unsigned char* at) {
    uint32_t value;
    std::memcpy(&value, at, 4);
    return value;
}

void TestSites() {
    const std::vector<SosResolutionSite> sites = Sites();
    Check(sites.size() == 3 + SOS_SET_RESOLUTION_SITE_COUNT, "three player sites and the game's resolution requests");
    const size_t size = 0xB000000;
    char error[256];

    {
        std::vector<unsigned char> image = SyntheticImage(sites, size);
        const SosRenderTarget target = sos_render_target(&sos_resolutions[4], &sos_aspect_ratios[0]);
        Check(sos_apply_render_target(image.data(), size, sites.data(), sites.size(), &target, error, sizeof(error)), "4K applies");
        Check(Le32(&image[sites[0].offset]) == 3840 && Le32(&image[sites[0].offset + 4]) == 3840 &&
                  Le32(&image[sites[0].offset + 8]) == 60,
              "both widths hold 3840 and the next setting is untouched");
        Check(!std::memcmp(&image[sites[1].offset], sites[1].original, sites[1].length) &&
                  !std::memcmp(&image[sites[2].offset], sites[2].original, sites[2].length),
              "16:9 leaves the height code alone");
    }
    {
        std::vector<unsigned char> image = SyntheticImage(sites, size);
        const SosRenderTarget target = sos_render_target(&sos_resolutions[0], &sos_aspect_ratios[2]);
        Check(sos_apply_render_target(image.data(), size, sites.data(), sites.size(), &target, error, sizeof(error)), "21:9 applies");
        const unsigned char* calc = &image[sites[1].offset];
        Check(calc[0] == 0x41 && calc[1] == 0xBC && Le32(calc + 2) == uint32_t(target.height) && calc[6] == 0x44 &&
                  calc[7] == 0x89 && calc[8] == 0xFE,
              "height request becomes mov r12d, height; mov esi, r15d");
        const unsigned char nops[13] = {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
        const bool tail_is_nops = sites[1].length == 22 && !std::memcmp(calc + 9, nops, 13);
        Check(tail_is_nops, "the rest of the request is padded with nops");
        const unsigned char* check = &image[sites[2].offset];
        const bool refusal_gone = std::memcmp(check + sites[2].length - 6, sites[2].original + sites[2].length - 6, 6) != 0 &&
                                  !(check[sites[2].length - 6] == 0x0F && check[sites[2].length - 5] == 0x85);
        Check(refusal_gone, "the refusal of a height that is not 9/16 is gone");
        Check(!std::memcmp(check, sites[2].original, sites[2].length - 6), "the comparison before it is untouched");
    }
    {
        std::vector<unsigned char> image = SyntheticImage(sites, size);
        image[sites[1].offset + 3] ^= 1;
        const std::vector<unsigned char> before = image;
        const SosRenderTarget target = sos_render_target(&sos_resolutions[0], &sos_aspect_ratios[2]);
        Check(!sos_apply_render_target(image.data(), size, sites.data(), sites.size(), &target, error, sizeof(error)) &&
                  Contains(error, "unexpected instruction"),
              "a changed instruction refuses the patch");
        Check(image == before, "nothing is written when a site does not match");
        const SosRenderTarget width_only = sos_render_target(&sos_resolutions[2], &sos_aspect_ratios[0]);
        Check(sos_apply_render_target(image.data(), size, sites.data(), sites.size(), &width_only, error, sizeof(error)),
              "a 16:9 target does not need the height sites");
    }
    {
        std::vector<unsigned char> image = SyntheticImage(sites, size);
        const SosRenderTarget target = sos_render_target(&sos_resolutions[4], &sos_aspect_ratios[0]);
        Check(!sos_apply_render_target(image.data(), 0x1000, sites.data(), sites.size(), &target, error, sizeof(error)) &&
                  Contains(error, "outside"),
              "a site outside the image is refused");
        Check(!sos_apply_render_target(image.data(), size, nullptr, 0, &target, error, sizeof(error)), "no sites, no patch");
        std::vector<SosResolutionSite> no_check(sites.begin(), sites.begin() + 2);
        const SosRenderTarget wide = sos_render_target(&sos_resolutions[0], &sos_aspect_ratios[2]);
        Check(!sos_apply_render_target(image.data(), size, no_check.data(), no_check.size(), &wide, error, sizeof(error)) &&
                  Contains(error, "missing"),
              "a shape needs every height site");
    }
}

void TestProfile() {
    const std::span<const sos::GameBuild> builds = sos::SupportedBuilds();
    for (const sos::GameBuild& build : builds) {
        Check(build.resolution_sites.size() == 3 + SOS_SET_RESOLUTION_SITE_COUNT && !sos::RestrictedToNativeResolution(&build),
              std::string("sites enabled for ") + build.label);
    }
    bool same_player_sites = true, own_requests = true;
    for (size_t i = 0; i < builds[0].resolution_sites.size(); ++i) {
        const SosResolutionSite& a = builds[0].resolution_sites[i];
        const SosResolutionSite& b = builds[1].resolution_sites[i];
        if (a.kind != SOS_SITE_SET_RESOLUTION) same_player_sites = same_player_sites && a.offset == b.offset;
        else own_requests = own_requests && a.offset != b.offset && b.kind == SOS_SITE_SET_RESOLUTION;
    }
    Check(same_player_sites, "1.00 and 1.01 share the player's sites: their executables hold the same bytes there");
    Check(own_requests, "each build has its own resolution requests: the game code moved in 1.01");
    const std::span<const SosResolutionSite> sites = builds[0].resolution_sites;
    Check(sites[0].offset == 0x9ab0470 && sites[1].offset == 0xaa30fd && sites[2].offset == 0x147c913, "site offsets");
}

struct BootImage {
    std::vector<unsigned char> bytes;
    std::vector<uint64_t> relocation_targets;
    std::string sha256;
};

bool ReadImage(const fs::path& boot, BootImage& out) {
    std::ifstream in(boot, std::ios::binary);
    char magic[8];
    if (!in.read(magic, 8) || std::memcmp(magic, "SOSBOOT1", 8)) return false;
    const auto q = [&] { uint64_t v = 0; in.read(reinterpret_cast<char*>(&v), 8); return v; };
    const uint64_t size = q(), entry = q(), segments = q(), relocs = q(), imports = q();
    (void)entry;
    q();
    q();
    for (int i = 0; i < 4; ++i) q();
    const uint64_t modules = q();
    for (uint64_t i = 0; i < modules * 7; ++i) q();
    const uint64_t bindings = q();
    for (uint64_t i = 0; i < bindings * 3 + segments * 3; ++i) q();
    in.seekg(std::streamoff(128 * imports), std::ios::cur);
    out.relocation_targets.resize(relocs);
    for (uint64_t& target : out.relocation_targets) {
        target = q();
        q();
        q();
        q();
    }
    out.bytes.resize(size);
    return bool(in.read(reinterpret_cast<char*>(out.bytes.data()), std::streamsize(size)));
}

size_t CountOccurrences(const std::vector<unsigned char>& image, const unsigned char* pattern, size_t length) {
    size_t count = 0;
    auto it = image.begin();
    while ((it = std::search(it, image.end(), pattern, pattern + length)) != image.end()) {
        ++count;
        ++it;
    }
    return count;
}

bool GameAccepts(const SosRenderTarget& target) {
    const long width = target.width;
    const bool pro_window = target.neo && target.display_height > 1080;
    const long minimum = target.neo ? 1920 : 1280, maximum = target.neo ? (pro_window ? 3840 : 1920) : 1920;
    if (width < minimum || width > maximum) return false;
    const long step = width < 1920 ? 32 : (target.neo && width >= 3200 ? 160 : 320);
    return width % step == 0;
}

void TestRealImageSafety(const BootImage& image, const sos::GameBuild& build, const std::string& name) {
    for (const SosResolutionSite& site : build.resolution_sites) {
        for (const uint64_t target : image.relocation_targets) {
            if (target < uint64_t(site.offset) + site.length && target + 8 > site.offset) {
                Check(false, name + ": no relocation overwrites the site at " + std::to_string(site.offset));
            }
        }
    }
    Check(true, name + ": relocations leave the sites alone");
    const std::span<const SosResolutionSite> sites = build.resolution_sites;
    for (int r = 0; r < SOS_RESOLUTION_COUNT; ++r) {
        for (int a = 0; a < SOS_ASPECT_COUNT; ++a) {
            const SosRenderTarget target = sos_render_target(&sos_resolutions[r], &sos_aspect_ratios[a]);
            const std::string label = name + " " + sos_resolutions[r].key + " " + sos_aspect_ratios[a].key;
            Check(GameAccepts(target), label + ": the game's screen manager accepts the width");
            std::vector<unsigned char> patched = image.bytes;
            char error[256];
            if (sos_render_target_is_native(&target)) {
                continue;
            }
            Check(sos_apply_render_target(patched.data(), patched.size(), sites.data(), sites.size(), &target, error,
                                          sizeof(error)),
                  label + ": applies");
            const bool height_sites = sos_target_needs_height_sites(&target);
            size_t changed_outside = 0;
            for (size_t i = 0; i < patched.size(); ++i) {
                if (patched[i] == image.bytes[i]) continue;
                bool inside = false;
                for (const SosResolutionSite& site : sites) {
                    inside = inside || (i >= site.offset && i < size_t(site.offset) + site.length &&
                                        ((site.kind != SOS_SITE_HEIGHT_CALC && site.kind != SOS_SITE_HEIGHT_CHECK) || height_sites));
                }
                changed_outside += inside ? 0 : 1;
            }
            Check(changed_outside == 0, label + ": only the needed sites change");
            for (const SosResolutionSite& site : sites) {
                if ((site.kind != SOS_SITE_HEIGHT_CALC && site.kind != SOS_SITE_HEIGHT_CHECK) || height_sites) {
                    std::memcpy(patched.data() + site.offset, site.original, site.length);
                }
            }
            Check(patched == image.bytes, label + ": the original bytes restore the image exactly");
        }
    }
    const SosRenderTarget native = sos_native_render_target();
    Check(sos_render_target_is_native(&native) && sos_budget_growth(&native) == 0 && !native.neo,
          name + ": Native selects no patch, no Pro mode and no extra memory");

    std::vector<unsigned char> corrupt = image.bytes;
    const SosResolutionSite& check = sites[2];
    corrupt[check.offset + 3] ^= 0xFF;
    const std::vector<unsigned char> before = corrupt;
    const SosRenderTarget wide = sos_render_target(&sos_resolutions[0], &sos_aspect_ratios[2]);
    char error[256];
    Check(!sos_apply_render_target(corrupt.data(), corrupt.size(), sites.data(), sites.size(), &wide, error, sizeof(error)) &&
              corrupt == before,
          name + ": a changed instruction is refused and nothing is written");
    std::vector<unsigned char> other = image.bytes;
    other[sites[0].offset] ^= 0xFF;
    const std::vector<unsigned char> other_before = other;
    const SosRenderTarget four_k = sos_render_target(&sos_resolutions[4], &sos_aspect_ratios[0]);
    Check(!sos_apply_render_target(other.data(), other.size(), sites.data(), sites.size(), &four_k, error, sizeof(error)) &&
              other == other_before,
          name + ": a different width constant is refused");
}

void TestPresentationShape() {
    const DcpPresent::Size windows[] = {{1920, 1080}, {2560, 1080}, {1280, 720}, {1440, 1080}, {3840, 1080}, {1000, 1400},
                                        {1366, 768}, {3440, 1440}};
    for (int r = 0; r < SOS_RESOLUTION_COUNT; ++r) {
        for (int a = 0; a < SOS_ASPECT_COUNT; ++a) {
            const SosRenderTarget target = sos_render_target(&sos_resolutions[r], &sos_aspect_ratios[a]);
            const float image_ratio = float(target.width) / float(target.height);
            for (const DcpPresent::Size window : windows) {
                const DcpPresent::Size frame = DcpPresent::GameFrameSize(window, image_ratio);
                const DcpPresent::Region region = DcpPresent::FitRegion(frame, window);
                const std::string label = std::string(sos_resolutions[r].key) + " " + sos_aspect_ratios[a].key + " in " +
                                          std::to_string(window.width) + "x" + std::to_string(window.height);
                Check(frame.width <= window.width && frame.height <= window.height &&
                          (frame.width + 1 >= window.width || frame.height + 1 >= window.height),
                      label + ": the frame fills the window along one axis");
                Check(std::abs(int(region.width) - int(frame.width)) <= 1 && std::abs(int(region.height) - int(frame.height)) <= 1,
                      label + ": the blit adds no second letterbox");
                Check(std::abs(float(region.width) / float(region.height) - image_ratio) / image_ratio < 0.01f,
                      label + ": the picture keeps the game's shape");
            }
        }
    }
}

void TestRealImages() {
    const fs::path images = kRoot / "build" / "image";
    std::error_code ec;
    int checked = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(images, ec)) {
        const fs::path boot = entry.path() / "boot.bin", report = entry.path() / "image.json";
        if (!fs::is_regular_file(boot, ec) || !fs::is_regular_file(report, ec)) continue;
        const std::string json = ReadFile(report);
        const sos::GameBuild* build = nullptr;
        for (const sos::GameBuild& candidate : sos::SupportedBuilds()) {
            if (Contains(json, std::string("\"eboot_sha256\": \"") + candidate.eboot_sha256 + "\"")) build = &candidate;
        }
        BootImage image;
        if (!build || !ReadImage(boot, image)) continue;
        const std::string name = std::string(build->label) + " (" + entry.path().filename().string() + ")";
        ++checked;
        bool all = true;
        for (const SosResolutionSite& site : build->resolution_sites) {
            all = all && site.offset + site.length <= image.bytes.size() &&
                  !std::memcmp(&image.bytes[site.offset], site.original, site.length);
            const size_t count = site.kind == SOS_SITE_WIDTH ? 1 : CountOccurrences(image.bytes, site.original, site.length);
            const size_t expected = site.kind == SOS_SITE_SET_RESOLUTION ? SOS_SET_RESOLUTION_SITE_COUNT : 1;
            Check(count == expected, name + ": the site's bytes occur where the table says and nowhere else");
        }
        Check(all, name + ": every site holds its original bytes");
        const SosRenderTarget target = sos_render_target(&sos_resolutions[3], &sos_aspect_ratios[2]);
        char error[256];
        Check(sos_apply_render_target(image.bytes.data(), image.bytes.size(), build->resolution_sites.data(),
                                      build->resolution_sites.size(), &target, error, sizeof(error)),
              name + ": 1800p 21:9 applies to the real image");
        Check(Le32(&image.bytes[0x9ab0470]) == uint32_t(target.width), name + ": width written");
        {
            ReadImage(boot, image);
            TestRealImageSafety(image, *build, name);
        }
    }
    if (!checked) std::printf("  [skip] no installed boot image of a supported build: the real images were not read\n");
}

void TestWiring() {
    const std::string loader = ReadFile(kRoot / "src" / "loader.c");
    for (const char* needle : {"sos_render_target_from_settings(settings_file", "sos_apply_render_target(image, size",
                               "runtime_sos_set_neo_mode(render_target.neo)",
                               "runtime_memory_set_direct_extra(sos_budget_growth(&render_target))",
                               "render_target.display_width"}) {
        Check(Contains(loader, needle), std::string("loader: ") + needle);
    }
    Check(loader.find("sos_apply_render_target") < loader.find("runtime_memory_prepare()"),
          "the patch is applied before the memory pool is sized");
    const std::string gpu = ReadFile(kRoot / "gpu" / "shim" / "dcpgpu.cpp");
    Check(Contains(gpu, "SetInternalScreenSize(u32(config->display_width), u32(config->display_height))"),
          "video core: the display size reaches the video out");
    Check(gpu.find("SetInternalScreenSize") < gpu.find("VideoOut::RegisterLib"), "video core: before the driver reads it");
    const std::string settings = ReadFile(kRoot / "gpu" / "shim" / "core" / "emulator_settings.h");
    Check(Contains(settings, "GetInternalScreenWidth() { return internal_width; }"), "video out reads the configured width");
}

void TestSettingsFile() {
    const fs::path folder = fs::temp_directory_path() / ("sos_resolution_test_" + std::to_string(std::rand()));
    fs::create_directories(folder);
    const fs::path file = folder / "sons_of_sparta.ini";
    std::ofstream(file) << "render_resolution = 1440p\naspect_ratio = 21:9\n";
    int resolution = -1, aspect = -1;
    SosRenderTarget target = sos_render_target_from_settings(file.string().c_str(), 1, &resolution, &aspect);
    Check(resolution == 2 && aspect == 2 && target.width >= 2560 && target.neo == 1, "settings choose 1440p 21:9");
    target = sos_render_target_from_settings(file.string().c_str(), 0, &resolution, &aspect);
    Check(sos_render_target_is_native(&target) && resolution == 0 && aspect == 0, "a build without sites stays native");
    std::ofstream(file) << "render_resolution = 8K\naspect_ratio = 9:16\n";
    target = sos_render_target_from_settings(file.string().c_str(), 1, &resolution, &aspect);
    Check(sos_render_target_is_native(&target), "unknown values are native");
    target = sos_render_target_from_settings((folder / "missing.ini").string().c_str(), 1, &resolution, &aspect);
    Check(sos_render_target_is_native(&target), "a missing file is native");
    std::ofstream(file) << "render_resolution = 720p\n";
    target = sos_render_target_from_settings(file.string().c_str(), 1, &resolution, &aspect);
    Check(target.width == 1280 && target.height == 720 && !target.neo, "720p 16:9 stays in base mode");
    std::error_code ec;
    fs::remove_all(folder, ec);
}

} // namespace

int main() {
    TestTables();
    TestTargets();
    TestSites();
    TestProfile();
    TestRealImages();
    TestPresentationShape();
    TestWiring();
    TestSettingsFile();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
