// Controller prompts: setting parsing, the verified sites of each build, the patch on a synthetic image and, when the boot
// images are installed, the real code of CUSA57767 1.00 and 1.01. No game, launcher or window is started.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "controller_prompts.h"

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
    Check(sos_prompt_style_parse("playstation", &style) && style == SOS_PROMPTS_PLAYSTATION, "playstation parses");
    Check(sos_prompt_style_parse("xbox", &style) && style == SOS_PROMPTS_XBOX, "xbox parses");
    Check(!sos_prompt_style_parse("Xbox", &style) && !sos_prompt_style_parse("", &style) &&
              !sos_prompt_style_parse(nullptr, &style) && !sos_prompt_style_parse("ps5", nullptr),
          "anything else is not a style");
    Check(std::string(sos_prompt_style_label(SOS_PROMPTS_PLAYSTATION)) == "PlayStation (Original)" &&
              std::string(sos_prompt_style_label(SOS_PROMPTS_XBOX)) == "Xbox",
          "style labels");
    Check(std::string(SOS_PROMPTS_KEY) == "controller_prompts" && std::string(SOS_PROMPTS_DEFAULT_KEY) == "playstation",
          "setting key and default");

    const char* parent = "E:\\ClaudeTemp\\opencode";
    const fs::path folder = (fs::is_directory(parent) ? fs::path(parent) : fs::temp_directory_path()) /
                            ("sos_prompts_test_" + std::to_string(std::rand()));
    fs::create_directories(folder);
    const auto read = [&](const char* text) {
        const fs::path file = folder / "sons_of_sparta.ini";
        std::ofstream(file, std::ios::binary) << text;
        return sos_controller_prompts_from_file(file.string().c_str());
    };
    Check(sos_controller_prompts_from_file((folder / "missing.ini").string().c_str()) == SOS_PROMPTS_PLAYSTATION,
          "a missing file keeps PlayStation");
    Check(sos_controller_prompts_from_file(nullptr) == SOS_PROMPTS_PLAYSTATION, "no file keeps PlayStation");
    Check(read("vsync = 1\n") == SOS_PROMPTS_PLAYSTATION, "a missing line keeps PlayStation");
    Check(read("controller_prompts = xbox\n") == SOS_PROMPTS_XBOX, "xbox is read");
    Check(read("controller_prompts=xbox\r\n") == SOS_PROMPTS_XBOX, "no spaces and CRLF");
    Check(read("  controller_prompts  =  xbox  # note\n") == SOS_PROMPTS_XBOX, "spaces and a trailing comment");
    Check(read("controller_prompts = playstation\n") == SOS_PROMPTS_PLAYSTATION, "playstation is read");
    Check(read("controller_prompts = ps5\n") == SOS_PROMPTS_PLAYSTATION, "an invalid value falls back");
    Check(read("# controller_prompts = xbox\n") == SOS_PROMPTS_PLAYSTATION, "a comment is not a setting");
    Check(read("controller_prompts_extra = xbox\n") == SOS_PROMPTS_PLAYSTATION, "another key is not the setting");
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
    struct Real {
        const char* key;
        const char* folders[2];
    };
    const Real builds[] = {{"CUSA57767-1.00", {"CUSA57767-1.00", "v1.00"}}, {"CUSA57767-1.01", {"CUSA57767-1.01", "v1.01"}}};
    bool any = false;
    for (const Real& build : builds) {
        fs::path boot;
        for (const char* folder : build.folders) {
            const fs::path candidate = root / "build" / "image" / folder / "boot.bin";
            if (boot.empty() && fs::is_regular_file(candidate)) boot = candidate;
        }
        if (boot.empty()) {
            std::printf("  [skip] %s: no boot image\n", build.key);
            continue;
        }
        any = true;
        const std::string name = build.key;
        const std::vector<SosPromptSite> sites = Sites(build.key);
        Image image;
        Check(RealImage(boot, sites, image), name + ": the real image is readable");
        if (image.empty()) continue;
        const Image original = image;
        Check(sos_controller_prompts_state(image.data(), image.size(), build.key) == SOS_PROMPTS_PLAYSTATION,
              name + ": the real code is the verified original");
        const std::string other = std::string(build.key) == kBuilds[0] ? kBuilds[1] : kBuilds[0];
        Check(sos_controller_prompts_state(image.data(), image.size(), other.c_str()) == -1,
              name + ": the other build's sites do not match this code");
        char message[200];
        Check(sos_controller_prompts_apply(image.data(), image.size(), build.key, SOS_PROMPTS_XBOX, message,
                                           sizeof(message)) == SOS_PROMPTS_XBOX,
              name + ": Xbox applies to the real code");
        Check(sos_controller_prompts_state(image.data(), image.size(), build.key) == SOS_PROMPTS_XBOX,
              name + ": the real code holds Xbox");
        Check(sos_controller_prompts_apply(image.data(), image.size(), build.key, SOS_PROMPTS_PLAYSTATION, message,
                                           sizeof(message)) == SOS_PROMPTS_PLAYSTATION && image == original,
              name + ": PlayStation restores the real code");
    }
    if (!any) std::printf("  [skip] no installed boot image: the real code was not read\n");
}

} // namespace

int main() {
    TestSettings();
    TestSupport();
    for (const char* key : kBuilds) TestTable(key);
    TestApply(kBuilds[0], kBuilds[1]);
    TestApply(kBuilds[1], kBuilds[0]);
    for (const char* key : kBuilds) TestValueRanges(key);
    TestRealImages();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
