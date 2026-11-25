// Controller prompts: setting parsing, the Automatic, PlayStation and Xbox styles, the verified sites of each build, the patch
// and the Automatic hook on a synthetic image and, when the boot images are installed, the real code of CUSA57767 1.00 and
// 1.01. No game, launcher or window is started.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <Zydis/Zydis.h>

#include "build_images.h"
#include "controller_prompts.h"
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

constexpr const char* kBuilds[] = {"CUSA57767-1.00", "CUSA57767-1.01"};
constexpr size_t kWriters = 4;
constexpr size_t kReaders = 10;
constexpr int kXboxSeries = 2;

using Image = std::vector<uint8_t>;

std::vector<SosPromptSite> Sites(const char* key) {
    size_t count = 0;
    const SosPromptSite* sites = sos_controller_prompts_sites(key, &count);
    return sites ? std::vector<SosPromptSite>(sites, sites + count) : std::vector<SosPromptSite>();
}

uint64_t ImageSize(const std::vector<SosPromptSite>& sites) {
    uint64_t end = 0;
    for (const SosPromptSite& site : sites) end = std::max<uint64_t>(end, site.offset + site.length + sizeof(site.after));
    return end + 64;
}

void LaySite(Image& image, const SosPromptSite& site, const uint8_t* bytes) {
    uint8_t* at = image.data() + site.offset;
    std::memcpy(at - sizeof(site.before), site.before, sizeof(site.before));
    std::memcpy(at, bytes, site.length);
    std::memcpy(at + site.length, site.after, sizeof(site.after));
}

Image NewImage(const std::vector<SosPromptSite>& sites) {
    Image image(ImageSize(sites), 0xcc);
    for (const SosPromptSite& site : sites) LaySite(image, site, site.original);
    return image;
}

size_t Differences(const Image& a, const Image& b) {
    size_t count = 0;
    for (size_t i = 0; i < a.size(); ++i) count += a[i] != b[i];
    return count;
}

void TestSettings() {
    int style = -1;
    Check(sos_prompt_style_parse("automatic", &style) && style == SOS_PROMPTS_AUTOMATIC, "automatic parses");
    Check(sos_prompt_style_parse("playstation", &style) && style == SOS_PROMPTS_PLAYSTATION, "playstation parses");
    Check(sos_prompt_style_parse("xbox", &style) && style == SOS_PROMPTS_XBOX, "xbox parses");
    Check(!sos_prompt_style_parse("Xbox", &style) && !sos_prompt_style_parse("Automatic", &style) && !sos_prompt_style_parse("", &style) &&
              !sos_prompt_style_parse(nullptr, &style) && !sos_prompt_style_parse("ps5", nullptr),
          "anything else is not a style");
    Check(std::string(sos_prompt_style_label(SOS_PROMPTS_PLAYSTATION)) == "PlayStation" &&
              std::string(sos_prompt_style_label(SOS_PROMPTS_XBOX)) == "Xbox" &&
              std::string(sos_prompt_style_label(SOS_PROMPTS_AUTOMATIC)) == "Automatic",
          "style labels");
    Check(std::string(SOS_PROMPTS_KEY) == "controller_prompts" && std::string(SOS_PROMPTS_DEFAULT_KEY) == "automatic",
          "setting key and a default of Automatic for new configurations");

    const char* parent = "E:\\ClaudeTemp\\opencode";
    const fs::path folder = (fs::is_directory(parent) ? fs::path(parent) : fs::temp_directory_path()) /
                            ("sos_prompts_test_" + std::to_string(std::rand()));
    fs::create_directories(folder);
    const auto read = [&](const char* text) {
        const fs::path file = folder / "sons_of_sparta.ini";
        std::ofstream(file, std::ios::binary) << text;
        return sos_controller_prompts_from_file(file.string().c_str());
    };
    Check(sos_controller_prompts_from_file((folder / "missing.ini").string().c_str()) == SOS_PROMPTS_AUTOMATIC,
          "a missing file is Automatic");
    Check(sos_controller_prompts_from_file(nullptr) == SOS_PROMPTS_AUTOMATIC, "no file is Automatic");
    Check(read("vsync = 1\n") == SOS_PROMPTS_AUTOMATIC, "a config without the setting is Automatic and keeps its other lines");
    Check(read("controller_prompts = automatic\n") == SOS_PROMPTS_AUTOMATIC, "automatic is read");
    Check(read("controller_prompts = xbox\n") == SOS_PROMPTS_XBOX, "xbox is read");
    Check(read("controller_prompts=xbox\r\n") == SOS_PROMPTS_XBOX, "no spaces and CRLF");
    Check(read("  controller_prompts  =  xbox  # note\n") == SOS_PROMPTS_XBOX, "spaces and a trailing comment");
    Check(read("controller_prompts = playstation\n") == SOS_PROMPTS_PLAYSTATION, "playstation is read");
    Check(read("controller_prompts = ps5\n") == SOS_PROMPTS_AUTOMATIC, "an invalid value falls back to Automatic");
    Check(read("# controller_prompts = xbox\n") == SOS_PROMPTS_AUTOMATIC, "a comment is not a setting");
    Check(read("controller_prompts_extra = xbox\n") == SOS_PROMPTS_AUTOMATIC, "another key is not the setting");
    Check(read("controller_prompts = xbox\ncontroller_prompts = playstation\n") == SOS_PROMPTS_PLAYSTATION,
          "the last line wins");
    std::error_code ec;
    fs::remove_all(folder, ec);
}

void TestSupport() {
    Check(sos_controller_prompts_supported("CUSA57767-1.00"), "1.00 has verified sites");
    Check(sos_controller_prompts_supported("CUSA57767-1.01"), "1.01 has verified sites");
    Check(!sos_controller_prompts_supported("CUSA57767-1.02") && !sos_controller_prompts_supported("CUSA00000-1.00") &&
              !sos_controller_prompts_supported("") && !sos_controller_prompts_supported(nullptr),
          "other builds have none");
}

bool PopsConstant(const SosPromptSite& site) {
    const uint8_t* xbox = site.xbox;
    if (site.length < 3 || xbox[0] != 0x6a || xbox[1] != kXboxSeries) return false;
    size_t at = 2;
    if (xbox[at] == 0x41) ++at;
    if ((xbox[at] & 0xf8) != 0x58) return false;
    for (++at; at < site.length; ++at) {
        if (xbox[at] != 0x90) return false;
    }
    return true;
}

// Every place that writes the controller type stores XboxSeries and every place that reads it yields XboxSeries, so no
// detection that runs later can bring the PlayStation symbols back.
void TestTable(const char* key) {
    const std::string name = key;
    const std::vector<SosPromptSite> sites = Sites(key);
    Check(sites.size() == kWriters + kReaders, name + ": " + std::to_string(kWriters) + " writers and " +
                                                    std::to_string(kReaders) + " readers");
    size_t immediates = 0;
    for (size_t i = 0; i < sites.size(); ++i) {
        const SosPromptSite& site = sites[i];
        const std::string label = name + ": site " + std::to_string(i + 1);
        Check(site.length >= 1 && site.length <= sizeof(site.original), label + " length");
        Check(std::memcmp(site.original, site.xbox, site.length) != 0, label + " changes the code");
        if (i < kWriters) {
            Check(site.immediate == 1 && site.length == 1 && site.xbox[0] == kXboxSeries, label + " stores XboxSeries");
            Check(site.original[0] == 3 || site.original[0] == 4, label + " replaces a PlayStation type");
        } else {
            Check(site.immediate == 0 && site.length == 3 && site.original[0] == 0x8b && site.original[2] == 0x34,
                  label + " is a read of the type field");
            Check(PopsConstant(site) && ((site.original[1] >> 3) & 7) == (site.xbox[2] & 7),
                  label + " yields XboxSeries in the register the game reads into");
        }
        for (size_t j = 0; j < sites.size(); ++j) {
            if (i == j) continue;
            const int64_t from = int64_t(sites[j].offset) - int64_t(sizeof(sites[j].before));
            const int64_t to = int64_t(sites[j].offset) + sites[j].length + int64_t(sizeof(sites[j].after));
            Check(int64_t(site.offset) + site.length <= from || int64_t(site.offset) >= to,
                  label + " is outside the checked bytes of site " + std::to_string(j + 1));
        }
        immediates += site.immediate;
    }
    Check(immediates == kWriters, name + ": only the writers are plain immediates");
}

void TestApply(const char* key, const char* other_key) {
    const std::string name = key;
    const std::vector<SosPromptSite> sites = Sites(key);
    char message[200];
    Image image = NewImage(sites);
    const Image original = image;
    Check(sos_controller_prompts_state(image.data(), image.size(), key) == SOS_PROMPTS_PLAYSTATION,
          name + ": the original code reads as PlayStation");
    Check(sos_controller_prompts_apply(image.data(), image.size(), key, SOS_PROMPTS_XBOX, message, sizeof(message)) ==
              SOS_PROMPTS_XBOX,
          name + ": Xbox applies");
    Check(std::string(message).find("Xbox") != std::string::npos, name + ": the log says Xbox");
    Check(sos_controller_prompts_state(image.data(), image.size(), key) == SOS_PROMPTS_XBOX, name + ": the sites hold Xbox");
    size_t expected = 0;
    for (const SosPromptSite& site : sites) {
        for (size_t k = 0; k < site.length; ++k) expected += site.original[k] != site.xbox[k];
    }
    Check(Differences(image, original) == expected, name + ": only the site bytes changed");
    for (const SosPromptSite& site : sites) {
        Check(std::memcmp(image.data() + site.offset, site.xbox, site.length) == 0, name + ": a site holds its Xbox bytes");
    }
    Check(sos_controller_prompts_apply(image.data(), image.size(), key, SOS_PROMPTS_XBOX, message, sizeof(message)) ==
              SOS_PROMPTS_XBOX,
          name + ": applying Xbox twice is harmless");
    Check(sos_controller_prompts_apply(image.data(), image.size(), key, SOS_PROMPTS_PLAYSTATION, message,
                                       sizeof(message)) == SOS_PROMPTS_PLAYSTATION,
          name + ": PlayStation applies");
    Check(image == original, name + ": PlayStation restores every original byte");
    Check(std::string(message).find("Xbox") == std::string::npos, name + ": the PlayStation log does not say Xbox");
    Check(sos_controller_prompts_apply(image.data(), image.size(), key, SOS_PROMPTS_AUTOMATIC, message, sizeof(message)) ==
                  SOS_PROMPTS_PLAYSTATION &&
              image == original,
          name + ": Automatic leaves the game's own code");

    for (size_t i = 0; i < sites.size(); ++i) {
        const SosPromptSite& site = sites[i];
        Image partial = original;
        std::memcpy(partial.data() + site.offset, site.xbox, site.length);
        Check(sos_controller_prompts_state(partial.data(), partial.size(), key) == -1,
              name + ": a half-patched image is unknown (site " + std::to_string(i + 1) + ")");

        for (const size_t at : {site.offset - 1, site.offset + site.length}) {
            Image broken = original;
            broken[at] ^= 0xff;
            const Image before = broken;
            Check(sos_controller_prompts_apply(broken.data(), broken.size(), key, SOS_PROMPTS_XBOX, message,
                                               sizeof(message)) == -1,
                  name + ": a changed byte next to site " + std::to_string(i + 1) + " refuses");
            Check(broken == before, name + ": a refusal writes nothing");
            Check(std::string(message).find("not the verified code") != std::string::npos,
                  name + ": the refusal is logged");
        }
        Image foreign = original;
        foreign[site.offset] = 0x7f;
        Check(sos_controller_prompts_apply(foreign.data(), foreign.size(), key, SOS_PROMPTS_XBOX, message,
                                           sizeof(message)) == -1,
              name + ": unknown bytes at site " + std::to_string(i + 1) + " refuse");
    }
    Check(sos_controller_prompts_apply(image.data(), sites.back().offset + 4, key, SOS_PROMPTS_XBOX, message,
                                       sizeof(message)) == -1,
          name + ": a truncated image refuses");
    Check(sos_controller_prompts_apply(image.data(), image.size(), "CUSA57767-9.99", SOS_PROMPTS_XBOX, message,
                                       sizeof(message)) == -1 &&
              std::string(message).find("unavailable") != std::string::npos,
          name + ": an unknown build refuses");
    Check(sos_controller_prompts_apply(image.data(), image.size(), nullptr, SOS_PROMPTS_XBOX, message, sizeof(message)) == -1,
          name + ": no build refuses");
    const std::vector<SosPromptSite> other = Sites(other_key);
    Image moved = NewImage(other);
    Check(sos_controller_prompts_state(moved.data(), moved.size(), other_key) == SOS_PROMPTS_PLAYSTATION &&
              sos_controller_prompts_state(moved.data(), moved.size(), key) == -1,
          name + ": the sites are selected by build");
}

void TestValueRanges(const char* key) {
    const std::string name = key;
    const std::vector<SosPromptSite> sites = Sites(key);
    Image image = NewImage(sites);
    uint64_t offsets[16];
    uint8_t lengths[16];
    char message[200];
    const size_t count = sos_controller_prompts_value_ranges(image.data(), image.size(), key, offsets, lengths, 16);
    Check(count == kWriters, name + ": the immediate ranges are the writers");
    for (size_t i = 0; i < count && i < kWriters; ++i) {
        Check(offsets[i] == sites[i].offset && lengths[i] == 1, name + ": range " + std::to_string(i + 1));
    }
    sos_controller_prompts_apply(image.data(), image.size(), key, SOS_PROMPTS_XBOX, message, sizeof(message));
    uint64_t again[16];
    uint8_t again_lengths[16];
    Check(sos_controller_prompts_value_ranges(image.data(), image.size(), key, again, again_lengths, 16) == count &&
              std::memcmp(again, offsets, count * sizeof(offsets[0])) == 0,
          name + ": the same ranges whichever style the code holds");
    Check(sos_controller_prompts_value_ranges(image.data(), image.size(), key, again, again_lengths, 2) == 0,
          name + ": too small a list gives none");
    image[sites[0].offset - 1] ^= 0xff;
    Check(sos_controller_prompts_value_ranges(image.data(), image.size(), key, again, again_lengths, 16) == 0,
          name + ": no ranges for code that is not the verified code");
    Check(sos_controller_prompts_value_ranges(image.data(), image.size(), "CUSA57767-9.99", again, again_lengths, 16) == 0,
          name + ": no ranges for an unknown build");
}

uint64_t ReadU64(std::ifstream& in) {
    uint64_t value = 0;
    in.read(reinterpret_cast<char*>(&value), 8);
    return value;
}

// The site windows of a real boot image (the file's layout is described in scripts/il2cpp_check.py).
bool RealImage(const fs::path& boot, const std::vector<SosPromptSite>& sites, Image& image) {
    std::ifstream in(boot, std::ios::binary);
    char magic[8] = {};
    if (!in || !in.read(magic, 8) || std::memcmp(magic, "SOSBOOT1", 8) != 0) return false;
    const uint64_t size = ReadU64(in), entry = ReadU64(in), segments = ReadU64(in), relocs = ReadU64(in),
                   imports = ReadU64(in);
    (void)entry;
    for (int i = 0; i < 6; ++i) ReadU64(in);
    const uint64_t modules = ReadU64(in);
    in.seekg(static_cast<std::streamoff>(modules * 56), std::ios::cur);
    const uint64_t bindings = ReadU64(in);
    const uint64_t body = static_cast<uint64_t>(in.tellg()) + bindings * 24 + segments * 24 + imports * 128 + relocs * 32;
    if (size < ImageSize(sites) || !in) return false;
    image.assign(ImageSize(sites), 0);
    for (const SosPromptSite& site : sites) {
        const uint64_t from = site.offset - sizeof(site.before);
        in.seekg(static_cast<std::streamoff>(body + from));
        in.read(reinterpret_cast<char*>(image.data() + from), sizeof(site.before) + site.length + sizeof(site.after));
        if (!in) return false;
    }
    return true;
}

void TestRealImages() {
    const fs::path root = SOS_SOURCE_DIR;
    for (const char* key : {"CUSA57767-1.00", "CUSA57767-1.01"}) {
        const sos_test::BootImage found = sos_test::ReportImage(root, key, "the Xbox patch sites");
        if (!found.found()) continue;
        const fs::path boot = found.path;
        const std::string name = key;
        const std::vector<SosPromptSite> sites = Sites(key);
        Image image;
        Check(RealImage(boot, sites, image), name + ": the real image is readable");
        if (image.empty()) continue;
        const Image original = image;
        Check(sos_controller_prompts_state(image.data(), image.size(), key) == SOS_PROMPTS_PLAYSTATION,
              name + ": the real code is the verified original");
        const std::string other = std::string(key) == kBuilds[0] ? kBuilds[1] : kBuilds[0];
        Check(sos_controller_prompts_state(image.data(), image.size(), other.c_str()) == -1,
              name + ": the other build's sites do not match this code");
        char message[200];
        Check(sos_controller_prompts_apply(image.data(), image.size(), key, SOS_PROMPTS_XBOX, message,
                                           sizeof(message)) == SOS_PROMPTS_XBOX,
              name + ": Xbox applies to the real code");
        Check(sos_controller_prompts_state(image.data(), image.size(), key) == SOS_PROMPTS_XBOX,
              name + ": the real code holds Xbox");
        Check(sos_controller_prompts_apply(image.data(), image.size(), key, SOS_PROMPTS_PLAYSTATION, message,
                                           sizeof(message)) == SOS_PROMPTS_PLAYSTATION && image == original,
              name + ": PlayStation restores the real code");
    }
}

// ---- Automatic: when Xbox symbols apply, the override step, the handler and the channel --------------------------------------

constexpr int kKinds[] = {SOS_PAD_NONE, SOS_PAD_PLAYSTATION5, SOS_PAD_PLAYSTATION4, SOS_PAD_OTHER, SOS_PAD_XBOX};
constexpr int32_t kKeyboard = 0, kMouse = 1, kGamepad = 2;
constexpr int32_t kXboxOne = 1, kXboxSeriesType = 2, kPs4 = 3, kPs5 = 4;

void TestUsesXbox() {
    for (const int kind : kKinds) {
        const std::string label = "controller kind " + std::to_string(kind);
        Check(sos_prompt_uses_xbox(SOS_PROMPTS_AUTOMATIC, kind) == (kind == SOS_PAD_XBOX), "Automatic: Xbox symbols only for an Xbox controller (" + label + ")");
        Check(sos_prompt_uses_xbox(SOS_PROMPTS_PLAYSTATION, kind) == 0, "PlayStation: never Xbox symbols (" + label + ")");
        Check(sos_prompt_uses_xbox(SOS_PROMPTS_XBOX, kind) == 1, "Xbox: always Xbox symbols (" + label + ")");
    }
    Check(SOS_PAD_NONE == 0 && SOS_PAD_PLAYSTATION5 == 1 && SOS_PAD_PLAYSTATION4 == 2 && SOS_PAD_OTHER == 3 && SOS_PAD_XBOX == 4,
          "the controller kinds are None, PlayStation 5, PlayStation 4, Other and Xbox");
}

void TestOverrideStep() {
    SosPromptOverride state{};
    int32_t used = kPs5;
    sos_prompt_override_step(&state, 1, kGamepad, &used);
    Check(used == kXboxSeriesType && state.replaced == 1 && state.game_value == kPs5, "an Xbox controller on a gamepad shows XboxSeries and remembers the game's value");
    sos_prompt_override_step(&state, 1, kGamepad, &used);
    Check(used == kXboxSeriesType && state.game_value == kPs5, "a second call changes nothing");
    used = kPs4;
    sos_prompt_override_step(&state, 1, kGamepad, &used);
    Check(used == kXboxSeriesType && state.game_value == kPs5, "a value the game wrote meanwhile is overridden again and the first value kept");
    sos_prompt_override_step(&state, 1, kKeyboard, &used);
    Check(used == kPs5 && state.replaced == 0, "when the input moves to the keyboard the game's own value comes back");
    sos_prompt_override_step(&state, 1, kKeyboard, &used);
    Check(used == kPs5 && state.replaced == 0, "and stays");

    SosPromptOverride mouse{};
    used = kPs4;
    sos_prompt_override_step(&mouse, 1, kMouse, &used);
    Check(used == kPs4 && mouse.replaced == 0, "an Xbox controller with the mouse as input device changes nothing");

    SosPromptOverride playstation{};
    for (const int32_t type : {kPs4, kPs5, kXboxOne}) {
        used = type;
        sos_prompt_override_step(&playstation, 0, kGamepad, &used);
        Check(used == type && playstation.replaced == 0, "without Xbox symbols the game's value is never touched (type " + std::to_string(type) + ")");
    }

    SosPromptOverride own{};
    used = kXboxSeriesType;
    sos_prompt_override_step(&own, 1, kGamepad, &used);
    Check(used == kXboxSeriesType && own.replaced == 0, "a game that already says XboxSeries is left alone");
    used = kPs4;
    sos_prompt_override_step(&own, 0, kGamepad, &used);
    Check(used == kPs4 && own.replaced == 0, "and nothing is restored for it");

    SosPromptOverride unplugged{};
    used = kPs4;
    sos_prompt_override_step(&unplugged, 1, kGamepad, &used);
    sos_prompt_override_step(&unplugged, 0, kGamepad, &used);
    Check(used == kPs4 && unplugged.replaced == 0, "when the Xbox controller goes away the game's value comes back");

    SosPromptOverride changed{};
    used = kPs4;
    sos_prompt_override_step(&changed, 1, kGamepad, &used);
    used = kPs5;
    sos_prompt_override_step(&changed, 0, kGamepad, &used);
    Check(used == kPs5 && changed.replaced == 0, "a value the game wrote after the override is not overwritten when it ends");
}

struct FakeOverseer {
    uint8_t bytes[0x80];
    int32_t& device() { return *reinterpret_cast<int32_t*>(bytes + 0x30); }
    int32_t& used() { return *reinterpret_cast<int32_t*>(bytes + 0x34); }
};

FakeOverseer MakeOverseer(int32_t device, int32_t used) {
    FakeOverseer overseer;
    for (size_t i = 0; i < sizeof(overseer.bytes); ++i) overseer.bytes[i] = static_cast<uint8_t>(0x40 + i);
    overseer.device() = device;
    overseer.used() = used;
    return overseer;
}

bool OnlyUsedChanged(const FakeOverseer& before, const FakeOverseer& after) {
    for (size_t i = 0; i < sizeof(before.bytes); ++i) {
        if ((i < 0x34 || i >= 0x38) && before.bytes[i] != after.bytes[i]) return false;
    }
    return true;
}

void TestHandler() {
    const char* key = "CUSA57767-1.00";
    Check(sos_pad_kind_current() == SOS_PAD_NONE, "no controller kind is published at first");
    sos_pad_kind_publish(SOS_PAD_XBOX);
    Check(sos_pad_kind_current() == SOS_PAD_XBOX, "a published kind is read back");

    sos_prompt_hook_arm(SOS_PROMPTS_AUTOMATIC, key);
    FakeOverseer overseer = MakeOverseer(kGamepad, kPs5);
    FakeOverseer before = overseer;
    sos_prompt_hook_handler(overseer.bytes);
    Check(overseer.used() == kXboxSeriesType && OnlyUsedChanged(before, overseer), "Automatic with an Xbox controller: XboxSeries, nothing else written");
    sos_pad_kind_publish(SOS_PAD_PLAYSTATION4);
    sos_prompt_hook_handler(overseer.bytes);
    Check(overseer.used() == kPs5, "Automatic after switching to a PlayStation 4 controller: the game's own value is back");
    sos_pad_kind_publish(SOS_PAD_XBOX);
    overseer.device() = kKeyboard;
    sos_prompt_hook_handler(overseer.bytes);
    Check(overseer.used() == kPs5, "Automatic with an Xbox controller but keyboard input: the game's own value");
    overseer.device() = kGamepad;
    sos_prompt_hook_handler(overseer.bytes);
    Check(overseer.used() == kXboxSeriesType, "and Xbox symbols again once the gamepad is the input");

    for (const int kind : {SOS_PAD_NONE, SOS_PAD_PLAYSTATION5, SOS_PAD_PLAYSTATION4, SOS_PAD_OTHER}) {
        sos_prompt_hook_arm(SOS_PROMPTS_AUTOMATIC, key);
        sos_pad_kind_publish(kind);
        FakeOverseer other = MakeOverseer(kGamepad, kPs4);
        const FakeOverseer reference = other;
        sos_prompt_hook_handler(other.bytes);
        Check(std::memcmp(other.bytes, reference.bytes, sizeof(other.bytes)) == 0, "Automatic with controller kind " + std::to_string(kind) + ": the overseer is untouched");
    }

    sos_prompt_hook_arm(SOS_PROMPTS_PLAYSTATION, key);
    sos_pad_kind_publish(SOS_PAD_XBOX);
    FakeOverseer forcedPs = MakeOverseer(kGamepad, kPs4);
    const FakeOverseer forcedPsBefore = forcedPs;
    sos_prompt_hook_handler(forcedPs.bytes);
    Check(std::memcmp(forcedPs.bytes, forcedPsBefore.bytes, sizeof(forcedPs.bytes)) == 0, "PlayStation: an Xbox controller never changes the overseer");

    sos_prompt_hook_arm(SOS_PROMPTS_XBOX, key);
    sos_pad_kind_publish(SOS_PAD_PLAYSTATION5);
    FakeOverseer forced = MakeOverseer(kGamepad, kPs5);
    sos_prompt_hook_handler(forced.bytes);
    Check(forced.used() == kXboxSeriesType, "Xbox: XboxSeries whatever the controller");

    sos_prompt_hook_arm(SOS_PROMPTS_AUTOMATIC, key);
    sos_prompt_hook_handler(nullptr);
    Check(true, "a missing overseer is ignored");
    sos_pad_kind_publish(SOS_PAD_NONE);
}

// ---- Automatic: the hook on the game's code ----------------------------------------------------------------------------------

constexpr uint8_t kExpectedHead[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x08, 0x48, 0xb8};
constexpr uint8_t kExpectedTail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};

void TestHookTable(const char* key) {
    const std::string name = key;
    const SosPromptHook* hook = sos_controller_prompts_hook(key);
    Check(hook != nullptr, name + ": has a verified prompt hook");
    if (hook == nullptr) return;
    Check(hook->method != 0 && hook->length >= 0x100 && hook->length < 0x1000 && hook->hash != 0, name + ": the hook has an offset, a length and a hash");
    Check(hook->input_device == 0x30 && hook->controller_used == 0x34, name + ": the overseer's input device is at +0x30 and its controller type at +0x34");
    static const uint8_t prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};
    Check(std::memcmp(hook->prologue, prologue, sizeof(prologue)) == 0, name + ": the method starts with push rbp, mov rbp,rsp, push r15, push r14");
    Check(SOS_PROMPT_HOOK_STOLEN == 6, name + ": three whole instructions of six bytes are stolen");
    Check(sos_controller_prompts_hook("CUSA57767-9.99") == nullptr && sos_controller_prompts_hook(nullptr) == nullptr, name + ": other builds have no hook");
    Check((8 + 6 * 8 + 8) % 16 == 0, name + ": the stub keeps the stack 16-byte aligned at its call");
}

void TestHookRefusals(const char* key) {
    const std::string name = key;
    const SosPromptHook* hook = sos_controller_prompts_hook(key);
    char message[200];
    const uint64_t size = uint64_t{hook->method} + hook->length + 0x400;
    Image blank(size + 256, 0);
    const Image original = blank;
    const char* why = "";
    Check(!sos_prompt_hook_verify(blank.data(), size, key, &why) && std::string(why).size() > 0, name + ": a blank image is not the verified method");
    Check(sos_prompt_hook_install(blank.data(), size, key, blank.data() + size, 128, message, sizeof(message)) == 0 && blank == original &&
              std::string(message).find("not applied") != std::string::npos,
          name + ": installing on a blank image refuses and writes nothing");
    Check(!sos_prompt_hook_verify(blank.data(), uint64_t{hook->method} + 8, key, &why), name + ": a truncated image refuses");
    Check(!sos_prompt_hook_verify(blank.data(), size, "CUSA57767-9.99", &why) && !sos_prompt_hook_verify(blank.data(), size, nullptr, &why), name + ": an unknown build refuses");
    Check(sos_prompt_hook_install(blank.data(), size, "CUSA57767-9.99", blank.data() + size, 128, message, sizeof(message)) == 0 && blank == original, name + ": an unknown build writes nothing");
}

bool ReadBootImage(const fs::path& boot, Image& image, uint64_t extra) {
    std::ifstream in(boot, std::ios::binary);
    char magic[8] = {};
    if (!in || !in.read(magic, 8) || std::memcmp(magic, "SOSBOOT1", 8) != 0) return false;
    const uint64_t size = ReadU64(in), entry = ReadU64(in), segments = ReadU64(in), relocs = ReadU64(in), imports = ReadU64(in);
    (void)entry;
    for (int i = 0; i < 6; ++i) ReadU64(in);
    const uint64_t modules = ReadU64(in);
    in.seekg(static_cast<std::streamoff>(modules * 56), std::ios::cur);
    const uint64_t bindings = ReadU64(in);
    in.seekg(static_cast<std::streamoff>(bindings * 24 + segments * 24 + imports * 128 + relocs * 32), std::ios::cur);
    image.assign(size + extra, 0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(size)));
}

// Decodes the method from its start: the stolen bytes must be three whole instructions that do not depend on their address, and
// no branch or call of the method may land inside them.
void TestHookCode(const std::string& name, const Image& image, const SosPromptHook& hook) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    const uint8_t* method = image.data() + hook.method;
    size_t at = 0;
    std::vector<size_t> lengths;
    while (at < SOS_PROMPT_HOOK_STOLEN) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, method + at, hook.length - at, &instruction, operands))) break;
        Check(!(instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) && instruction.meta.category != ZYDIS_CATEGORY_CALL &&
                  instruction.meta.category != ZYDIS_CATEGORY_UNCOND_BR && instruction.meta.category != ZYDIS_CATEGORY_COND_BR && instruction.meta.category != ZYDIS_CATEGORY_RET,
              name + ": stolen instruction " + std::to_string(lengths.size() + 1) + " is position independent and not a branch");
        for (int k = 0; k < instruction.operand_count; ++k) {
            Check(!(operands[k].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[k].mem.base == ZYDIS_REGISTER_RIP),
                  name + ": stolen instruction " + std::to_string(lengths.size() + 1) + " has no RIP-relative operand");
        }
        lengths.push_back(instruction.length);
        at += instruction.length;
    }
    Check(at == SOS_PROMPT_HOOK_STOLEN && lengths == std::vector<size_t>({1, 3, 2}), name + ": the six stolen bytes are push rbp; mov rbp,rsp; push r15 (whole instructions)");

    std::vector<uint64_t> targets;
    size_t decoded = 0;
    for (at = 0; at < hook.length;) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, method + at, hook.length - at, &instruction, operands))) break;
        if (instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) {
            for (int k = 0; k < instruction.operand_count_visible; ++k) {
                if (operands[k].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operands[k].imm.is_relative) {
                    targets.push_back(at + instruction.length + static_cast<int64_t>(operands[k].imm.value.s));
                }
            }
        }
        at += instruction.length;
        ++decoded;
        if (instruction.meta.category == ZYDIS_CATEGORY_RET && at + 16 >= hook.length) break;
    }
    Check(decoded > 100, name + ": the method decodes (" + std::to_string(decoded) + " instructions swept)");
    Check(std::none_of(targets.begin(), targets.end(), [](uint64_t target) { return target > 0 && target < SOS_PROMPT_HOOK_STOLEN; }),
          name + ": no branch of the method lands inside the stolen bytes");
}

void TestHookReal(const char* key, const fs::path& boot) {
    const std::string name = key;
    const SosPromptHook* hook = sos_controller_prompts_hook(key);
    Image image;
    constexpr uint64_t kStubRoom = SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES;
    if (!ReadBootImage(boot, image, kStubRoom)) {
        Check(false, name + ": the real image is readable");
        return;
    }
    const uint64_t size = image.size() - kStubRoom;
    const Image original = image;
    const char* why = "";
    Check(sos_prompt_hook_verify(image.data(), size, key, &why), name + ": the real method is the verified code (every byte hashed)");
    const std::string other = std::string(key) == kBuilds[0] ? kBuilds[1] : kBuilds[0];
    Check(!sos_prompt_hook_verify(image.data(), size, other.c_str(), &why), name + ": the other build's hook does not match this code");
    TestHookCode(name, image, *hook);

    uint8_t* stub = image.data() + size + SOS_SKIP_INTRO_STUB_BYTES;
    char message[200];
    Check(sos_prompt_hook_install(image.data(), size, key, stub, 40, message, sizeof(message)) == 0 && image == original,
          name + ": a stub area that is too small refuses and writes nothing");
    Check(sos_prompt_hook_install(image.data(), size, key, image.data() + size + (uint64_t{1} << 32), SOS_PROMPT_HOOK_STUB_BYTES, message, sizeof(message)) == 0 && image == original,
          name + ": a stub out of 32-bit reach refuses and writes nothing");
    Check(sos_prompt_hook_install(image.data(), size, key, stub, SOS_PROMPT_HOOK_STUB_BYTES, message, sizeof(message)) == 1, name + ": the hook installs");

    size_t first = 0, last = 0, changed = 0;
    for (uint64_t i = 0; i < size; ++i) {
        if (image[i] == original[i]) continue;
        if (changed++ == 0) first = i;
        last = i;
    }
    Check(changed > 0 && first >= hook->method && last < uint64_t{hook->method} + SOS_PROMPT_HOOK_STOLEN, name + ": only the method's first six bytes of the image change");
    const uint8_t* method = image.data() + hook->method;
    int32_t rel = 0;
    std::memcpy(&rel, method + 1, 4);
    Check(method[0] == 0xe9 && method[5] == 0x90 && method + 5 + rel == stub, name + ": the method starts with a jump to the stub and a nop");
    std::vector<uint8_t> expected(kExpectedHead, kExpectedHead + sizeof(kExpectedHead));
    const uint64_t handler = reinterpret_cast<uint64_t>(&sos_prompt_hook_handler);
    expected.insert(expected.end(), reinterpret_cast<const uint8_t*>(&handler), reinterpret_cast<const uint8_t*>(&handler) + 8);
    expected.insert(expected.end(), kExpectedTail, kExpectedTail + sizeof(kExpectedTail));
    expected.insert(expected.end(), original.begin() + hook->method, original.begin() + hook->method + SOS_PROMPT_HOOK_STOLEN);
    expected.push_back(0xe9);
    const int32_t back = static_cast<int32_t>((method + SOS_PROMPT_HOOK_STOLEN) - (stub + expected.size() + 4));
    expected.insert(expected.end(), reinterpret_cast<const uint8_t*>(&back), reinterpret_cast<const uint8_t*>(&back) + 4);
    Check(std::memcmp(stub, expected.data(), expected.size()) == 0, name + ": the stub saves the argument registers, calls the handler, restores them, replays the stolen bytes and jumps back");
    Check(std::all_of(stub + expected.size(), stub + SOS_PROMPT_HOOK_STUB_BYTES, [](uint8_t b) { return b == 0; }) && expected.size() <= SOS_PROMPT_HOOK_STUB_BYTES,
          name + ": the stub fits its area and writes nothing past itself");

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t at = 0;
    int calls = 0, pushes = 0, pops = 0;
    uint64_t jumpTarget = 0;
    while (at < expected.size()) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, stub + at, expected.size() - at, &instruction, operands))) break;
        calls += instruction.mnemonic == ZYDIS_MNEMONIC_CALL;
        pushes += instruction.mnemonic == ZYDIS_MNEMONIC_PUSH;
        pops += instruction.mnemonic == ZYDIS_MNEMONIC_POP;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_JMP) jumpTarget = reinterpret_cast<uint64_t>(stub + at + instruction.length) + static_cast<int64_t>(operands[0].imm.value.s);
        at += instruction.length;
    }
    Check(at == expected.size(), name + ": the stub decodes to whole instructions");
    Check(calls == 1 && pushes == 8 && pops == 6, name + ": the stub has one call, six argument registers saved and restored, and the two replayed pushes");
    Check(jumpTarget == reinterpret_cast<uint64_t>(method + SOS_PROMPT_HOOK_STOLEN), name + ": the stub returns to the instruction after the stolen bytes");

    Check(sos_controller_prompts_state(image.data(), size, key) == SOS_PROMPTS_PLAYSTATION, name + ": the hook leaves every static prompt site as the game has it");
    Check(!sos_prompt_hook_verify(image.data(), size, key, &why), name + ": a hooked method no longer verifies, so a second install refuses");
    Image twice = image;
    Check(sos_prompt_hook_install(twice.data(), size, key, twice.data() + size + SOS_SKIP_INTRO_STUB_BYTES, SOS_PROMPT_HOOK_STUB_BYTES, message, sizeof(message)) == 0 && twice == image,
          name + ": installing twice refuses and writes nothing");

    Image xbox = original;
    sos_controller_prompts_apply(xbox.data(), size, key, SOS_PROMPTS_XBOX, message, sizeof(message));
    Check(!sos_prompt_hook_verify(xbox.data(), size, key, &why) && sos_prompt_hook_install(xbox.data(), size, key, xbox.data() + size, 128, message, sizeof(message)) == 0,
          name + ": the hook refuses on an image the Xbox patch changed (the two styles never combine)");

    for (const uint32_t offset : {0u, 5u, hook->length / 2, hook->length - 1}) {
        Image damaged = original;
        damaged[hook->method + offset] ^= 0x5a;
        const Image before = damaged;
        Check(sos_prompt_hook_install(damaged.data(), size, key, damaged.data() + size, 128, message, sizeof(message)) == 0 && damaged == before,
              name + ": a changed byte at method+" + std::to_string(offset) + " refuses and writes nothing");
    }

    const SosSkipIntroSite* skip = sos_skip_intro_site(key);
    const uint64_t begin = hook->method, end = uint64_t{hook->method} + hook->length;
    Check(skip != nullptr, name + ": Skip Intro has sites in this build");
    if (skip != nullptr) {
        for (const uint64_t at_skip : {uint64_t{skip->coroutine}, uint64_t{skip->init_save_system}, uint64_t{skip->task_delay}, uint64_t{skip->task_delay_other},
                                       uint64_t{skip->load_save_task}, uint64_t{skip->check_file_exist}, uint64_t{skip->title_check}, uint64_t{skip->base_tick}}) {
            Check(at_skip < begin || at_skip >= end, name + ": Skip Intro site 0x" + std::to_string(at_skip) + " is outside the hooked method");
        }
    }
}

void TestHookRealImages() {
    const fs::path root = SOS_SOURCE_DIR;
    for (const char* key : {"CUSA57767-1.00", "CUSA57767-1.01"}) {
        const sos_test::BootImage boot = sos_test::ReportImage(root, key, "the Automatic prompt hook");
        if (boot.found()) TestHookReal(key, boot.path);
    }
}

} // namespace

int main() {
    TestSettings();
    TestSupport();
    for (const char* key : kBuilds) TestTable(key);
    TestApply(kBuilds[0], kBuilds[1]);
    TestApply(kBuilds[1], kBuilds[0]);
    for (const char* key : kBuilds) TestValueRanges(key);
    TestUsesXbox();
    TestOverrideStep();
    TestHandler();
    for (const char* key : kBuilds) {
        TestHookTable(key);
        TestHookRefusals(key);
    }
    TestRealImages();
    TestHookRealImages();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
