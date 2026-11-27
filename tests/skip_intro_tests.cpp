// Skip Intro: the setting, the build gating, the verified game code of CUSA57767 1.00 and 1.01 (read from the boot images
// when they are installed), the patch set, and the wait for the save system the patched coroutine runs. No game, launcher
// or window is started.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "build_images.h"
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

std::string Read(const std::string& relative) {
    std::ifstream in(std::string(SOS_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

void TestSetting() {
    const fs::path file = fs::temp_directory_path() / "sos_skip_intro_setting.ini";
    const auto write = [&](const std::string& text) {
        std::ofstream(file, std::ios::binary) << text;
    };
    write("skip_intro = 1\n");
    Check(sos_skip_intro_from_file(file.string().c_str()) == 1, "setting: 1 turns it on");
    write("# skip_intro = 0\nskip_intro = on   # comment\n");
    Check(sos_skip_intro_from_file(file.string().c_str()) == 1, "setting: on with a comment turns it on");
    write("skip_intro = 0\n");
    Check(sos_skip_intro_from_file(file.string().c_str()) == 0, "setting: 0 turns it off");
    write("skip_intro = maybe\n");
    Check(sos_skip_intro_from_file(file.string().c_str()) == 0, "setting: an invalid value is off");
    write("frame_rate = 60\n");
    Check(sos_skip_intro_from_file(file.string().c_str()) == 0, "setting: a missing line is off");
    Check(sos_skip_intro_from_file("") == 0 && sos_skip_intro_from_file(nullptr) == 0, "setting: no file is off");
    std::error_code ignored;
    fs::remove(file, ignored);
}

void TestBuildGating() {
    for (const sos::GameBuild& build : sos::SupportedBuilds()) {
        const std::string key = std::string(build.title_id) + "-" + build.version;
        Check(build.skip_intro == (sos_skip_intro_site(key.c_str()) != nullptr),
              key + ": the launcher offers it exactly where a site is verified");
    }
    Check(sos_skip_intro_site("CUSA57767-1.00") && sos_skip_intro_site("CUSA57767-1.01"), "sites: 1.00 and 1.01");
    Check(!sos_skip_intro_site("CUSA57767-9.99") && !sos_skip_intro_site("CUSA00000-1.00") && !sos_skip_intro_site(nullptr),
          "sites: unknown builds have none");
    const char* why = "";
    Check(!sos_skip_intro_verify(nullptr, 0, nullptr, &why) && std::strstr(why, "not a supported build"),
          "verify: an unknown build is refused");
}

int64_t JumpTarget(const std::vector<uint8_t>& image, uint64_t at) {
    int32_t rel;
    std::memcpy(&rel, image.data() + at + 1, sizeof(rel));
    return static_cast<int64_t>(at) + 5 + rel;
}

// Offsets of the `mov edi,<ms> ; xor esi,esi ; call` immediates inside [start, start + length) that call one of the two Task.Delay.
std::vector<uint64_t> DelayImmediates(const std::vector<uint8_t>& image, const SosSkipIntroSite& site, uint64_t start, uint64_t length,
                                      uint32_t milliseconds) {
    std::vector<uint64_t> found;
    for (uint64_t at = start; at + 12 <= start + length; ++at) {
        uint32_t value;
        std::memcpy(&value, image.data() + at + 1, sizeof(value));
        if (image[at] != 0xbf || value != milliseconds || image[at + 5] != 0x31 || image[at + 6] != 0xf6 || image[at + 7] != 0xe8) continue;
        const int64_t target = JumpTarget(image, at + 7);
        if (target == site.task_delay || target == site.task_delay_other) found.push_back(at + 1);
    }
    return found;
}

// The wait block as the patch writes it, run on its bytes (the mov, test, cmp and branches it uses), and the coroutine's
// flow around it, followed through the jumps in the patched image.
enum class Outcome { kEnding, kSaveReady, kNextFrame, kUnknown };

struct SaveSystem {
    uint8_t bytes[0x30] = {};
    void SetInitialized(bool value) { bytes[0x11] = value; }
    void SetGlobal(uint8_t* global) { std::memcpy(bytes + 0x20, &global, sizeof(global)); }
};

Outcome RunWait(const uint8_t* block, const SaveSystem* save, const uint8_t* splash) {
    const uint8_t* rax = save ? save->bytes : nullptr;
    const uint8_t* rcx = nullptr;
    bool zero = false;
    for (size_t pc = 0; pc < 51;) {
        const auto at = [&](size_t i) { return block[pc + i]; };
        if (at(0) == 0x48 && at(1) == 0x85 && (at(2) == 0xc0 || at(2) == 0xc9)) {
            zero = (at(2) == 0xc0 ? rax : rcx) == nullptr;
            pc += 3;
        } else if (at(0) == 0x80 && (at(1) == 0x78 || at(1) == 0x79)) {
            zero = (at(1) == 0x78 ? rax : rcx)[at(2)] == at(3);
            pc += 4;
        } else if (at(0) == 0x41 && at(1) == 0x80 && at(2) == 0xbd) {
            uint32_t displacement;
            std::memcpy(&displacement, block + pc + 3, sizeof(displacement));
            zero = splash[displacement] == at(7);
            pc += 8;
        } else if (at(0) == 0x48 && at(1) == 0x8b && at(2) == 0x48) {
            std::memcpy(&rcx, rax + at(3), sizeof(rcx));
            pc += 4;
        } else if (at(0) == 0x74 || at(0) == 0x75) {
            const bool taken = at(0) == 0x74 ? zero : !zero;
            pc += 2 + (taken ? static_cast<int8_t>(at(1)) : 0);
        } else if (at(0) == 0xe9) {
            return pc == 36 ? Outcome::kEnding : pc == 41 ? Outcome::kSaveReady : pc == 46 ? Outcome::kNextFrame : Outcome::kUnknown;
        } else {
            return Outcome::kUnknown;
        }
    }
    return Outcome::kUnknown;
}

struct Run {
    int frames = 0, ending = 0, booted_mark = 0, scene_loads = 0, lookups = 0;
    bool finished = false;
};

// Runs the patched coroutine one frame at a time; the save system is ready from frame `ready_after` on.
Run Simulate(const std::vector<uint8_t>& image, uint64_t c, int max_frames, int ready_after, bool first_boot) {
    Run run;
    SaveSystem save;
    uint8_t global[0x40] = {};
    uint8_t splash[0xb0] = {};
    int64_t entry = JumpTarget(image, c + 0x1a9);
    for (; run.frames < max_frames && !run.finished; ++run.frames) {
        if (run.frames == ready_after) {
            save.SetInitialized(true);
            save.SetGlobal(global);
            global[0x1c] = 1;
            global[0x14] = first_boot ? 0 : 1;
        }
        for (int steps = 0; steps < 50; ++steps) {
            const int64_t lookup = static_cast<int64_t>(c + 0x1046), ending = static_cast<int64_t>(c + 0xa0e);
            if (entry == lookup) {
                ++run.lookups;
                const SaveSystem* visible = run.frames >= ready_after ? &save : nullptr;
                if (image[c + 0x10aa] != 0xe9 || JumpTarget(image, c + 0x10aa) != static_cast<int64_t>(c + 0x8a6)) return run;
                switch (RunWait(image.data() + c + 0x8a6, visible, splash)) {
                case Outcome::kEnding: entry = ending; break;
                case Outcome::kSaveReady:
                    ++run.booted_mark;
                    ++run.scene_loads;
                    run.finished = true;
                    break;
                case Outcome::kNextFrame:
                    entry = JumpTarget(image, c + 0x561);
                    steps = 50;
                    break;
                default: return run;
                }
                if (run.finished) break;
            } else if (entry == ending) {
                ++run.ending;
                splash[0xa2] = 1;
                entry = lookup;
            } else {
                return run;
            }
        }
    }
    return run;
}

void TestPatchedFlow(const std::vector<uint8_t>& image, uint64_t c, const std::string& name) {
    Check(JumpTarget(image, c + 0x1a9) == static_cast<int64_t>(c + 0x1046),
          name + ": the first state goes to the save system lookup, before the ending");
    Check(image[c + 0x561] == 0xe9 && JumpTarget(image, c + 0x561) == static_cast<int64_t>(c + 0x1046),
          name + ": the yield resumes at the save system lookup, not at the video loop");
    SaveSystem save;
    uint8_t global[0x40] = {};
    uint8_t splash[0xb0] = {};
    const uint8_t* block = image.data() + c + 0x8a6;
    Check(RunWait(block, nullptr, splash) == Outcome::kNextFrame, name + ": no save system yet yields a frame");
    Check(RunWait(block, &save, splash) == Outcome::kNextFrame, name + ": an uninitialised save system yields a frame");
    save.SetInitialized(true);
    Check(RunWait(block, &save, splash) == Outcome::kNextFrame, name + ": no global data yet yields a frame");
    save.SetGlobal(global);
    Check(RunWait(block, &save, splash) == Outcome::kNextFrame, name + ": defaults not initialised yields a frame");
    global[0x1c] = 1;
    Check(RunWait(block, &save, splash) == Outcome::kEnding, name + ": ready enters the ending before it has run");
    splash[0xa2] = 1;
    Check(RunWait(block, &save, splash) == Outcome::kSaveReady, name + ": ready after the ending continues to the booted mark");
    Check(RunWait(block, nullptr, splash) == Outcome::kNextFrame, name + ": the ending's pass still waits when the save system is not ready");

    for (const bool first_boot : {true, false}) {
        const std::string boot = name + (first_boot ? ": first boot, " : ": returning boot, ");
        for (const int ready_after : {0, 1, 5, 929}) {
            const Run run = Simulate(image, c, 2000, ready_after, first_boot);
            const std::string label = boot + "ready after " + std::to_string(ready_after) + " frames: ";
            Check(run.finished && run.ending == 1 && run.booted_mark == 1 && run.scene_loads == 1,
                  label + "the ending, the booted mark and the scene load run once each");
            Check(run.frames - 1 == ready_after, label + "it proceeds in the frame the save system is ready");
            Check(run.lookups <= ready_after + 3, label + "only the lookup repeats while waiting");
        }
        const Run never = Simulate(image, c, 300, 100000, first_boot);
        Check(!never.finished && never.ending == 0 && never.booted_mark == 0 && never.lookups == 300,
              boot + "while the save system is not ready the ending is never reached");
    }
}

bool RealImage(const fs::path& boot, std::vector<uint8_t>& image, uint64_t& size) {
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
    image.assign(size + SOS_SKIP_INTRO_STUB_BYTES, 0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(size)));
}

void TestRealImage(const char* key, const fs::path& boot) {
    const std::string name = key;
    std::vector<uint8_t> image;
    uint64_t size = 0;
    Check(RealImage(boot, image, size), name + ": the real image is readable");
    const SosSkipIntroSite* site = sos_skip_intro_site(key);
    const char* why = "";
    Check(site && sos_skip_intro_verify(image.data(), size, site, &why), name + ": the real code is the verified code");
    if (!site) return;
    Check(site->image_size == size, name + ": the verified image size is the real one");
    Check(!sos_skip_intro_verify(image.data(), size - 8, site, &why), name + ": another image size is refused");

    const uint64_t c = site->coroutine;
    const uint8_t cancel_head[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8b, 0x77, 0x58, 0x48, 0x85, 0xf6, 0x74};
    const auto cancel_at = std::search(image.begin(), image.end(), std::begin(cancel_head), std::end(cancel_head));
    Check(cancel_at != image.end(), name + ": the title animation watcher's cancel coroutine is in the image");
    const uint64_t cancel_site =
        cancel_at != image.end() ? static_cast<uint64_t>(std::distance(image.begin(), cancel_at)) : 0;
    struct Mutation {
        uint64_t at;
        bool tolerated;
        const char* what;
    };
    const Mutation mutations[] = {
        {c + 16, false, "the coroutine's frame size"},
        {c + 20, true, "a displacement in the coroutine"},
        {c + 0x162 + 27, false, "the state offset of the dispatch"},
        {c + 0x19d, false, "the first state's null check"},
        {c + 0x1a9, false, "the first state's branch"},
        {c + 0x82f - 3, false, "the video loop's isPlaying test"},
        {c + 0x82f + 2, false, "the video loop's exit target"},
        {c + 0x898 + 3, false, "the skipFinished offset"},
        {c + 0x8a6, false, "the user skip's first instruction"},
        {c + 0x8a0 + 2, false, "the user skip's end"},
        {c + 0xa0e, false, "the ending's first instruction"},
        {c + 0x9b2 + 24, false, "the next frame yield's state"},
        {c + 0x10aa, false, "the save check's null test"},
        {c + 0x10aa + 6, false, "the save check's isInitialized offset"},
        {c + 0x10aa + 4, false, "the save check's null target"},
        {c + 0x555 + 4, false, "the resume case's state store"},
        {c + 0x561 + 2, false, "the resume case's video loop target"},
        {c + 0x1046, false, "the save system lookup"},
        {site->init_save_system + 14, false, "the save system's frame size"},
        {site->init_save_system + 0x1e9, false, "the startup delay's class check"},
        {site->init_save_system + 0x1f8, false, "the startup delay's 10000"},
        {site->init_save_system + 0x1f8 + 1, false, "the startup delay's 10000, second byte"},
        {site->init_save_system + 0x1e9 + 22, false, "the startup delay's call target"},
        {site->init_save_system + 0x1e9 + 14, false, "the startup delay's mov"},
        {c + 0xf87 + 4, false, "the ending's finished flag"},
        {site->load_save_task + 14, false, "LoadSaveTask's frame size"},
        {site->load_save_task + 20, true, "a displacement in LoadSaveTask's prologue"},
        {site->check_file_exist + 16, false, "CheckFileExist's frame size"},
        {site->title_check, false, "the title check's cmp"},
        {site->title_check + 3, false, "the title check's flag offset"},
        {site->title_check - 4, false, "the call before the title check"},
        {site->title_check + 7, false, "the title check's branch target"},
        {cancel_site + 3, false, "the title animation watcher's cancel coroutine"},
    };
    for (const Mutation& mutation : mutations) {
        const uint8_t original = image[mutation.at];
        image[mutation.at] = static_cast<uint8_t>(original ^ 0x5a);
        const bool accepted = sos_skip_intro_verify(image.data(), size, site, &why);
        image[mutation.at] = original;
        Check(accepted == mutation.tolerated, name + ": " + mutation.what + (mutation.tolerated ? " is tolerated" : " is refused"));
    }
    Check(sos_skip_intro_verify(image.data(), size, site, &why), name + ": the image is restored");

    const auto load_polls = DelayImmediates(image, *site, site->load_save_task, 0x3430, 100);
    const auto exist_polls = DelayImmediates(image, *site, site->check_file_exist, 0x3610, 100);
    const auto exist_retries = DelayImmediates(image, *site, site->check_file_exist, 0x3610, 500);
    Check(load_polls.size() == 25 && exist_polls.size() == 12 && exist_retries.size() == 2,
          name + ": LoadSaveTask awaits Task.Delay(100) 25 times, CheckFileExist 12 times, and Task.Delay(500) twice");
    Check(DelayImmediates(image, *site, 0, size, 100).size() >= 37, name + ": the sites are among the image's own Task.Delay waits");
    const std::vector<uint64_t> sampled = {load_polls.front(), load_polls[12], load_polls.back(), exist_polls.front(), exist_polls.back(),
                                           exist_retries.front(), exist_retries.back()};
    for (const uint64_t at : sampled) {
        image[at] ^= 0x01;
        Check(!sos_skip_intro_verify(image.data(), size, site, &why) && std::strstr(why, "polling waits"),
              name + ": a changed wait at +0x" + std::to_string(at) + " is refused");
        image[at] ^= 0x01;
        image[at + 7] ^= 0x10;
        Check(!sos_skip_intro_verify(image.data(), size, site, &why) && std::strstr(why, "polling waits"),
              name + ": a wait that no longer calls Task.Delay at +0x" + std::to_string(at) + " is refused");
        image[at + 7] ^= 0x10;
    }
    Check(sos_skip_intro_verify(image.data(), size, site, &why), name + ": the image is restored after the waits");

    const std::vector<uint8_t> pristine = image;
    char message[200];
    Check(!sos_skip_intro_apply(image.data(), size, "CUSA57767-9.99", image.data() + size, SOS_SKIP_INTRO_STUB_BYTES, message, sizeof(message)) && image == pristine,
          name + ": an unknown build writes nothing");
    Check(!sos_skip_intro_apply(image.data(), size, key, image.data() + size, SOS_SKIP_INTRO_STUB_BYTES - 1, message, sizeof(message)) &&
              image == pristine,
          name + ": a stub area that is too small writes nothing");
    Check(!sos_skip_intro_apply(image.data(), size - 8, key, image.data() + size, SOS_SKIP_INTRO_STUB_BYTES, message, sizeof(message)) && image == pristine,
          name + ": another image size writes nothing");
    for (const uint64_t at : std::vector<uint64_t>{c + 0x19d, c + 0x8a6, c + 0xa0e, c + 0x10aa + 6, c + 0x555, c + 0x1046,
                                                  site->init_save_system + 0x1f8, site->init_save_system + 0x1e9 + 22, load_polls[3],
                                                  exist_polls[5], exist_retries[1]}) {
        image[at] ^= 0x5a;
        const std::vector<uint8_t> broken = image;
        Check(!sos_skip_intro_apply(image.data(), size, key, image.data() + size, SOS_SKIP_INTRO_STUB_BYTES, message, sizeof(message)) && image == broken,
              name + ": a mismatch at +0x" + std::to_string(at - c) + " leaves every byte as it was");
        image[at] ^= 0x5a;
    }
    Check(image == pristine, name + ": the original bytes are back");

    Check(sos_skip_intro_apply(image.data(), size, key, image.data() + size, SOS_SKIP_INTRO_STUB_BYTES, message, sizeof(message)) && std::strstr(message, "bypassed"),
          name + ": the patch applies");
    std::vector<uint64_t> changed;
    for (uint64_t i = 0; i < size; ++i)
        if (image[i] != pristine[i]) changed.push_back(i);
    const uint64_t delay = site->init_save_system + 0x1f8;
    std::vector<uint64_t> expected = {delay, delay + 1};
    for (const auto* list : {&load_polls, &exist_polls}) expected.insert(expected.end(), list->begin(), list->end());
    const uint64_t title = site->title_check;
    const auto in_title = [&](uint64_t at) { return at >= title && at < title + 5; };
    const auto in_coroutine = [&](uint64_t at) {
        return (at >= c + 0x1a9 && at < c + 0x1af) || (at >= c + 0x561 && at < c + 0x567) || (at >= c + 0x8a6 && at < c + 0x8a6 + 51) ||
               (at >= c + 0x10aa && at < c + 0x10b5);
    };
    Check(image[title] == 0xe8 && JumpTarget(image, title) >= static_cast<int64_t>(size) &&
              JumpTarget(image, title) + SOS_SKIP_INTRO_STUB_BYTES <= static_cast<int64_t>(image.size()) &&
              std::memcmp(image.data() + title + 5, pristine.data() + title + 5, 6) == 0,
          name + ": the title check is a call to the stub, its je is untouched");
    Check(!changed.empty() && changed.front() == delay && std::find(changed.begin(), changed.end(), c + 0x10aa + 10) != changed.end(),
          name + ": the patch lies in the startup delay, the save waits, the coroutine's first state, resume, user skip and save check");
    Check(std::all_of(changed.begin(), changed.end(), [&](uint64_t at) {
              return in_title(at) || in_coroutine(at) || std::find(expected.begin(), expected.end(), at) != expected.end();
          }), name + ": nothing outside the delay, the 37 save waits, the title check and the five coroutine points changed");
    Check(std::all_of(expected.begin(), expected.end(), [&](uint64_t at) { return std::find(changed.begin(), changed.end(), at) != changed.end(); }),
          name + ": the startup delay and every save wait changed");
    Check(std::all_of(load_polls.begin(), load_polls.end(), [&](uint64_t at) {
              return image[at] == 1 && image[at + 1] == 0 && image[at + 2] == 0 && image[at + 3] == 0 && pristine[at] == 100 && image[at - 1] == 0xbf &&
                     image[at + 4] == 0x31 && image[at + 5] == 0xf6 && image[at + 6] == 0xe8 && std::memcmp(image.data() + at + 7, pristine.data() + at + 7, 4) == 0;
          }), name + ": each LoadSaveTask wait is mov edi,1 in front of the same Task.Delay call");
    Check(std::all_of(exist_polls.begin(), exist_polls.end(), [&](uint64_t at) { return image[at] == 1 && pristine[at] == 100; }),
          name + ": each CheckFileExist wait is 1 ms");
    Check(DelayImmediates(image, *site, site->check_file_exist, 0x3610, 500) == exist_retries &&
              DelayImmediates(image, *site, site->load_save_task, 0x3430, 100).empty() &&
              DelayImmediates(image, *site, site->check_file_exist, 0x3610, 100).empty(),
          name + ": the two 500 ms retries stay and no 100 ms wait is left in either function");
    Check(pristine[delay] == 0x10 && pristine[delay + 1] == 0x27 && pristine[delay + 2] == 0 && pristine[delay + 3] == 0 &&
              pristine[delay - 1] == 0xbf && pristine[delay + 4] == 0x31 && pristine[delay + 5] == 0xf6 && pristine[delay + 6] == 0xe8,
          name + ": the original is mov edi,10000 ; xor esi,esi in front of the Task.Delay call");
    Check(image[delay - 1] == 0xbf && image[delay] == 1 && image[delay + 1] == 0 && image[delay + 2] == 0 && image[delay + 3] == 0 &&
              image[delay + 4] == 0x31 && image[delay + 5] == 0xf6 && image[delay + 6] == 0xe8,
          name + ": the patched delay is mov edi,1 and nothing else in the await changed");
    Check(image[c + 0x10aa] == 0xe9 && JumpTarget(image, c + 0x10aa) == static_cast<int64_t>(c + 0x8a6),
          name + ": the save check goes to the wait for the save system");
    Check(image[c + 0x8a6 + 36] == 0xe9 && JumpTarget(image, c + 0x8a6 + 36) == static_cast<int64_t>(c + 0xa0e) &&
              JumpTarget(image, c + 0x8a6 + 41) == static_cast<int64_t>(c + 0x10b5) &&
              JumpTarget(image, c + 0x8a6 + 46) == static_cast<int64_t>(c + 0x9b2),
          name + ": the wait ends in the ending, the original check's continuation or the next frame yield");
    TestPatchedFlow(image, c, name);
    Check(!sos_skip_intro_verify(image.data(), size, site, &why) && !sos_skip_intro_apply(image.data(), size, key, image.data() + size, SOS_SKIP_INTRO_STUB_BYTES, message, sizeof(message)),
          name + ": a patched image is refused, so the patch is never applied twice");
}

#ifdef _WIN32
// The title stub, run for real on a function shaped like the intro's TickState around the check.
void TestTitleStub() {
    using Tick = int(__attribute__((sysv_abi)) *)(void*);
    const uint8_t function[] = {0x41, 0x56, 0x49, 0x89, 0xfe, 0x41, 0x80, 0x7e, 0x6c, 0x00, 0x74, 0x07, 0xb8, 0x01, 0x00,
                                0x00, 0x00, 0xeb, 0x02, 0x31, 0xc0, 0x41, 0x5e, 0xc3};
    auto* block = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    Check(block != nullptr, "title stub: executable memory");
    if (!block) return;
    uint8_t* code = block;
    uint8_t* stub = block + 0x100;
    std::memcpy(code, function, sizeof(function));
    alignas(16) uint8_t object[0x90] = {};
    const Tick tick = reinterpret_cast<Tick>(code);

    object[0x6c] = 1;
    Check(tick(object) == 1, "title stub: the original test sees a queued switch");
    object[0x6c] = 0;
    Check(tick(object) == 0, "title stub: the original test sees none");
    const std::vector<uint8_t> before(code, code + sizeof(function));
    Check(!sos_skip_intro_hook_title(code + 6, stub) && std::equal(before.begin(), before.end(), code), "title stub: another check is refused");
    Check(!sos_skip_intro_hook_title(code + 5, reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(code) + (1ull << 33))) &&
              std::equal(before.begin(), before.end(), code),
          "title stub: a stub out of reach writes nothing");
    Check(sos_skip_intro_hook_title(code + 5, stub) && code[5] == 0xe8, "title stub: the check becomes a call");

    alignas(16) uint8_t menu[0x140] = {};
    const auto link = [&](bool open, bool processing) {
        const uint8_t* menu_pointer = menu;
        std::memcpy(object + 0x48, &menu_pointer, sizeof(menu_pointer));
        const int32_t state = open ? 0 : 2;
        std::memcpy(menu + 0x88, &state, sizeof(state));
        menu[0x130] = processing ? 1 : 0;
    };
    const auto arm = [&] {
        object[0x6c] = 0;
        sos_skip_intro_arm();
    };
    const auto settle = [&] {
        for (unsigned i = 1; i < SOS_SKIP_INTRO_MIN_TICKS; ++i) tick(object);
    };

    link(true, false);
    for (unsigned i = 0; i < SOS_SKIP_INTRO_MIN_TICKS; ++i) tick(object);
    object[0x6c] = 0;
    Check(tick(object) == 0 && object[0x6c] == 0, "title stub: not armed, the original test runs");
    object[0x6c] = 1;
    Check(tick(object) == 1 && object[0x6c] == 1, "title stub: not armed, a queued switch still counts");

    arm();
    Check(tick(object) == 0 && tick(object) == 0 && object[0x6c] == 0, "title stub: armed, the first ticks of the intro do not switch");
    Check(tick(object) == 1 && object[0x6c] == 1, "title stub: armed, ready: the switch is queued and taken");
    object[0x6c] = 0;
    Check(tick(object) == 0 && object[0x6c] == 0, "title stub: the next test runs the original");
    Check(tick(object) == 0 && tick(object) == 0, "title stub: it never claims again until armed");

    arm();
    link(false, false);
    settle();
    Check(tick(object) == 0 && tick(object) == 0 && object[0x6c] == 0, "title stub: a closed main menu waits");
    link(true, true);
    Check(tick(object) == 0 && object[0x6c] == 0, "title stub: the main menu processing the load button waits");
    std::memset(object + 0x48, 0, sizeof(uint8_t*));
    Check(tick(object) == 0 && object[0x6c] == 0, "title stub: no main menu waits");
    link(true, false);
    Check(tick(object) == 1 && object[0x6c] == 1, "title stub: the claim is granted as soon as the menu is ready");
    object[0x6c] = 0;
    Check(tick(object) == 0, "title stub: and only once");

    arm();
    link(true, false);
    uint64_t kept[4] = {0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull, 0x4444444444444444ull};
    settle();
    uint64_t after[4] = {};
    asm volatile(
        "mov %[a], %%rdi\n\t"
        "mov %[b], %%rsi\n\t"
        "mov %[c], %%rcx\n\t"
        "mov %[d], %%rdx\n\t"
        "mov %[obj], %%r14\n\t"
        "push %%rbp\n\t"
        "mov %%rsp, %%rbp\n\t"
        "and $-16, %%rsp\n\t"
        "call *%[stub]\n\t"
        "mov %%rbp, %%rsp\n\t"
        "pop %%rbp\n\t"
        "mov %%rdi, %[oa]\n\t"
        "mov %%rsi, %[ob]\n\t"
        "mov %%rcx, %[oc]\n\t"
        "mov %%rdx, %[od]"
        : [oa] "=m"(after[0]), [ob] "=m"(after[1]), [oc] "=m"(after[2]), [od] "=m"(after[3])
        : [a] "m"(kept[0]), [b] "m"(kept[1]), [c] "m"(kept[2]), [d] "m"(kept[3]), [obj] "r"(object), [stub] "r"(stub)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r14", "r8", "r9", "r10", "r11", "memory", "cc");
    Check(object[0x6c] == 1 && std::equal(kept, kept + 4, after), "title stub: rdi, rsi, rcx and rdx come back as they went in");
    VirtualFree(block, 0, MEM_RELEASE);
}
#else
void TestTitleStub() { std::printf("  [skip] the title stub is only run on Windows\n"); }
#endif

std::vector<void*> g_title_cancelled;

void SOS_SKIP_INTRO_SYSV OnTitleAnimationCancelled(void* component) { g_title_cancelled.push_back(component); }

void TestTitleGuard() {
    alignas(16) uint8_t intro[0x20] = {};
    alignas(16) uint8_t dead[0x20] = {};
    void* native = intro;
    std::memcpy(intro + 0x10, &native, sizeof(native));
    g_title_cancelled.clear();
    sos_skip_intro_arm_title(0x10, OnTitleAnimationCancelled);
    sos_skip_intro_arm();
    Check(sos_skip_intro_watching_title() == 1, "title guard: armed while the skipped title is on screen");
    sos_skip_intro_watch_title_animation(intro);
    sos_skip_intro_watch_title_animation(intro);
    sos_skip_intro_watch_title_animation(dead);
    sos_skip_intro_dismiss_title();
    Check(g_title_cancelled.size() == 1 && g_title_cancelled[0] == intro,
          "title guard: dismissing the prompt cancels each pending title animation once, and skips destroyed objects");
    Check(sos_skip_intro_watching_title() == 0, "title guard: it stops watching after the prompt is dismissed");
    sos_skip_intro_dismiss_title();
    Check(g_title_cancelled.size() == 1, "title guard: a second dismissal cancels nothing");
    sos_skip_intro_watch_title_animation(intro);
    Check(g_title_cancelled.size() == 1, "title guard: no animation is recorded after the dismissal");

    g_title_cancelled.clear();
    sos_skip_intro_arm_title(0x10, nullptr);
    sos_skip_intro_arm();
    sos_skip_intro_watch_title_animation(intro);
    sos_skip_intro_dismiss_title();
    Check(g_title_cancelled.empty() && sos_skip_intro_watching_title() == 0,
          "title guard: without a cancel method it still stops watching and fails safe");
}

void TestRealImages() {
    const fs::path root = SOS_SOURCE_DIR;
    for (const char* key : {"CUSA57767-1.00", "CUSA57767-1.01"}) {
        const sos_test::BootImage boot = sos_test::ReportImage(root, key, "the Skip Intro sites");
        if (boot.found()) TestRealImage(key, boot.path);
    }
}

void TestWiring() {
    const std::string loader = Read("src/loader.c");
    Check(Has(loader, "sos_skip_intro_from_file(settings_file) && sos_skip_intro_supported(paths.image_dir)"),
          "loader: the setting and the build gate the patch");
    Check(Has(loader, "if (skip_intro_wanted) {"), "loader: with the setting off nothing is applied, the 10 s delay stays");
    Check(Has(loader, "sos_skip_intro_apply(image, size, skip_key, image + round_page(size), stub_area, skip_message, sizeof(skip_message));"),
          "loader: the patch is applied once, on the verified key");
    const auto apply_at = loader.find("sos_skip_intro_apply(");
    const auto redzone_at = loader.find("runtime_redzone_install(");
    const auto protect_at = loader.find("protect(image, round_page(size), 0);");
    Check(redzone_at != std::string::npos && apply_at > redzone_at && apply_at < protect_at,
          "loader: the patch goes in after the red zone analysis, before the image is protected");
    Check(Has(loader, "image = allocate(round_page(size) + stub_area + compat_area);") &&
              Has(loader, "protect(image + round_page(size), stub_area, 5);") &&
              Has(loader, "if (compat_area) protect(image + round_page(size) + stub_area, compat_area, 5);"),
          "loader: the title stub's page follows the image and ends up read and execute only");
    Check(Has(loader, "image + round_page(size), stub_area, skip_message"), "loader: the stub goes in the page after the image");
    Check(!Has(loader, "sos_trace") && !Has(Read("CMakeLists.txt"), "sos_trace") && !fs::exists(fs::path(SOS_SOURCE_DIR) / "src/sos_trace.c"),
          "no startup trace hooks remain");
    Check(!Has(Read("src/runtime_file.c"), "DEBUG_SAULO"), "no DEBUG_SAULO diagnostics remain");

    Check(!Has(Read("src/runtime_pad.c"), "skip_press") && !Has(Read("src/runtime_pad.c"), "skip_cross"),
          "pad: no input is injected for Skip Intro");
    Check(!Has(Read("gpu/shim/dcp_startup_watch.cpp"), "skip_intro") && !Has(Read("gpu/shim/dcp_startup_watch.h"), "skip_intro"),
          "video core: the startup watch has no Skip Intro hook");
    for (const char* file : {"avplayer.cpp", "avplayer_source.cpp", "avplayer_source.h", "avplayer_state.cpp", "avplayer_state.h"}) {
        const std::string text = Read(std::string("gpu/shadps4/core/libraries/avplayer/") + file);
        Check(!Has(text, "skip_intro") && !Has(text, "SkipIntro") && !Has(text, "m_skip"), std::string("player: ") + file + " is untouched by Skip Intro");
    }
    const std::string gpu = Read("gpu/shim/dcpgpu.cpp");
    Check(!Has(gpu, "SkipIntro::") && !Has(gpu, "avplayer_skip.h"), "video core: no demuxer cut is configured");
    for (const char* gone : {"gpu/shadps4/core/libraries/avplayer/avplayer_skip.h", "src/guest_hook.c", "src/guest_hook.h",
                             "src/skip_press.h"}) {
        Check(!fs::exists(fs::path(SOS_SOURCE_DIR) / gone), std::string("the old mechanism is gone: ") + gone);
    }
    const std::string cmake = Read("CMakeLists.txt");
    Check(Has(cmake, "src/sos_skip_intro.c") && !Has(cmake, "guest_hook"), "the loader builds the patch and no hook code");
    const std::string source = Read("src/sos_skip_intro.c");
    Check(!Has(source, "TrySkipping") && !Has(source, "CanSkip") && !Has(source, "Cross"), "no TrySkipping, CanSkip or Cross handling remains");
}

} // namespace

int main() {
    TestSetting();
    TestBuildGating();
    TestTitleStub();
    TestTitleGuard();
    TestRealImages();
    TestWiring();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
