// True Ultrawide on the production code: the render target shapes, the game's resolution requests rewritten to them
// (synthetic images and the installed boot images of CUSA57767 1.00 and 1.01), the orthographic Hor+ view that follows,
// the presenter fit, the boot splash kept at 16:9, unknown-build and partial-patch refusal, and the loader wiring.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "build_images.h"
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
#ifndef SOS_LOADER_SOURCE
#define SOS_LOADER_SOURCE SOS_SOURCE_DIR "/src/loader.c"
#endif
constexpr int kNative = 0, k1440p = 2, k4K = 4;
constexpr int k169 = 0, k219 = 2, k329 = 3, k43 = 4;
// The orthographic sizes of the game's gameplay cameras (scripts/ultrawide_check.py reads them from the scenes).
constexpr double kCameraSizes[] = {3.36, 3.6, 4.32};

SosRenderTarget Target(int resolution, int aspect) {
    return sos_render_target(&sos_resolutions[resolution], &sos_aspect_ratios[aspect]);
}

uint32_t Le32(const unsigned char* at) {
    uint32_t value;
    std::memcpy(&value, at, 4);
    return value;
}

struct Request {
    uint32_t width, height;
};

// What the game asks Unity for at each of its Screen.SetResolution calls once the patch is in.
std::vector<Request> Requests(const std::vector<unsigned char>& image, std::span<const SosResolutionSite> sites) {
    std::vector<Request> out;
    for (const SosResolutionSite& site : sites) {
        if (site.kind == SOS_SITE_SET_RESOLUTION) out.push_back({Le32(&image[site.offset + 1]), Le32(&image[site.offset + 6])});
    }
    return out;
}

std::vector<unsigned char> SyntheticImage(std::span<const SosResolutionSite> sites) {
    std::vector<unsigned char> image(0xB000000, 0xCC);
    for (const SosResolutionSite& site : sites) std::memcpy(image.data() + site.offset, site.original, site.length);
    return image;
}

void TestAspectMath() {
    for (int r = 0; r < SOS_RESOLUTION_COUNT; ++r) {
        const SosRenderTarget original = Target(r, k169), wide = Target(r, k219), super = Target(r, k329);
        const std::string label = sos_resolutions[r].key;
        Check(std::abs(sos_render_target_widening(&original) - 1.0) < 0.01 && !sos_render_target_wider_than_16_9(&original),
              label + " 16:9: widening factor 1");
        Check(sos_render_target_widening(&wide) > 1.25 && sos_render_target_wider_than_16_9(&wide), label + " 21:9: wider than 16:9");
        Check(sos_render_target_widening(&super) > sos_render_target_widening(&wide) + 0.5, label + " 32:9: wider than 21:9");
        Check(wide.width > original.width || wide.height < original.height, label + " 21:9: the target is genuinely wider");
        const SosRenderTarget again = Target(r, k219);
        Check(again.width == wide.width && again.height == wide.height && again.neo == wide.neo, label + ": deterministic");
    }
    const SosRenderTarget native = Target(kNative, k169);
    const SosRenderTarget reference = sos_native_render_target();
    Check(native.width == reference.width && native.height == reference.height && native.neo == reference.neo &&
              native.display_width == reference.display_width,
          "1080p 16:9 is exactly the game's own target");
    const SosRenderTarget q169 = Target(k1440p, k169), q219 = Target(k1440p, k219), q329 = Target(k1440p, k329);
    Check(q169.width == 2560 && q169.height == 1440, "1440p 16:9 is 2560 x 1440");
    Check(q219.width == 3360 && q219.height == 1440 && q219.neo, "1440p 21:9 is 3360 x 1440: the vertical resolution is kept");
    Check(q329.width == 3840 && q329.height == 1080, "1440p 32:9 stops at the screen manager's 3840 width: 3840 x 1080");
    const SosRenderTarget n219 = Target(kNative, k219), n329 = Target(kNative, k329);
    Check(n219.width == 2560 && n219.height == 1098, "1080p 21:9 is 2560 x 1098: the nearest width the game accepts");
    Check(n329.width == 3840 && n329.height == 1080, "1080p 32:9 is 3840 x 1080");
    const SosRenderTarget k219t = Target(k4K, k219);
    Check(k219t.width == 3840 && k219t.height % 2 == 0 && k219t.height < 2160, "4K 21:9 keeps the shape within 3840");
    for (const int height : {1081, 1439, 1, 99999}) {
        const SosResolution odd{"odd", "odd", height * 16 / 9, height};
        const SosRenderTarget t = sos_render_target(&odd, &sos_aspect_ratios[k219]);
        Check(t.width >= 1280 && t.width <= 3840 && t.height > 0 && t.height % 2 == 0,
              "a nonstandard height " + std::to_string(height) + " gives a valid target");
    }
    const SosRenderTarget classic = Target(kNative, k43);
    Check(!sos_render_target_wider_than_16_9(&classic) && classic.width == 1440, "4:3 is not an ultrawide shape");
}

// Unity's orthographic projection: half height = orthographic size, half width = size * the screen's width / height.
// The camera code never sets an aspect (scripts/ultrawide_check.py), so the screen the game requests decides it.
void TestHorPlus() {
    const std::span<const SosResolutionSite> sites = sos::SupportedBuilds()[0].resolution_sites;
    const int pairs[][2] = {{k1440p, k219}, {kNative, k329}, {k1440p, k329}, {kNative, k219}};
    for (const auto& pair : pairs) {
        const int aspect = pair[1];
        const SosRenderTarget base = Target(pair[0], k169), target = Target(pair[0], aspect);
        std::vector<unsigned char> original = SyntheticImage(sites), patched = original;
        char error[256];
        Check(sos_apply_render_target(original.data(), original.size(), sites.data(), sites.size(), &base, error, sizeof(error)) &&
                  sos_apply_render_target(patched.data(), patched.size(), sites.data(), sites.size(), &target, error, sizeof(error)),
              (std::string(sos_resolutions[pair[0]].key) + " " + sos_aspect_ratios[aspect].key) + ": both targets apply");
        const std::vector<Request> before = Requests(original, sites), after = Requests(patched, sites);
        bool every_request = after.size() == SOS_SET_RESOLUTION_SITE_COUNT;
        for (const Request& r : after) every_request = every_request && r.width == uint32_t(target.width) && r.height == uint32_t(target.height);
        Check(every_request, (std::string(sos_resolutions[pair[0]].key) + " " + sos_aspect_ratios[aspect].key) + ": every resolution request asks for the target");
        if (after.empty() || before.empty()) continue;
        const double aspect_before = double(before[0].width) / before[0].height, aspect_after = double(after[0].width) / after[0].height;
        for (const double size : kCameraSizes) {
            const std::string label = (std::string(sos_resolutions[pair[0]].key) + " " + sos_aspect_ratios[aspect].key) + " camera size " + std::to_string(size);
            const double half_height_before = size, half_height_after = size;
            const double half_width_before = size * aspect_before, half_width_after = size * aspect_after;
            Check(std::abs(half_height_after - half_height_before) < 1e-9, label + ": vertical world extent unchanged");
            Check(half_width_after > half_width_before * 1.25, label + ": horizontal world extent grows");
            const double pixels_per_unit_x = after[0].width / (2 * half_width_after);
            const double pixels_per_unit_y = after[0].height / (2 * half_height_after);
            Check(std::abs(pixels_per_unit_x / pixels_per_unit_y - 1.0) < 1e-6, label + ": square world units, no stretch");
            if (before[0].height == after[0].height) {
                const double scale_before = before[0].height / (2 * half_height_before);
                Check(std::abs(pixels_per_unit_y - scale_before) < 1e-6, label + ": the same pixels per world unit as 16:9");
            }
        }
        Check(std::abs(aspect_after / aspect_before - sos_render_target_widening(&target)) < 0.01,
              (std::string(sos_resolutions[pair[0]].key) + " " + sos_aspect_ratios[aspect].key) + ": the world widens by the target's widening factor");
    }
}

// Before these sites the game set 1920 x 1080 once its settings loaded, whatever the render resolution.
void TestRenderResolutionKept() {
    const std::span<const SosResolutionSite> sites = sos::SupportedBuilds()[0].resolution_sites;
    for (int r = 1; r < SOS_RESOLUTION_COUNT; ++r) {
        const SosRenderTarget target = Target(r, k169);
        std::vector<unsigned char> image = SyntheticImage(sites);
        char error[256];
        const bool applied = sos_apply_render_target(image.data(), image.size(), sites.data(), sites.size(), &target, error, sizeof(error));
        bool kept = applied;
        for (const Request& request : Requests(image, sites)) kept = kept && request.width == uint32_t(target.width) && request.height == uint32_t(target.height);
        Check(kept, std::string(sos_resolutions[r].key) + " 16:9: the game keeps the render resolution after its settings load");
    }
}

DcpPresent::Region Present(const SosRenderTarget& target, DcpPresent::Size window) {
    const DcpPresent::Size frame = DcpPresent::GameFrameSize(window, float(target.width) / float(target.height));
    return DcpPresent::FitRegion(frame, window);
}

void TestPresenter() {
    struct Case {
        int resolution, aspect;
        DcpPresent::Size window;
        const char* name;
        bool bars_left_right, bars_top_bottom;
    };
    const Case cases[] = {
        {k1440p, k169, {2560, 1440}, "16:9 into a 16:9 window", false, false},
        {k1440p, k219, {3360, 1440}, "21:9 into a 21:9 window", false, false},
        {kNative, k219, {2520, 1080}, "21:9 1080p into the launcher's 21:9 window", false, false},
        {kNative, k329, {3840, 1080}, "32:9 into a 32:9 window", false, false},
        {k1440p, k219, {1920, 1080}, "21:9 into a 16:9 window", false, true},
        {k1440p, k169, {3360, 1440}, "a 16:9 image into a 21:9 window", true, false},
        {kNative, k329, {3440, 1440}, "32:9 into a 21:9 monitor", false, true},
    };
    for (const Case& c : cases) {
        const SosRenderTarget target = Target(c.resolution, c.aspect);
        const DcpPresent::Region region = Present(target, c.window);
        const double image = double(target.width) / target.height, shown = double(region.width) / region.height;
        Check(std::abs(shown - image) / image < 0.005, std::string(c.name) + ": no stretch");
        const uint32_t side = c.window.width - region.width, band = c.window.height - region.height;
        Check((side > c.window.width / 100) == c.bars_left_right, std::string(c.name) + ": side bars only when the shapes differ");
        Check((band > c.window.height / 100) == c.bars_top_bottom, std::string(c.name) + ": top and bottom bars only when the shapes differ");
        Check(std::abs(region.x * 2 + int(region.width) - int(c.window.width)) <= 1 &&
                  std::abs(region.y * 2 + int(region.height) - int(c.window.height)) <= 1,
              std::string(c.name) + ": centred");
        Check(DcpPresent::DestinationRegion(DcpPresent::GameFrameSize(c.window, float(image)), c.window, false) == region,
              std::string(c.name) + ": the presenter's destination is the fit, not a stretch");
    }
}

void TestSplash() {
    for (int r = 0; r < SOS_RESOLUTION_COUNT; ++r) {
        for (int a = 0; a < SOS_ASPECT_COUNT; ++a) {
            const SosRenderTarget target = Target(r, a);
            const bool wide = sos_aspect_ratios[a].width * 9 > sos_aspect_ratios[a].height * 16;
            Check(sos_render_target_wider_than_16_9(&target) == (wide ? 1 : 0),
                  std::string(sos_resolutions[r].key) + " " + sos_aspect_ratios[a].key + ": splash kept at 16:9 only when wider");
        }
    }
    float centred[8], original[8];
    std::memcpy(centred, sos_splash_patch.centred, sizeof(centred));
    std::memcpy(original, sos_splash_patch.original, sizeof(original));
    Check(original[0] == 0 && original[1] == 0 && original[2] == 1 && original[3] == 1 && original[6] == 0 && original[7] == 0,
          "the game stretches the splash video over the whole canvas");
    Check(centred[0] == 0.5f && centred[1] == 0.5f && centred[2] == 0.5f && centred[3] == 0.5f && centred[4] == 0 &&
              centred[5] == 0,
          "the patched splash video is anchored at the centre");
    // SplashScreenCanvas scales 1920 x 1080 by the screen height.
    for (const int aspect : {k219, k329}) {
        const SosRenderTarget t = Target(k1440p, aspect);
        const double scale = t.height / 1080.0, width = centred[6] * scale, height = centred[7] * scale;
        const std::string label = std::string("splash at ") + sos_aspect_ratios[aspect].key;
        Check(std::abs(width / height - 16.0 / 9.0) < 1e-6 && std::abs(height - t.height) < 1e-6, label + ": 16:9 at the full height");
        const double left = t.width * centred[0] + centred[4] * scale - width * 0.5, right = t.width - (left + width);
        Check(left > 0 && std::abs(left - right) < 1e-6, label + ": centred with equal black sides");
    }
    unsigned char bytes[32];
    std::memcpy(bytes, sos_splash_patch.original, 32);
    Check(sos_splash_patch_matches(bytes, sos_splash_patch.file_length), "the shipped scene matches");
    Check(!sos_splash_patch_matches(bytes, sos_splash_patch.file_length + 4), "another file length is refused");
    bytes[13] ^= 1;
    Check(!sos_splash_patch_matches(bytes, sos_splash_patch.file_length), "other scene bytes are refused");
    Check(!sos_splash_patch_matches(nullptr, sos_splash_patch.file_length), "nothing read is refused");
    Check(std::string(sos_splash_patch.guest_path) == "/app0/Media/level1", "the boot splash scene");
}

void TestRefusal() {
    for (const sos::GameBuild& build : sos::SupportedBuilds()) {
        const std::string name = build.label;
        const std::vector<SosResolutionSite> sites(build.resolution_sites.begin(), build.resolution_sites.end());
        const SosRenderTarget wide = Target(k1440p, k219);
        char error[256];
        std::vector<SosResolutionSite> fewer;
        bool dropped = false;
        for (const SosResolutionSite& site : sites) {
            if (!dropped && site.kind == SOS_SITE_SET_RESOLUTION) {
                dropped = true;
                continue;
            }
            fewer.push_back(site);
        }
        std::vector<unsigned char> image = SyntheticImage(sites);
        const std::vector<unsigned char> untouched = image;
        Check(!sos_apply_render_target(image.data(), image.size(), fewer.data(), fewer.size(), &wide, error, sizeof(error)) &&
                  image == untouched,
              name + ": a table missing one resolution request is refused and nothing is written");
        const SosRenderTarget width_only = Target(k1440p, k169);
        Check(!sos_apply_render_target(image.data(), image.size(), fewer.data(), fewer.size(), &width_only, error, sizeof(error)) &&
                  image == untouched,
              name + ": 1440p 16:9 also needs every request, or the game would return to 1080p");
        for (const SosResolutionSite& site : sites) {
            if (site.kind != SOS_SITE_SET_RESOLUTION) continue;
            image[site.offset + 2] ^= 1;
            Check(!sos_apply_render_target(image.data(), image.size(), sites.data(), sites.size(), &wide, error, sizeof(error)),
                  name + ": a request that is not 1920 x 1080 is refused");
            image[site.offset + 2] ^= 1;
            image[site.offset + 16] ^= 1;
            Check(!sos_apply_render_target(image.data(), image.size(), sites.data(), sites.size(), &wide, error, sizeof(error)),
                  name + ": a request without its call is refused");
            image[site.offset + 16] ^= 1;
            break;
        }
        Check(image == untouched, name + ": refused patches leave the image as it was");
    }
    const fs::path folder = fs::temp_directory_path() / ("sos_ultrawide_test_" + std::to_string(g_checks));
    fs::create_directories(folder);
    const fs::path file = folder / "sons_of_sparta.ini";
    std::ofstream(file) << "render_resolution = 1440p\naspect_ratio = 21:9\n";
    int resolution = -1, aspect = -1;
    SosRenderTarget target = sos_render_target_from_settings(file.string().c_str(), 0, &resolution, &aspect);
    Check(sos_render_target_is_native(&target) && aspect == 0, "an unknown build stays at the original 16:9");
    target = sos_render_target_from_settings(file.string().c_str(), 1, &resolution, &aspect);
    Check(target.width == 3360 && target.height == 1440 && aspect == 2, "a supported build gets 3360 x 1440");
    std::error_code ec;
    fs::remove_all(folder, ec);
}

bool ReadImage(const fs::path& boot, std::vector<unsigned char>& bytes) {
    std::ifstream in(boot, std::ios::binary);
    char magic[8];
    if (!in.read(magic, 8) || std::memcmp(magic, "SOSBOOT1", 8)) return false;
    const auto q = [&] { uint64_t v = 0; in.read(reinterpret_cast<char*>(&v), 8); return v; };
    const uint64_t size = q();
    q();
    const uint64_t segments = q(), relocs = q(), imports = q();
    for (int i = 0; i < 6; ++i) q();
    const uint64_t modules = q();
    for (uint64_t i = 0; i < modules * 7; ++i) q();
    const uint64_t bindings = q();
    for (uint64_t i = 0; i < bindings * 3 + segments * 3; ++i) q();
    in.seekg(std::streamoff(128 * imports + 32 * relocs), std::ios::cur);
    bytes.resize(size);
    return bool(in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size)));
}

void TestRealImages() {
    for (const sos::GameBuild& build : sos::SupportedBuilds()) {
        std::vector<unsigned char> image;
        const sos_test::BootImage boot = sos_test::ReportImage(kRoot, std::string("CUSA57767-") + build.version, "the SetResolution sites");
        if (!boot.found() || !ReadImage(boot.path, image)) continue;
        const std::string name = build.label;
        bool originals = true;
        for (const SosResolutionSite& site : build.resolution_sites) {
            originals = originals && site.offset + site.length <= image.size() && !std::memcmp(&image[site.offset], site.original, site.length);
        }
        Check(originals, name + ": every site holds its original bytes");
        const std::vector<unsigned char> before = image;
        const SosRenderTarget target = Target(k1440p, k219);
        char error[256];
        Check(sos_apply_render_target(image.data(), image.size(), build.resolution_sites.data(), build.resolution_sites.size(),
                                      &target, error, sizeof(error)),
              name + ": 1440p 21:9 applies to the real image");
        const std::vector<Request> requests = Requests(image, build.resolution_sites);
        bool all = requests.size() == SOS_SET_RESOLUTION_SITE_COUNT;
        for (const Request& r : requests) all = all && r.width == 3360 && r.height == 1440;
        Check(all, name + ": all seven Screen.SetResolution calls ask for 3360 x 1440");
        bool calls_kept = true;
        for (const SosResolutionSite& site : build.resolution_sites) {
            if (site.kind == SOS_SITE_SET_RESOLUTION)
                calls_kept = calls_kept && !std::memcmp(&image[site.offset + 10], &before[site.offset + 10], 7) &&
                             image[site.offset] == 0xBF && image[site.offset + 5] == 0xBE;
        }
        Check(calls_kept, name + ": the opcodes, the window mode and the call are untouched");
        std::printf("  CUSA57767 %s: %zu resolution requests rewritten on the real image\n", build.version, requests.size());
    }
}

void TestWiring() {
    const std::string loader = ReadFile(SOS_LOADER_SOURCE);
    Check(Contains(loader, "site->kind == SOS_SITE_SET_RESOLUTION") && Contains(loader, "runtime_redzone_add_volatile(site->offset + 1, 4)") &&
              Contains(loader, "runtime_redzone_add_volatile(site->offset + 6, 4)"),
          "loader: the rewritten request values are volatile for the red-zone cache");
    const size_t gate = loader.find("if (sos_render_target_wider_than_16_9(&render_target))");
    Check(gate != std::string::npos && loader.find("keep_splash_video_shape()", gate) != std::string::npos,
          "loader: the splash is kept at 16:9 only for wider targets");
    const size_t match = loader.find("sos_splash_patch_matches(bytes, length)");
    Check(match != std::string::npos && match < loader.find("runtime_file_overlay(sos_splash_patch.guest_path"),
          "loader: the scene bytes are checked before the overlay is registered");
    Check(loader.find("sos_apply_render_target") < loader.find("keep_splash_video_shape()", gate),
          "loader: the splash follows the render target the patch accepted");
    Check(Contains(loader, "True Ultrawide: Hor+ world view"), "loader: one startup line describes the view");
}

void TestNoExperimentalRoomMask() {
    const std::string loader = ReadFile(SOS_LOADER_SOURCE);
    const std::string probe = ReadFile(kRoot / "src" / "sos_uw_probe.c");
    const fs::path src = kRoot / "src";
    Check(!Contains(probe, "CameraSubsystem") && !Contains(probe, "CameraSwapInfo") &&
              !Contains(probe, "CameraBoundVolume") && !Contains(probe, "RoomVolume") &&
              !Contains(probe, "active_room_span") && !Contains(probe, "cachedRoomBounds") &&
              !Contains(probe, "IsWorldTransitioning"),
          "room mask: no raw camera or room traversal in the camera hook");
    Check(!fs::exists(src / "sos_ultrawide.c") && !fs::exists(src / "sos_ultrawide.h") &&
              !fs::exists(src / "sos_ultrawide_sites.h"),
          "room mask: no runtime room-bound module");
    Check(!Contains(loader, "SosUltrawideConfig") && !Contains(loader, "room_rule") &&
              !Contains(loader, "dcpgpu_ultrawide_world_mask") && !Contains(loader, "mask UI") &&
              !Contains(loader, "sos_centred_render_target"),
          "room mask: the loader keeps no room-bound or fallback wiring");
    Check(!fs::exists(kRoot / "gpu" / "shim" / "dcp_active_room_mask.h") &&
              !fs::exists(kRoot / "gpu" / "shim" / "dcp_world_mask.h"),
          "video core: experimental room mask headers are absent");
    const std::string cmake = ReadFile(kRoot / "CMakeLists.txt");
    Check(!Contains(cmake, "sos_ultrawide"), "build: no room module is compiled");
    const fs::path video = kRoot / "gpu" / "shadps4" / "video_core" / "renderer_vulkan";
    const std::string vk_rasterizer = ReadFile(video / "vk_rasterizer.cpp");
    const std::string gl_rasterizer = ReadFile(kRoot / "gpu" / "shadps4" / "video_core" / "renderer_opengl" / "gl_rasterizer.cpp");
    const std::string vulkan = ReadFile(video / "vk_presenter.cpp");
    const std::string gl = ReadFile(kRoot / "gpu" / "shadps4" / "video_core" / "renderer_opengl" / "gl_presenter.cpp");
    for (const auto& [name, text] : std::vector<std::pair<std::string, std::string>>{
             {"vk_rasterizer.cpp", vk_rasterizer},
             {"gl_rasterizer.cpp", gl_rasterizer},
             {"vk_presenter.cpp", vulkan},
             {"gl_presenter.cpp", gl},
         }) {
        Check(!Contains(text, "dcp_active_room_mask") && !Contains(text, "DcpActiveRoomMask") &&
                  !Contains(text, "DCP_ACTIVE_ROOM_MASK") && !Contains(text, "dcpgpu_debug_camera_snapshot"),
              "room mask: " + name + " has no experimental room mask code");
    }
    const size_t vk_mask = vulkan.find("PresentUltrawideSideMask(cmdbuf");
    const size_t vk_overlay = vulkan.find("DcpOverlay::Render(cmdbuf", vk_mask);
    Check(vk_mask != std::string::npos && vk_overlay != std::string::npos && vk_mask < vk_overlay,
          "fade mask: Vulkan fade mask draws before the host overlay");
    const std::string resolution = ReadFile(kRoot / "launcher" / "resolution.cpp") +
                                   ReadFile(kRoot / "launcher" / "sos_resolution.h");
    Check(!Contains(resolution, "sos_centred_render_target") && !Contains(resolution, "room_fallback"),
          "launcher: no centred fallback render target");
}

} // namespace

int main() {
    TestAspectMath();
    TestHorPlus();
    TestRenderResolutionKept();
    TestPresenter();
    TestSplash();
    TestRefusal();
    TestRealImages();
    TestWiring();
    TestNoExperimentalRoomMask();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
