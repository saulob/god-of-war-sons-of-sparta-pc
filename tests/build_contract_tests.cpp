// The port's contract with each supported build, from the tables of the sources alone: no boot image and no game folder is read, so it
// holds the same on every machine. Every build must have every feature, each site must lie inside its image, no patched byte may
// belong to another feature's patch or verified code, and an unknown executable must reach no site.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "build_images.h"
#include "controller_prompts.h"
#include "game_profile.h"
#include "image_glyphs.h"
#include "prompt_dialog.h"
#include "sos_exit_game.h"
#include "sos_hermes_unlock.h"
#include "sos_pit_unlock.h"
#include "sos_skip_intro.h"

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

// `styles` are the controller prompt styles that install it: 1 Xbox, 2 Automatic. The Xbox patch and the Automatic hooks never meet.
enum { kXbox = 1, kAutomatic = 2, kBoth = 3 };

struct Range {
    uint64_t begin, end;
    std::string owner;
    int styles;
};

struct Layout {
    std::vector<Range> writes, reads;
};

void Write(Layout& layout, const std::string& owner, uint64_t at, uint64_t length, int styles = kBoth) { layout.writes.push_back({at, at + length, owner, styles}); }
void Read(Layout& layout, const std::string& owner, uint64_t at, uint64_t length, int styles = kBoth) { layout.reads.push_back({at, at + length, owner, styles}); }

bool Overlap(const Range& a, const Range& b) { return (a.styles & b.styles) != 0 && a.begin < b.end && b.begin < a.end; }

std::vector<std::string> Features(const sos::GameBuild& build, const std::string& key) {
    std::vector<std::string> found;
    size_t count = 0;
    if (sos_skip_intro_site(key.c_str())) found.push_back("skip-intro");
    if (sos_hermes_unlock_site(key.c_str())) found.push_back("hermes");
    if (sos_pit_unlock_site(key.c_str())) found.push_back("pit");
    if (sos_exit_game_site(key.c_str())) found.push_back("exit-game");
    if (sos_controller_prompts_sites(key.c_str(), &count) && count) found.push_back("xbox-prompts");
    if (sos_controller_prompts_hook(key.c_str())) found.push_back("prompt-hook");
    if (sos_prompt_dialog_site(key.c_str())) found.push_back("dialog-menus");
    if (sos_image_glyphs_site(key.c_str())) found.push_back("glyph-images");
    if (build.resolution_sites.size() >= SOS_SET_RESOLUTION_SITE_COUNT) found.push_back("resolution");
    return found;
}

Layout LayoutOf(const sos::GameBuild& build, const std::string& key) {
    Layout layout;
    const char* k = key.c_str();
    if (const SosHermesUnlockSite* hermes = sos_hermes_unlock_site(k)) {
        Write(layout, "hermes", hermes->gate, 6);
        Read(layout, "hermes", hermes->gate - 10, 16);
    }
    if (const SosPitUnlockSite* pit = sos_pit_unlock_site(k)) {
        const SosPitUnlockPattern& pattern = *sos_pit_unlock_pattern();
        Write(layout, "pit", pit->gate, 2);
        Read(layout, "pit", pit->gate - pattern.jump, pattern.length);
    }
    if (const SosExitGameSite* exit = sos_exit_game_site(k)) {
        Write(layout, "exit-game", exit->toggle_method, SOS_EXIT_GAME_TOGGLE_STOLEN);
        Write(layout, "exit-game", exit->confirm_method, SOS_EXIT_GAME_CONFIRM_STOLEN);
        Write(layout, "exit-game", exit->quit_method, 5);
        Write(layout, "exit-game", exit->text_method, SOS_EXIT_GAME_TEXT_STOLEN);
        for (const SosExitGameRange& range : exit->ranges) Read(layout, "exit-game", range.begin, range.end - range.begin);
    }
    size_t count = 0;
    if (const SosPromptSite* sites = sos_controller_prompts_sites(k, &count))
        for (size_t i = 0; i < count; ++i) {
            Write(layout, "xbox-prompts", sites[i].offset, sites[i].length, kXbox);
            Read(layout, "xbox-prompts", sites[i].offset - sizeof(sites[i].before), sizeof(sites[i].before) + sites[i].length + sizeof(sites[i].after), kXbox);
        }
    if (const SosPromptHook* hook = sos_controller_prompts_hook(k)) {
        Write(layout, "prompt-hook", hook->method, SOS_PROMPT_HOOK_STOLEN, kAutomatic);
        Read(layout, "prompt-hook", hook->method, hook->length, kAutomatic);
    }
    if (const SosPromptDialogSite* dialog = sos_prompt_dialog_site(k)) {
        Write(layout, "dialog-menus", dialog->method, SOS_PROMPT_DIALOG_ENTRY_BYTES);
        Read(layout, "dialog-menus", dialog->method, SOS_PROMPT_DIALOG_ENTRY_BYTES);
        for (const SosPromptDialogRange& range : dialog->ranges) Read(layout, "dialog-menus", range.begin, range.end - range.begin);
        for (const SosPromptMenu& menu : dialog->menus) {
            Write(layout, "dialog-menus", menu.method, SOS_PROMPT_MENU_STOLEN);
            Read(layout, "dialog-menus", menu.method, menu.length);
        }
    }
    if (const SosImageGlyphSite* glyphs = sos_image_glyphs_site(k)) {
        for (int i = 0; i < SOS_GLYPH_HOOKS; ++i) {
            const int styles = i < SOS_GLYPH_HOOK_TEXT_ATLAS ? kBoth : kAutomatic;
            Write(layout, "glyph-images", glyphs->hooks[i].method, SOS_GLYPH_HOOK_STOLEN, styles);
            Read(layout, "glyph-images", glyphs->hooks[i].method, glyphs->hooks[i].length, styles);
        }
        for (const SosGlyphRange& range : glyphs->services) Read(layout, "glyph-images", range.begin, range.end - range.begin);
    }
    for (const SosResolutionSite& site : build.resolution_sites) Write(layout, "resolution", site.offset, site.length);
    return layout;
}

void TestBuild(const sos::GameBuild& build, const std::vector<std::string>& expected) {
    const std::string key = std::string(build.title_id) + "-" + build.version;
    Check(sos_test::Sha256Of(key) != nullptr && std::string(sos_test::Sha256Of(key)) == build.eboot_sha256, key + ": the fingerprint of the launcher and of the loader are the same");
    Check(std::string(build.label).find(build.version) != std::string::npos, key + ": the label names the version");
    Check(build.skip_intro, key + ": Skip Intro is enabled");
    Check(sos::IsUpdateBuild(build) == (std::string(build.version) != "1.00"), key + ": only the update has an update layer");
    if (sos::IsUpdateBuild(build)) {
        const sos::GameBuild* base = sos::BaseBuildOf(build);
        Check(base != nullptr && std::string(base->version) == "1.00", key + ": its base build is CUSA57767 1.00");
    }

    Check(Features(build, key) == expected, key + ": it has every feature the other builds have");
    const SosPitUnlockSite* pit = sos_pit_unlock_site(key.c_str());
    Check(pit != nullptr, key + ": its image size is known");
    const Layout layout = LayoutOf(build, key);
    Check(!layout.writes.empty() && !layout.reads.empty(), key + ": its patch and verified ranges are known");
    const auto inside = [&](const Range& range) { return range.begin < range.end && pit && range.end <= pit->image_size; };
    Check(std::all_of(layout.writes.begin(), layout.writes.end(), inside), key + ": every patched range lies inside the image");
    Check(std::all_of(layout.reads.begin(), layout.reads.end(), inside), key + ": every verified range lies inside the image");
    const auto at = [](const Range& a, const Range& b) {
        char text[96];
        std::snprintf(text, sizeof(text), " %s 0x%llx / %s 0x%llx;", a.owner.c_str(), static_cast<unsigned long long>(a.begin), b.owner.c_str(), static_cast<unsigned long long>(b.begin));
        return std::string(text);
    };
    std::string shared, verified;
    for (size_t i = 0; i < layout.writes.size(); ++i)
        for (size_t j = i + 1; j < layout.writes.size(); ++j)
            if (Overlap(layout.writes[i], layout.writes[j])) shared += at(layout.writes[i], layout.writes[j]);
    for (const Range& write : layout.writes)
        for (const Range& read : layout.reads)
            if (write.owner != read.owner && Overlap(write, read)) verified += at(write, read);
    Check(shared.empty(), key + ": no two patches write the same byte:" + shared);
    Check(verified.empty(), key + ": no patch writes a byte another feature verifies:" + verified);
    sos_test::ReportContract(key, "registration, features, ranges and unknown-build refusal from the source tables");
}

void TestUnknown() {
    const char* const names[] = {nullptr, "", "CUSA57767", "CUSA57767-9.99", "CUSA57767-1.0", "CUSA57767-1.000", "cusa57767-1.00", "CUSA57767-1.00 ", " CUSA57767-1.01", "CUSA00000-1.00"};
    for (const char* name : names) {
        const std::string label = std::string("'") + (name ? name : "(none)") + "'";
        size_t count = 0;
        Check(!sos_skip_intro_site(name) && !sos_hermes_unlock_site(name) && !sos_pit_unlock_site(name) && !sos_exit_game_site(name) &&
                  !sos_controller_prompts_sites(name, &count) && !sos_controller_prompts_hook(name) && !sos_prompt_dialog_site(name) &&
                  !sos_image_glyphs_site(name),
              label + " reaches no site");
        Check(!sos_controller_prompts_supported(name), label + " is not a supported build");
    }
    for (const sos::GameBuild& build : sos::SupportedBuilds()) {
        Check(!sos::IsBuildExecutable(build, "0000000000000000000000000000000000000000000000000000000000000000"), std::string(build.version) + ": another executable is not this build");
        Check(!sos::IsBuildExecutable(build, ""), std::string(build.version) + ": no executable is not this build");
    }
}

}  // namespace

int main() {
    const std::vector<sos::GameBuild> builds(sos::SupportedBuilds().begin(), sos::SupportedBuilds().end());
    Check(builds.size() == 2, "CUSA57767 1.00 and CUSA57767 1.01 are the supported builds");
    std::set<std::string> versions;
    for (const sos::GameBuild& build : builds) versions.insert(build.version);
    Check(versions == std::set<std::string>{"1.00", "1.01"}, "the supported versions are 1.00 and 1.01");
    std::vector<std::string> expected;
    if (!builds.empty()) expected = Features(builds.front(), std::string(builds.front().title_id) + "-" + builds.front().version);
    std::string names;
    for (const std::string& name : expected) names += " " + name;
    Check(expected.size() == 9, "the first build has the nine features:" + names);
    for (const sos::GameBuild& build : builds) TestBuild(build, expected);
    TestUnknown();
    std::printf("build contract tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
