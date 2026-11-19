// Controller prompts: setting parsing, the verified sites of each build, the patch on a synthetic image and, when the
// boot images are installed, the real code of CUSA57767 1.00 and 1.01. No game, launcher or window is started.
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

struct Site {
    std::vector<uint8_t> before;
    std::vector<uint8_t> instruction;
    std::vector<uint8_t> after;
    uint8_t original;
};

std::vector<uint8_t> Hex(const char* text) {
    std::vector<uint8_t> bytes;
    for (const char* p = text; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        bytes.push_back(static_cast<uint8_t>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
        p += 2;
    }
    return bytes;
}

const Site kSites[4] = {
    {Hex("48 8b 47 10 c3 00 00 00 00 00 00 00 00 00 00 00"), Hex("c7 47 34 04 00 00 00"), Hex("c6 47 10 01 c3"), 4},
    {Hex("30 01 00 00 72 22 48 8b 92 c8 00 00 00 48 39 4c f2 f8 75 14 48 8d 7b 38"), Hex("c7 43 34 03 00 00 00"),
     Hex("48 89 43 38"), 3},
    {Hex("00 72 0e 48 8b 92 c8 00 00 00 48 39 4c f2 f8 74 07 48 83 c4 08 5b 5d c3"), Hex("c7 43 34 03 00 00 00"),
     Hex("48 8d 7b 38 48 89 43 38"), 3},
    {Hex("ff ff 48 8b 92 c8 00 00 00 48 39 4c f2 f8 0f 85 52 fd ff ff 49 8d 7e 38"), Hex("41 c7 01 03 00 00 00"),
     Hex("48 89 07"), 3},
};

const uint32_t kOffsets100[4] = {0x47c0710, 0x47bce56, 0x47bcf4c, 0x47c0543};
const uint32_t kOffsets101[4] = {0x47c59a0, 0x47c20e6, 0x47c21dc, 0x47c57d3};
const uint64_t kImageSize = 0x4800000;

void Lay(std::vector<uint8_t>& image, const uint32_t (&offsets)[4]) {
    for (int i = 0; i < 4; ++i) {
        const Site& site = kSites[i];
        uint8_t* at = image.data() + offsets[i];
        std::memcpy(at - site.before.size(), site.before.data(), site.before.size());
        std::memcpy(at, site.instruction.data(), site.instruction.size());
        std::memcpy(at + site.instruction.size(), site.after.data(), site.after.size());
        at[3] = site.original;
    }
}

size_t Immediate(int site, const uint32_t (&offsets)[4]) { return offsets[site] + 3; }

std::vector<uint8_t> NewImage(const uint32_t (&offsets)[4]) {
    std::vector<uint8_t> image(kImageSize, 0xcc);
    Lay(image, offsets);
    return image;
}

size_t Differences(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
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

void TestApply(const char* key, const uint32_t (&offsets)[4], const uint32_t (&other)[4], const char* other_key) {
    const std::string name = key;
    char message[200];
    std::vector<uint8_t> image = NewImage(offsets);
    const std::vector<uint8_t> original = image;
    Check(sos_controller_prompts_state(image.data(), image.size(), key) == SOS_PROMPTS_PLAYSTATION,
          name + ": the original code reads as PlayStation");
    Check(sos_controller_prompts_apply(image.data(), image.size(), key, SOS_PROMPTS_XBOX, message, sizeof(message)) ==
              SOS_PROMPTS_XBOX,
          name + ": Xbox applies");
    Check(std::string(message).find("Xbox") != std::string::npos, name + ": the log says Xbox");
    Check(sos_controller_prompts_state(image.data(), image.size(), key) == SOS_PROMPTS_XBOX, name + ": the sites hold Xbox");
    Check(Differences(image, original) == 4, name + ": exactly four bytes changed");
    for (int i = 0; i < 4; ++i) {
        Check(image[Immediate(i, offsets)] == 2, name + ": site " + std::to_string(i + 1) + " stores XboxSeries");
    }
    Check(sos_controller_prompts_apply(image.data(), image.size(), key, SOS_PROMPTS_XBOX, message, sizeof(message)) ==
              SOS_PROMPTS_XBOX,
          name + ": applying Xbox twice is harmless");
    Check(sos_controller_prompts_apply(image.data(), image.size(), key, SOS_PROMPTS_PLAYSTATION, message,
                                       sizeof(message)) == SOS_PROMPTS_PLAYSTATION,
          name + ": PlayStation applies");
    Check(image == original, name + ": PlayStation restores every original byte");

    std::vector<uint8_t> partial = original;
    partial[Immediate(2, offsets)] = 2;
    Check(sos_controller_prompts_state(partial.data(), partial.size(), key) == -1, name + ": a half-patched image is unknown");

    for (int i = 0; i < 4; ++i) {
        std::vector<uint8_t> broken = original;
        broken[offsets[i] - 1] ^= 0xff;
        const std::vector<uint8_t> before = broken;
        Check(sos_controller_prompts_apply(broken.data(), broken.size(), key, SOS_PROMPTS_XBOX, message, sizeof(message)) == -1,
              name + ": a changed byte before site " + std::to_string(i + 1) + " refuses");
        Check(broken == before, name + ": a refusal writes nothing");
        Check(std::string(message).find("not the verified code") != std::string::npos, name + ": the refusal is logged");
        std::vector<uint8_t> after = original;
        after[offsets[i] + 7] ^= 0xff;
        Check(sos_controller_prompts_apply(after.data(), after.size(), key, SOS_PROMPTS_XBOX, message, sizeof(message)) == -1,
              name + ": a changed byte after site " + std::to_string(i + 1) + " refuses");
        std::vector<uint8_t> foreign = original;
        foreign[Immediate(i, offsets)] = 7;
        Check(sos_controller_prompts_apply(foreign.data(), foreign.size(), key, SOS_PROMPTS_XBOX, message, sizeof(message)) ==
                  -1,
              name + ": an unknown immediate at site " + std::to_string(i + 1) + " refuses");
    }
    Check(sos_controller_prompts_apply(image.data(), offsets[0] + 4, key, SOS_PROMPTS_XBOX, message, sizeof(message)) == -1,
          name + ": a truncated image refuses");
    Check(sos_controller_prompts_apply(image.data(), image.size(), "CUSA57767-9.99", SOS_PROMPTS_XBOX, message,
                                       sizeof(message)) == -1 &&
              std::string(message).find("unavailable") != std::string::npos,
          name + ": an unknown build refuses");
    std::vector<uint8_t> moved = NewImage(other);
    Check(sos_controller_prompts_state(moved.data(), moved.size(), other_key) == SOS_PROMPTS_PLAYSTATION &&
              sos_controller_prompts_state(moved.data(), moved.size(), key) == -1,
          name + ": the sites are selected by build");
}

uint64_t ReadU64(std::ifstream& in) {
    uint64_t value = 0;
    in.read(reinterpret_cast<char*>(&value), 8);
    return value;
}

bool RealImage(const fs::path& boot, const uint32_t (&offsets)[4], std::vector<uint8_t>& image) {
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
    if (size < kImageSize || !in) return false;
    image.assign(kImageSize, 0);
    for (int i = 0; i < 4; ++i) {
        const uint64_t from = offsets[i] - 24;
        in.seekg(static_cast<std::streamoff>(body + from));
        in.read(reinterpret_cast<char*>(image.data() + from), 24 + 24 + 16);
        if (!in) return false;
    }
    return true;
}

void TestRealImages() {
    const fs::path root = SOS_SOURCE_DIR;
    struct Real {
        const char* key;
        const char* folder;
        const uint32_t (*offsets)[4];
    };
    const Real builds[] = {{"CUSA57767-1.00", "CUSA57767-1.00", &kOffsets100}, {"CUSA57767-1.01", "v1.01", &kOffsets101}};
    bool any = false;
    for (const Real& build : builds) {
        std::vector<uint8_t> image;
        const fs::path boot = root / "build" / "image" / build.folder / "boot.bin";
        if (!fs::is_regular_file(boot)) {
            std::printf("  [skip] %s: no boot image\n", build.key);
            continue;
        }
        any = true;
        Check(RealImage(boot, *build.offsets, image), std::string(build.key) + ": the real image is readable");
        Check(sos_controller_prompts_state(image.data(), image.size(), build.key) == SOS_PROMPTS_PLAYSTATION,
              std::string(build.key) + ": the real code is the verified original");
        char message[200];
        Check(sos_controller_prompts_apply(image.data(), image.size(), build.key, SOS_PROMPTS_XBOX, message, sizeof(message)) ==
                  SOS_PROMPTS_XBOX,
              std::string(build.key) + ": Xbox applies to the real code");
    }
    if (!any) std::printf("  [skip] no installed boot image: the real code was not read\n");
}

} // namespace

int main() {
    TestSettings();
    TestSupport();
    TestApply("CUSA57767-1.00", kOffsets100, kOffsets101, "CUSA57767-1.01");
    TestApply("CUSA57767-1.01", kOffsets101, kOffsets100, "CUSA57767-1.00");
    TestRealImages();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
