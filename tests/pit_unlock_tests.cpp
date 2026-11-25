// The Pit early unlock on the real boot images of CUSA57767 1.00 and 1.01: the site verifies, the patch changes only the
// two bytes of the title menu's branch, it stays independent of Hermes Mode, Skip Intro and Exit Game, and an unknown
// build, a changed image or a game without the Pit scenes is left alone. No game, launcher or window is started.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

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

}  // namespace

int main() {
    const fs::path root = SOS_SOURCE_DIR;
    Check(sos_pit_unlock_site(nullptr) == nullptr && sos_pit_unlock_site("CUSA57767-9.99") == nullptr, "unknown builds have no site");

    const fs::path folder = fs::temp_directory_path() / ("sos_pit_unlock_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(folder);
    TestSettings(folder);
    TestContent(folder);

    const struct {
        const char* key;
        std::vector<const char*> folders;
    } builds[] = {{"CUSA57767-1.00", {"CUSA57767-1.00", "v1.00"}}, {"CUSA57767-1.01", {"CUSA57767-1.01", "v1.01"}}};
    bool any = false;
    for (const auto& build : builds) {
        fs::path boot;
        for (const char* name : build.folders) {
            const fs::path candidate = root / "build" / "image" / name / "boot.bin";
            if (boot.empty() && fs::is_regular_file(candidate)) boot = candidate;
        }
        if (boot.empty()) {
            std::printf("  [skip] %s: no boot image\n", build.key);
            continue;
        }
        any = true;
        TestBuild(build.key, boot, root / "game" / "prepared" / build.key, folder);
    }
    fs::remove_all(folder);
    if (!any) std::printf("  [skip] no installed boot image: the real code was not read\n");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
