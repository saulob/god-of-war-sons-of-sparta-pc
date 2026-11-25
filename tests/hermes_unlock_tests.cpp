// Hermes Mode early unlock on the real boot images of CUSA57767 1.00 and 1.01: the site verifies, the patch changes only
// the six bytes of the game-mode selection's hidden-button branch, and an unknown build or a changed image is left alone.
// No game, launcher or window is started.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "build_images.h"
#include "sos_hermes_unlock.h"

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

void TestSettings(const fs::path& folder) {
    const fs::path on = folder / "on.ini";
    const fs::path off = folder / "off.ini";
    const fs::path absent = folder / "absent.ini";
    const fs::path unknown = folder / "unknown.ini";
    WriteFile(on, "skip_intro = 1\nunlock_hermes_mode = 1\n");
    WriteFile(off, "skip_intro = 1\nunlock_hermes_mode = 0\n");
    WriteFile(unknown, "unlock_hermes_mode = maybe\n");
    WriteFile(absent, "skip_intro = 1\n");
    Check(sos_hermes_unlock_from_file(nullptr) == 0, "unlock_hermes_mode defaults to off without a file");
    Check(sos_hermes_unlock_from_file(absent.string().c_str()) == 0, "unlock_hermes_mode defaults to off without the key");
    Check(sos_hermes_unlock_from_file(on.string().c_str()) == 1, "unlock_hermes_mode on is read");
    Check(sos_hermes_unlock_from_file(off.string().c_str()) == 0, "unlock_hermes_mode off is read");
    Check(sos_hermes_unlock_from_file(unknown.string().c_str()) == 0, "an unknown unlock_hermes_mode value keeps the default off");
}

void TestBuild(const char* key, const fs::path& boot, const fs::path& on_settings, const fs::path& off_settings) {
    const std::string name = key;
    std::vector<uint8_t> pristine;
    uint64_t size = 0;
    if (!ReadImage(boot, pristine, size)) {
        Check(false, name + ": the real image is readable");
        return;
    }
    const SosHermesUnlockSite* site = sos_hermes_unlock_site(key);
    const char* why = "";
    Check(site != nullptr, name + ": the build has a site");
    if (!site) return;
    Check(site->image_size == size, name + ": the site's image size is the real image size");
    Check(sos_hermes_unlock_verify(pristine.data(), size, site, &why), name + ": the pristine image is the verified code");

    std::vector<uint8_t> untouched = pristine;
    char settings_message[200] = {};
    if (sos_hermes_unlock_from_file(off_settings.string().c_str()))
        sos_hermes_unlock_apply(untouched.data(), size, key, settings_message, sizeof(settings_message));
    Check(untouched == pristine, name + ": unlock_hermes_mode off leaves the original bytes untouched");

    std::vector<uint8_t> enabled = pristine;
    if (sos_hermes_unlock_from_file(on_settings.string().c_str()))
        sos_hermes_unlock_apply(enabled.data(), size, key, settings_message, sizeof(settings_message));
    Check(enabled != pristine, name + ": unlock_hermes_mode on applies the patch");

    std::vector<uint8_t> image = pristine;
    char message[200] = {};
    Check(sos_hermes_unlock_apply(image.data(), size, key, message, sizeof(message)) == 1, name + ": the patch applies");
    Check(std::strstr(message, "offered") != nullptr, name + ": the patch reports Hermes as offered");
    const auto changed = Changed(pristine, image);
    Check(changed.size() == 6, name + ": exactly six bytes change");
    Check(changed.empty() || (*changed.begin() == site->gate && *changed.rbegin() == site->gate + 5),
          name + ": the changed bytes are the hidden-button branch");
    Check(std::all_of(image.begin() + site->gate, image.begin() + site->gate + 6, [](uint8_t b) { return b == 0x90; }),
          name + ": the hidden-button branch becomes NOPs");
    Check(!sos_hermes_unlock_verify(image.data(), size, site, &why), name + ": the patched image is no longer the pristine code");

    std::vector<uint8_t> again = image;
    Check(sos_hermes_unlock_apply(again.data(), size, key, message, sizeof(message)) == 0 && again == image,
          name + ": a second apply writes nothing");
    std::vector<uint8_t> unknown = pristine;
    Check(sos_hermes_unlock_apply(unknown.data(), size, "CUSA57767-9.99", message, sizeof(message)) == 0 && unknown == pristine,
          name + ": an unknown build is left alone");

    std::vector<uint8_t> short_image = pristine;
    Check(sos_hermes_unlock_apply(short_image.data(), size - 8, key, message, sizeof(message)) == 0 && short_image == pristine,
          name + ": an image of another size is left alone");

    std::vector<uint8_t> bad_gate = pristine;
    bad_gate[site->gate - 10] ^= 0xff;
    const auto before_gate = bad_gate;
    Check(!sos_hermes_unlock_verify(bad_gate.data(), size, site, &why), name + ": a changed comparison fails verification");
    Check(sos_hermes_unlock_apply(bad_gate.data(), size, key, message, sizeof(message)) == 0 && bad_gate == before_gate,
          name + ": a changed comparison is left alone");

    std::vector<uint8_t> bad_target = pristine;
    bad_target[site->gate + 2] ^= 0xff;
    Check(!sos_hermes_unlock_verify(bad_target.data(), size, site, &why), name + ": a changed hidden-branch target fails verification");
}

}  // namespace

int main() {
    const fs::path root = SOS_SOURCE_DIR;
    Check(sos_hermes_unlock_site(nullptr) == nullptr && sos_hermes_unlock_site("CUSA57767-9.99") == nullptr,
          "unknown builds have no site");

    std::ifstream loader(root / "src" / "loader.c");
    const std::string loader_text((std::istreambuf_iterator<char>(loader)), std::istreambuf_iterator<char>());
    Check(loader_text.find("sos_hermes_unlock_apply(image, size, prompt_key") != std::string::npos,
          "the loader applies the patch to the boot image");
    Check(loader_text.find("sos_hermes_unlock_from_file(settings_file)") != std::string::npos,
          "the loader reads unlock_hermes_mode");
    Check(loader_text.find("if (hermes_unlock_wanted)") != std::string::npos, "the loader applies the patch only when it is on");

    const fs::path settings = fs::temp_directory_path() / "sos_hermes_unlock_tests";
    fs::remove_all(settings);
    fs::create_directories(settings);
    TestSettings(settings);
    const fs::path on_settings = settings / "on.ini";
    const fs::path off_settings = settings / "off.ini";

    for (const char* key : {"CUSA57767-1.00", "CUSA57767-1.01"}) {
        const sos_test::BootImage boot = sos_test::ReportImage(root, key, "the Hermes Mode site");
        if (boot.found()) TestBuild(key, boot.path, on_settings, off_settings);
    }
    fs::remove_all(settings);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
