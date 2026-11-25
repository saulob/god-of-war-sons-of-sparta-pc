// The Pit early unlock on the real boot images of CUSA57767 1.00 and 1.01: the site verifies, the patch changes only the
// two bytes of the title menu's branch, it stays independent of Hermes Mode, Skip Intro and Exit Game, and an unknown
// build, a changed image or a game without the Pit scenes is left alone. No game, launcher or window is started.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "build_images.h"
#include "sos_exit_game.h"
#include "sos_hermes_unlock.h"
#include "sos_pit_unlock.h"
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
    image.assign(size, 0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(size)));
}

std::set<uint64_t> Changed(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::set<uint64_t> changed;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) changed.insert(i);
    return changed;
}

struct SparseImage {
    uint8_t* data;
    uint64_t size;
    explicit SparseImage(uint64_t bytes) : data(static_cast<uint8_t*>(std::calloc(static_cast<size_t>(bytes), 1))), size(bytes) {}
    ~SparseImage() { std::free(data); }
    SparseImage(const SparseImage&) = delete;
    SparseImage& operator=(const SparseImage&) = delete;
};

// The verified check of the title menu, laid on a zeroed image of the build's size from the pattern the source verifies.
void LayCheck(SparseImage& image, const SosPitUnlockSite& site) {
    const SosPitUnlockPattern& pattern = *sos_pit_unlock_pattern();
    const uint64_t start = site.gate - pattern.jump;
    for (int i = 0; i < pattern.length; ++i) image.data[start + i] = pattern.bytes[i] >= 0 ? static_cast<uint8_t>(pattern.bytes[i]) : 0xab;
    for (const int call : {pattern.pit_call, pattern.hermes_call}) {
        const int32_t relative = static_cast<int32_t>(static_cast<int64_t>(site.invoke) - static_cast<int64_t>(start + call + 5));
        std::memcpy(image.data + start + call + 1, &relative, sizeof(relative));
    }
}

bool ZeroOutside(const SparseImage& image, uint64_t low, uint64_t high) {
    static const std::vector<uint8_t> zeros(1 << 20, 0);
    for (uint64_t at = 0; at < image.size; at += zeros.size()) {
        const uint64_t end = std::min<uint64_t>(at + zeros.size(), image.size);
        for (const auto& [from, to] : {std::pair<uint64_t, uint64_t>{at, std::min(end, low)}, {std::max(at, high), end}})
            if (from < to && std::memcmp(image.data + from, zeros.data(), static_cast<size_t>(to - from)) != 0) return false;
    }
    return true;
}

void WriteFile(const fs::path& file, const char* text) {
    std::ofstream out(file, std::ios::binary);
    out << text;
}

fs::path MakeGameFolder(const fs::path& folder) {
    const fs::path media = folder / "Media";
    fs::create_directories(media / "StreamingAssets" / "Scenes");
    for (int scene = 175; scene <= 219; ++scene) {
        if (scene == 201) continue;
        WriteFile(media / ("level" + std::to_string(scene)), "x");
        WriteFile(media / ("sharedassets" + std::to_string(scene) + ".assets"), "x");
    }
    for (const char* bank : {"PitofAgonies.bank", "PitofAgonies.assets.bank", "PitofAgonies.streams.bank"})
        WriteFile(media / "StreamingAssets" / "Scenes" / bank, "x");
    return folder;
}

void TestSettings(const fs::path& folder) {
    const fs::path file = folder / "settings.ini";
    const auto read = [&](const char* text) {
        WriteFile(file, text);
        return sos_pit_unlock_from_file(file.string().c_str());
    };
    Check(sos_pit_unlock_from_file(nullptr) == 0, "unlock_the_pit defaults to off without a file");
    Check(sos_pit_unlock_from_file((folder / "absent.ini").string().c_str()) == 0, "unlock_the_pit defaults to off when the file is absent");
    Check(read("skip_intro = 1\n") == 0, "unlock_the_pit defaults to off without the key");
    Check(read("unlock_the_pit = 1\n") == 1, "unlock_the_pit on is read");
    Check(read("unlock_the_pit = 0\n") == 0, "unlock_the_pit off is read");
    Check(read("unlock_the_pit = maybe\n") == 0, "an unknown unlock_the_pit value keeps the default off");
    Check(read("# unlock_the_pit = 1\n") == 0, "a commented unlock_the_pit stays off");
    Check(read("unlock_hermes_mode = 1\n") == 0 && sos_hermes_unlock_from_file(file.string().c_str()) == 1,
          "Hermes Mode on leaves The Pit off");
    Check(read("unlock_the_pit = 1\n") == 1 && sos_hermes_unlock_from_file(file.string().c_str()) == 0,
          "The Pit on leaves Hermes Mode off");
}

void TestContent(const fs::path& folder) {
    const fs::path game = MakeGameFolder(folder / "content");
    const std::string path = game.string();
    char missing[64] = {};
    Check(sos_pit_unlock_content(path.c_str(), missing, sizeof(missing)) == 1, "a complete set of Pit files is accepted");
    Check(sos_pit_unlock_content(nullptr, missing, sizeof(missing)) == 0, "no game folder means no Pit content");
    for (const char* name : {"level175", "level180", "level200", "level202", "level219", "sharedassets180.assets", "sharedassets219.assets",
                             "StreamingAssets/Scenes/PitofAgonies.bank", "StreamingAssets/Scenes/PitofAgonies.streams.bank"}) {
        const fs::path file = game / "Media" / name;
        fs::remove(file);
        missing[0] = 0;
        Check(sos_pit_unlock_content(path.c_str(), missing, sizeof(missing)) == 0 && std::string(missing) == name,
              std::string("a game without ") + name + " is refused");
        WriteFile(file, "x");
    }
}

void TestBuild(const char* key, const fs::path& boot, const fs::path& prepared, const fs::path& folder) {
    const std::string name = key;
    std::vector<uint8_t> pristine;
    uint64_t size = 0;
    if (!ReadImage(boot, pristine, size)) {
        Check(false, name + ": the real image is readable");
        return;
    }
    const std::string game = MakeGameFolder(folder / name).string();
    const SosPitUnlockSite* site = sos_pit_unlock_site(key);
    const char* why = "";
    char message[200] = {};
    Check(site != nullptr, name + ": the build has a site");
    if (!site) return;
    Check(site->image_size == size, name + ": the site's image size is the real image size");
    Check(sos_pit_unlock_verify(pristine.data(), size, site, &why), name + ": the pristine image is the verified code");
    if (fs::is_directory(prepared))
        Check(sos_pit_unlock_content(prepared.string().c_str(), message, sizeof(message)) == 1,
              name + ": the prepared game has every Pit file");

    std::vector<uint8_t> image = pristine;
    Check(sos_pit_unlock_apply(image.data(), size, key, game.c_str(), message, sizeof(message)) == 1, name + ": the patch applies");
    Check(std::strstr(message, "offered") != nullptr, name + ": the patch reports The Pit as offered");
    const auto changed = Changed(pristine, image);
    Check(changed == std::set<uint64_t>{site->gate, site->gate + 1}, name + ": exactly the two branch bytes change");
    Check(image[site->gate] == 0x90 && image[site->gate + 1] == 0x90, name + ": the branch becomes NOPs");
    Check(pristine[site->gate] == 0x74 && pristine[site->gate + 1] == 0x13, name + ": the original branch is je +0x13");
    Check(!sos_pit_unlock_verify(image.data(), size, site, &why), name + ": the patched image is no longer the pristine code");

    std::vector<uint8_t> again = image;
    Check(sos_pit_unlock_apply(again.data(), size, key, game.c_str(), message, sizeof(message)) == 0 && again == image,
          name + ": a second apply writes nothing");

    const auto refuses = [&](std::vector<uint8_t> copy, uint64_t copy_size, const char* build, const char* content, const std::string& what) {
        const auto before = copy;
        Check(sos_pit_unlock_apply(copy.data(), copy_size, build, content, message, sizeof(message)) == 0 && copy == before,
              name + ": " + what + " is left alone");
    };
    refuses(pristine, size, "CUSA57767-9.99", game.c_str(), "an unknown build");
    refuses(pristine, size - 8, key, game.c_str(), "an image of another size");
    refuses(pristine, size, key, nullptr, "a launch without a game folder");
    refuses(pristine, size, key, (folder / "nothing").string().c_str(), "a game without the Pit content");
    const char* other = std::strcmp(key, "CUSA57767-1.00") ? "CUSA57767-1.00" : "CUSA57767-1.01";
    refuses(pristine, size, other, game.c_str(), "the other build's key");
    for (const int at : {-13, -9, -8, -4, -3, 0, 1, 4, 9, 16, 17, 21, 24, 28, 33, 40, 45, 46, 48, 49}) {
        std::vector<uint8_t> bad = pristine;
        bad[site->gate + at] ^= 0xff;
        refuses(bad, size, key, game.c_str(), "a changed byte at gate" + std::to_string(at));
    }

    std::vector<uint8_t> hermes = pristine;
    Check(sos_hermes_unlock_apply(hermes.data(), size, key, message, sizeof(message)) == 1, name + ": Hermes Mode alone applies");
    const auto hermes_changed = Changed(pristine, hermes);
    Check(std::none_of(hermes_changed.begin(), hermes_changed.end(), [&](uint64_t at) { return changed.count(at) != 0; }),
          name + ": the Hermes Mode and Pit patches touch different bytes");
    Check(sos_pit_unlock_verify(hermes.data(), size, site, &why), name + ": The Pit still verifies with Hermes Mode on");
    Check(sos_hermes_unlock_verify(image.data(), size, sos_hermes_unlock_site(key), &why), name + ": Hermes Mode still verifies with The Pit on");
    std::vector<uint8_t> both = hermes;
    Check(sos_pit_unlock_apply(both.data(), size, key, game.c_str(), message, sizeof(message)) == 1 &&
              Changed(pristine, both).size() == changed.size() + hermes_changed.size(),
          name + ": both options apply together");

    Check(sos_exit_game_verify(image.data(), size, key, &why), name + ": Exit Game still verifies with The Pit on");
    Check(sos_skip_intro_verify(image.data(), size, sos_skip_intro_site(key), &why), name + ": Skip Intro still verifies with The Pit on");
}

// The contract of a build, from its site table and the verified pattern alone: no game file is read.
void TestContract(const char* key, const fs::path& folder) {
    const std::string name = std::string(key) + " contract";
    const SosPitUnlockSite* site = sos_pit_unlock_site(key);
    const SosPitUnlockPattern& pattern = *sos_pit_unlock_pattern();
    Check(site != nullptr, name + ": the build has a site");
    if (!site) return;
    // Where the title menu check was verified in each build's code; a change here is a decision, not a side effect.
    static const struct {
        const char* key;
        uint64_t size;
        uint32_t gate, invoke;
    } verified[] = {{"CUSA57767-1.00", 168044704u, 0x4c22879, 0x90e2640}, {"CUSA57767-1.01", 168046032u, 0x4c290b9, 0x90f9b20}};
    bool pinned = false;
    for (const auto& row : verified) pinned |= !std::strcmp(row.key, key) && row.size == site->image_size && row.gate == site->gate && row.invoke == site->invoke;
    Check(pinned, name + ": the site is where the check was verified");
    const uint64_t start = site->gate - pattern.jump, end = start + pattern.length;
    Check(site->gate >= static_cast<uint32_t>(pattern.jump) && end <= site->image_size && site->invoke < site->image_size,
          name + ": the check and UnityEvent.Invoke lie inside the image");
    Check(pattern.bytes[pattern.jump] == 0x74 && pattern.bytes[pattern.jump + 1] == 0x13, name + ": the patched branch is je +0x13");
    Check(pattern.bytes[pattern.pit_call] == 0xe8 && pattern.bytes[pattern.hermes_call] == 0xe8, name + ": both events are called with a relative call");

    const SosHermesUnlockSite* hermes = sos_hermes_unlock_site(key);
    Check(hermes != nullptr && hermes->image_size == site->image_size, name + ": Hermes Mode and The Pit describe the same image");
    if (hermes) {
        const uint64_t low = hermes->gate - 10, high = hermes->gate + 6;
        Check(end <= low || high <= start, name + ": the Hermes Mode check and the Pit check share no byte");
    }

    SparseImage image(site->image_size);
    if (!image.data) {
        Check(false, name + ": the sparse image is allocated");
        return;
    }
    LayCheck(image, *site);
    const std::string game = MakeGameFolder(folder / (name + " game")).string();
    const char* why = "";
    char message[200] = {};
    Check(sos_pit_unlock_verify(image.data, image.size, site, &why), name + ": the laid check verifies");
    const uint64_t low = start > 64 ? start - 64 : 0, high = end + 64;
    const std::vector<uint8_t> before(image.data + low, image.data + high);
    const auto window = [&] { return std::vector<uint8_t>(image.data + low, image.data + high); };
    Check(ZeroOutside(image, low, high), name + ": nothing but the check was laid");

    const auto refuses = [&](const char* build, uint64_t size, const char* content, const std::string& what) {
        const auto kept = window();
        Check(sos_pit_unlock_apply(image.data, size, build, content, message, sizeof(message)) == 0 && window() == kept, name + ": " + what + " is left alone");
    };
    refuses("CUSA57767-9.99", image.size, game.c_str(), "an unknown build");
    refuses(nullptr, image.size, game.c_str(), "no build");
    refuses(key, image.size - 8, game.c_str(), "an image of another size");
    refuses(key, image.size, nullptr, "a launch without a game folder");
    refuses(key, image.size, (folder / "nothing").string().c_str(), "a game without the Pit content");
    const char* other = std::strcmp(key, "CUSA57767-1.00") ? "CUSA57767-1.00" : "CUSA57767-1.01";
    refuses(other, image.size, game.c_str(), "the other build's key");

    for (int i = 0; i < pattern.length; ++i) {
        image.data[start + i] ^= 0xff;
        refuses(key, image.size, game.c_str(), "a changed byte at check+" + std::to_string(i));
        image.data[start + i] ^= 0xff;
    }
    Check(window() == before, name + ": the refusals left the image as it was");

    Check(sos_pit_unlock_apply(image.data, image.size, key, game.c_str(), message, sizeof(message)) == 1, name + ": the patch applies");
    Check(std::strstr(message, "offered") != nullptr, name + ": the patch reports The Pit as offered");
    const auto patched = window();
    bool only_the_branch = true;
    for (size_t i = 0; i < patched.size(); ++i)
        only_the_branch = only_the_branch && ((patched[i] != before[i]) == (low + i == site->gate || low + i == site->gate + 1));
    Check(only_the_branch, name + ": exactly the two branch bytes change");
    Check(image.data[site->gate] == 0x90 && image.data[site->gate + 1] == 0x90, name + ": the branch becomes NOPs");
    Check(ZeroOutside(image, low, high), name + ": nothing outside the check changed");
    Check(!sos_pit_unlock_verify(image.data, image.size, site, &why), name + ": the patched image is no longer the pristine code");
    Check(sos_pit_unlock_apply(image.data, image.size, key, game.c_str(), message, sizeof(message)) == 0 && window() == patched, name + ": a second apply writes nothing");
    sos_test::ReportContract(key, "the verified check on a sparse image: patch, every changed byte refused, second apply, other build and content refused");
}

}  // namespace

int main() {
    const fs::path root = SOS_SOURCE_DIR;
    Check(sos_pit_unlock_site(nullptr) == nullptr && sos_pit_unlock_site("CUSA57767-9.99") == nullptr, "unknown builds have no site");

    const fs::path folder = fs::temp_directory_path() / ("sos_pit_unlock_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(folder);
    TestSettings(folder);
    TestContent(folder);

    for (const char* key : {"CUSA57767-1.00", "CUSA57767-1.01"}) {
        TestContract(key, folder);
        const sos_test::BootImage boot = sos_test::ReportImage(root, key, "the title menu check");
        if (boot.found()) TestBuild(key, boot.path, sos_test::EvidenceRoot(root) / "game" / "prepared" / key, folder);
        sos_test::ReportContent(root, key, "the Pit scene files");
    }
    fs::remove_all(folder);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
