// The patch combinations a launch can produce on the real boot images of CUSA57767 1.00 and 1.01: Controller prompts
// Automatic, PlayStation or Xbox, Skip Intro off or on, applied in the loader's order, at 60 and 90 FPS. Each combination must
// verify, change only its own bytes, hold the style it was given, and not depend on the order or on the frame rate. No game,
// launcher or window is started.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "build_images.h"
#include "controller_prompts.h"
#include "dcp_frame_pacing.h"
#include "game_profile.h"
#include "sos_skip_intro.h"

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

bool ReadImage(const fs::path& boot, std::vector<uint8_t>& image, uint64_t& size) {
    std::ifstream in(boot, std::ios::binary);
    char magic[8] = {};
    if (!in || !in.read(magic, 8) || std::memcmp(magic, "SOSBOOT1", 8) != 0) return false;
    uint64_t header[5] = {};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    size = header[0];
    const uint64_t segments = header[2], relocs = header[3], imports = header[4];
    in.seekg(48, std::ios::cur);
    uint64_t modules = 0;
    in.read(reinterpret_cast<char*>(&modules), 8);
    in.seekg(static_cast<std::streamoff>(modules * 56), std::ios::cur);
    uint64_t bindings = 0;
    in.read(reinterpret_cast<char*>(&bindings), 8);
    in.seekg(static_cast<std::streamoff>(bindings * 24 + segments * 24 + imports * 128 + relocs * 32), std::ios::cur);
    image.assign(size + SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES, 0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(size)));
}

std::set<uint64_t> Changed(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint64_t size) {
    std::set<uint64_t> changed;
    for (uint64_t i = 0; i < size; ++i)
        if (a[i] != b[i]) changed.insert(i);
    return changed;
}

struct Combination {
    int style;
    bool skip;
};

// The prompt style the static sites hold: only Xbox changes them; Automatic hooks a method instead.
int StaticStyle(int style) { return style == SOS_PROMPTS_XBOX ? SOS_PROMPTS_XBOX : SOS_PROMPTS_PLAYSTATION; }

bool ApplyPrompts(std::vector<uint8_t>& image, uint64_t size, const char* key, int style) {
    char message[240];
    if (style == SOS_PROMPTS_AUTOMATIC) {
        return sos_prompt_hook_install(image.data(), size, key, image.data() + size + SOS_SKIP_INTRO_STUB_BYTES, SOS_PROMPT_HOOK_STUB_BYTES, message,
                                       sizeof(message)) == 1;
    }
    return sos_controller_prompts_apply(image.data(), size, key, style, message, sizeof(message)) == style;
}

// The loader's order: the static prompt patch, Skip Intro, then the Automatic hook.
bool Apply(std::vector<uint8_t>& image, uint64_t size, const char* key, Combination c, bool skip_first = false) {
    char message[240];
    const auto prompts = [&] { return ApplyPrompts(image, size, key, c.style); };
    const auto skip = [&] {
        return !c.skip || sos_skip_intro_apply(image.data(), size, key, image.data() + size, SOS_SKIP_INTRO_STUB_BYTES, message, sizeof(message)) == 1;
    };
    return skip_first ? skip() && prompts() : prompts() && skip();
}

void TestBuild(const char* key, const fs::path& boot) {
    const std::string name = key;
    std::vector<uint8_t> pristine;
    uint64_t size = 0;
    if (!ReadImage(boot, pristine, size)) {
        Check(false, name + ": the real image is readable");
        return;
    }
    const SosSkipIntroSite* site = sos_skip_intro_site(key);
    const char* why = "";
    Check(site && sos_skip_intro_verify(pristine.data(), size, site, &why), name + ": the pristine image is the verified code");

    std::vector<uint8_t> only_xbox = pristine, only_skip = pristine;
    Check(Apply(only_xbox, size, key, {SOS_PROMPTS_XBOX, false}), name + ": the Xbox patch applies alone");
    Check(Apply(only_skip, size, key, {SOS_PROMPTS_PLAYSTATION, true}), name + ": Skip Intro applies alone");
    const auto xbox_bytes = Changed(pristine, only_xbox, size);
    const auto skip_bytes = Changed(pristine, only_skip, size);
    Check(!xbox_bytes.empty() && !skip_bytes.empty(), name + ": each patch writes something");
    Check(std::none_of(xbox_bytes.begin(), xbox_bytes.end(), [&](uint64_t at) { return skip_bytes.count(at) != 0; }),
          name + ": the Xbox patch and Skip Intro write different bytes");

    std::vector<uint8_t> only_hook = pristine;
    Check(Apply(only_hook, size, key, {SOS_PROMPTS_AUTOMATIC, false}), name + ": the Automatic hook applies alone");
    const auto hook_bytes = Changed(pristine, only_hook, size);
    const SosPromptHook* hook = sos_controller_prompts_hook(key);
    Check(hook != nullptr && hook_bytes.size() == 6 && *hook_bytes.begin() == hook->method && *hook_bytes.rbegin() == hook->method + 5,
          name + ": the Automatic hook writes the first six bytes of its method and nothing else of the image");
    Check(std::none_of(hook_bytes.begin(), hook_bytes.end(), [&](uint64_t at) { return skip_bytes.count(at) != 0 || xbox_bytes.count(at) != 0; }),
          name + ": the Automatic hook, the Xbox patch and Skip Intro write different bytes");
    const uint64_t hook_begin = hook->method, hook_end = uint64_t{hook->method} + hook->length;
    std::span<const SosResolutionSite> resolution_sites;
    for (const sos::GameBuild& build : sos::SupportedBuilds()) {
        if (std::string(build.title_id) + "-" + build.version == name) resolution_sites = build.resolution_sites;
    }
    Check(!resolution_sites.empty(), name + ": the build has render resolution sites");
    for (const SosResolutionSite& resolution : resolution_sites) {
        Check(resolution.offset + resolution.length <= hook_begin || resolution.offset >= hook_end,
              name + ": the render resolution site at 0x" + std::to_string(resolution.offset) + " is outside the hooked method");
    }
    Check(std::all_of(only_hook.begin() + static_cast<std::ptrdiff_t>(size), only_hook.begin() + static_cast<std::ptrdiff_t>(size + SOS_SKIP_INTRO_STUB_BYTES),
                      [](uint8_t b) { return b == 0; }),
          name + ": the Automatic hook's stub is outside Skip Intro's stub");
    Check(std::all_of(only_skip.begin() + static_cast<std::ptrdiff_t>(size + SOS_SKIP_INTRO_STUB_BYTES), only_skip.end(), [](uint8_t b) { return b == 0; }),
          name + ": Skip Intro's stub is outside the Automatic hook's stub");

    const Combination matrix[] = {{SOS_PROMPTS_AUTOMATIC, false}, {SOS_PROMPTS_PLAYSTATION, false}, {SOS_PROMPTS_XBOX, false},
                                  {SOS_PROMPTS_AUTOMATIC, true},  {SOS_PROMPTS_PLAYSTATION, true},  {SOS_PROMPTS_XBOX, true}};
    for (const Combination& combination : matrix) {
        const char* style_name = combination.style == SOS_PROMPTS_XBOX ? "Xbox" : combination.style == SOS_PROMPTS_AUTOMATIC ? "Automatic" : "PlayStation";
        const std::string label = name + ": " + style_name + " prompts, Skip Intro " + (combination.skip ? "on" : "off");
        std::vector<uint8_t> image = pristine;
        Check(Apply(image, size, key, combination), label + ": both patches apply in the loader's order");
        Check(sos_controller_prompts_state(image.data(), size, key) == StaticStyle(combination.style), label + ": the static prompt sites hold the requested style");
        const auto changed = Changed(pristine, image, size);
        std::set<uint64_t> expected;
        if (combination.style == SOS_PROMPTS_XBOX) expected.insert(xbox_bytes.begin(), xbox_bytes.end());
        if (combination.style == SOS_PROMPTS_AUTOMATIC) expected.insert(hook_bytes.begin(), hook_bytes.end());
        if (combination.skip) expected.insert(skip_bytes.begin(), skip_bytes.end());
        Check(changed == expected, label + ": exactly the bytes of the patches in use change");
        Check(!combination.skip || (image[site->title_check] == 0xe8 && changed.count(site->title_check) == 1),
              label + ": the title check is the stub call exactly when Skip Intro is on");
        Check(combination.skip || image[site->title_check] == pristine[site->title_check],
              label + ": with Skip Intro off the title's own flag test is untouched");

        std::vector<uint8_t> reversed = pristine;
        Check(Apply(reversed, size, key, combination, true) && std::equal(image.begin(), image.begin() + size, reversed.begin()),
              label + ": the order of the two patches does not matter");

        // The Skip Intro code check must also hold on an image the prompt patch has changed.
        std::vector<uint8_t> prompted = pristine;
        ApplyPrompts(prompted, size, key, combination.style);
        Check(sos_skip_intro_verify(prompted.data(), size, site, &why), label + ": Skip Intro still verifies on the prompt-patched image");
        std::vector<uint8_t> skipped = pristine;
        char message[240];
        sos_skip_intro_apply(skipped.data(), size, key, skipped.data() + size, SOS_SKIP_INTRO_STUB_BYTES, message, sizeof(message));
        Check(sos_prompt_hook_verify(skipped.data(), size, key, &why), label + ": the Automatic hook still verifies on the Skip Intro-patched image");

        for (const int fps : {60, 90}) {
            const unsigned vblank = DcpFramePacing::GuestVblankHz(fps);
            Check(vblank == static_cast<unsigned>(fps) && DcpFramePacing::ShownFramesPerSecond(fps) == static_cast<unsigned>(fps),
                  label + ": " + std::to_string(fps) + " FPS runs the guest vblank at " + std::to_string(fps) + " Hz");
        }
    }
    Check(DcpFramePacing::GuestVblankHz(30) == 60 && DcpFramePacing::ShownFramesPerSecond(30) == 30,
          name + ": 30 FPS keeps the 60 Hz vblank and shows every second flip");
}

}  // namespace

int main() {
    const fs::path root = SOS_SOURCE_DIR;
    for (const char* key : {"CUSA57767-1.00", "CUSA57767-1.01"}) {
        const sos_test::BootImage boot = sos_test::ReportImage(root, key, "the patch combinations");
        if (boot.found()) TestBuild(key, boot.path);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
