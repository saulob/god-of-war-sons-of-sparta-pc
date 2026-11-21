// Pure unit and source-wiring tests for user/gamecontrollerdb.txt and unmapped joystick diagnostics.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "sos_pad_mapping.h"

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

#define SOS_STR2(x) #x
#define SOS_STR(x) SOS_STR2(x)

std::string ReadSource(const char* relative) {
    std::ifstream file(std::string(SOS_STR(SOS_SOURCE_DIR)) + "/" + relative, std::ios::binary);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

std::size_t Pos(const std::string& text, const std::string& needle) {
    return text.find(needle);
}

int Count(const std::string& text, const std::string& needle) {
    int n = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) {
        ++n;
    }
    return n;
}

std::string DbPath(const char* settings, std::size_t size = 4300) {
    std::string buf(size, 'x');
    const int ok = sos_gamepad_db_path(settings, buf.data(), size);
    return ok ? std::string(buf.c_str()) : std::string("<none>");
}

void TestGamepadDbPath() {
    Check(DbPath("user/sons_of_sparta.ini") == "user/gamecontrollerdb.txt", "POSIX user/ relative path");
    Check(DbPath("C:\\Games\\Sons\\user\\sons_of_sparta.ini") == "C:\\Games\\Sons\\user\\gamecontrollerdb.txt",
          "Windows backslash user path");
    Check(DbPath("C:/Games/Sons/user/sons_of_sparta.ini") == "C:/Games/Sons/user/gamecontrollerdb.txt",
          "Windows forward-slash user path");
    Check(DbPath("C:\\Games/Sons\\user/sons_of_sparta.ini") == "C:\\Games/Sons\\user/gamecontrollerdb.txt",
          "mixed separators preserve directory prefix");
    Check(DbPath("sons_of_sparta.ini") == "gamecontrollerdb.txt", "bare filename resolves in current directory");
    Check(DbPath("user/") == "user/gamecontrollerdb.txt", "trailing slash directory");
    Check(DbPath("/home/player/My Games/user/sons_of_sparta.ini") == "/home/player/My Games/user/gamecontrollerdb.txt",
          "path with spaces");
    Check(DbPath(nullptr) == "<none>", "null settings path rejected");
    Check(DbPath("") == "<none>", "empty settings path rejected");
    Check(sos_gamepad_db_path("user/sons_of_sparta.ini", nullptr, 64) == 0, "null output buffer rejected");

    const std::string expected = "user/gamecontrollerdb.txt";
    for (std::size_t size = 0; size <= expected.size() + 2; ++size) {
        std::string guarded(expected.size() + 16, '#');
        const int ok = sos_gamepad_db_path("user/sons_of_sparta.ini", guarded.data(), size);
        Check(ok == (size >= expected.size() + 1), "buffer boundary check");
        bool untouched = true;
        for (std::size_t i = size; i < guarded.size(); ++i) {
            if (guarded[i] != '#') untouched = false;
        }
        Check(untouched, "no write past output buffer size");
        if (!ok && size > 0) {
            Check(guarded[0] == 0, "failed path call clears output buffer");
        }
    }
    const std::string huge(5000, 'a');
    Check(DbPath((huge + "/sons_of_sparta.ini").c_str()) == "<none>", "oversized path rejected");
}

void TestLoadLineFormatting() {
    char line[384];
    sos_gamepad_db_load_line(0, "C:\\Users\\Secret\\user\\gamecontrollerdb.txt", "", line, sizeof(line));
    Check(std::string(line) == "Runtime: loaded 0 user controller mappings from user/gamecontrollerdb.txt",
          "zero mappings load line is concise and hides local path");

    sos_gamepad_db_load_line(1, "C:\\Users\\Secret\\user\\gamecontrollerdb.txt", "", line, sizeof(line));
    Check(std::string(line) == "Runtime: loaded 1 user controller mapping from user/gamecontrollerdb.txt",
          "singular mapping load line");

    sos_gamepad_db_load_line(5, "C:\\Users\\Secret\\user\\gamecontrollerdb.txt", "", line, sizeof(line));
    Check(std::string(line) == "Runtime: loaded 5 user controller mappings from user/gamecontrollerdb.txt",
          "plural mappings load line");

    const char* secret_path = "C:\\Users\\Secret\\user\\gamecontrollerdb.txt";
    const char* sdl_err = "Couldn't open C:\\Users\\Secret\\user\\gamecontrollerdb.txt: Permission\ndenied";
    sos_gamepad_db_load_line(-1, secret_path, sdl_err, line, sizeof(line));
    const std::string err_line(line);
    Check(err_line.find("Secret") == std::string::npos, "error log never exposes local username/path");
    Check(err_line.find("user/gamecontrollerdb.txt") != std::string::npos, "error log uses logical path");
    Check(err_line.find("Permission denied") != std::string::npos, "error log keeps sanitized bounded reason");
    Check(err_line.find('\n') == std::string::npos, "error log strips control characters");

    sos_gamepad_db_load_line(-1, secret_path, "", line, sizeof(line));
    Check(std::string(line).find("read error") != std::string::npos, "empty error falls back cleanly");
}

std::string UnmappedLine(const char* name, const char* guid, unsigned vendor, unsigned product) {
    char line[384];
    sos_unmapped_joystick_line(name, guid, vendor, product, line, sizeof(line));
    return line;
}

void TestUnmappedJoystickLine() {
    const char* guid = "030000005e040000d102000001000000";
    const std::string full = UnmappedLine("Generic Pad", guid, 0x045e, 0x02d1);
    Check(full.find("Unmapped joystick: Generic Pad, GUID=030000005e040000d102000001000000, VID:PID=045e:02d1") !=
              std::string::npos,
          "unmapped diagnostic includes name, GUID and VID:PID");
    Check(full.find("user/gamecontrollerdb.txt") != std::string::npos, "unmapped diagnostic points to logical file");

    Check(UnmappedLine("Pad", guid, 0x1, 0xabc).find("VID:PID=0001:0abc") != std::string::npos,
          "VID:PID is zero-padded 4-digit hex");
    Check(UnmappedLine("Pad", guid, 0, 0x10).find("VID:PID=0000:0010") != std::string::npos,
          "non-zero product keeps VID:PID formatting");

    const std::string no_ids = UnmappedLine("Virtual Stick", guid, 0, 0);
    Check(no_ids.find("VID:PID=unavailable") != std::string::npos, "missing VID:PID reported as unavailable");
    Check(no_ids.find("0000:0000") == std::string::npos, "missing VID:PID does not fabricate 0000:0000");

    for (const char* empty_name : {static_cast<const char*>(nullptr), ""}) {
        const std::string unnamed = UnmappedLine(empty_name, guid, 0x1234, 0x5678);
        Check(unnamed.find("Unmapped joystick: GUID=030000005e040000d102000001000000, VID:PID=1234:5678") !=
                  std::string::npos,
              "missing device name formatted cleanly without double spaces");
    }

    const std::string ctrl = UnmappedLine("Pad\tWith\nBad\rChars", guid, 1, 2);
    Check(ctrl.find_first_of("\t\n\r") == std::string::npos && ctrl.find("Pad With Bad Chars") != std::string::npos,
          "control characters in device name are sanitized");

    const std::string huge = UnmappedLine(std::string(2000, 'x').c_str(), guid, 1, 2);
    Check(huge.size() < 384 && huge.find(guid) != std::string::npos, "oversized device name stays bounded and keeps GUID");

    char tiny[24];
    sos_unmapped_joystick_line("Pad", guid, 1, 2, tiny, sizeof(tiny));
    Check(std::strlen(tiny) < sizeof(tiny), "tiny output buffer stays null-terminated");
}

void TestUnmappedSeenLifetime() {
    SosUnmappedSeen seen{};
    Check(sos_unmapped_first_sight(&seen, 11) == 1, "first sight logs device");
    Check(sos_unmapped_first_sight(&seen, 11) == 0, "second sight suppresses duplicate");
    Check(sos_unmapped_first_sight(&seen, 12) == 1, "different device logs once");
    Check(seen.count == 2, "two devices tracked");

    const std::uint32_t both[] = {11, 12};
    sos_unmapped_forget_absent(&seen, both, 2);
    Check(seen.count == 2 && sos_unmapped_first_sight(&seen, 11) == 0, "present devices stay suppressed");

    const std::uint32_t only12[] = {12};
    sos_unmapped_forget_absent(&seen, only12, 1);
    Check(seen.count == 1 && sos_unmapped_first_sight(&seen, 12) == 0, "remaining device stays suppressed");
    Check(sos_unmapped_first_sight(&seen, 11) == 1, "removed and reconnected device logs again");

    sos_unmapped_forget_absent(&seen, nullptr, 0);
    Check(seen.count == 0, "all absent clears seen list");

    unsigned logged = 0;
    for (std::uint32_t id = 1; id <= 50000; ++id) {
        const std::uint32_t present[] = {id};
        sos_unmapped_forget_absent(&seen, present, 1);
        logged += static_cast<unsigned>(sos_unmapped_first_sight(&seen, id));
        if (seen.count > SOS_UNMAPPED_SEEN) {
            Check(false, "seen list exceeded fixed bound during churn");
            break;
        }
    }
    Check(logged == 50000 && seen.count == 1, "50000 connect/disconnect cycles stay strictly bounded");

    std::uint32_t crowd[SOS_UNMAPPED_SEEN * 2];
    for (std::size_t i = 0; i < SOS_UNMAPPED_SEEN * 2; ++i) crowd[i] = 100 + static_cast<std::uint32_t>(i);
    bool bounded = true;
    for (const std::uint32_t id : crowd) {
        sos_unmapped_forget_absent(&seen, crowd, SOS_UNMAPPED_SEEN * 2);
        if (sos_unmapped_first_sight(&seen, id) != 1 || seen.count > SOS_UNMAPPED_SEEN) bounded = false;
    }
    Check(bounded, "simultaneous crowd stays within fixed capacity");
}

void TestRuntimeWiringInvariants() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    const std::string loader = ReadSource("src/loader.c");
    const std::string header = ReadSource("src/sos_pad_mapping.h");
    Check(!pad.empty() && !loader.empty() && !header.empty(), "production sources readable");
    Check(header.find("SDL_") == std::string::npos, "sos_pad_mapping.h stays pure without SDL dependency");

    const std::size_t init_pos = Pos(pad, "SDL_InitSubSystem(SDL_INIT_GAMEPAD)");
    const std::size_t load_pos = Pos(pad, "if (sdl_ready>0) load_user_gamepad_mappings();");
    const std::size_t unmapped_pos = Pos(pad, "log_unmapped_joysticks();");
    const std::size_t enum_pos = Pos(pad, "SDL_GetGamepads(&count)");
    Check(Pos(pad, "sos_prepare_sdl_gamepad();") < init_pos && init_pos < load_pos && load_pos < unmapped_pos &&
              unmapped_pos < enum_pos,
          "runtime_pad initializes SDL gamepad, loads user DB, logs unmapped, then enumerates gamepads");

    Check(Count(pad, "load_user_gamepad_mappings();") == 1, "user database loads once per initialization");
    Check(Count(pad, "SDL_AddGamepadMappingsFromFile(") == 1, "SDL_AddGamepadMappingsFromFile called once");
    Check(Pos(pad, "!SDL_GetPathInfo(path,&info)) return;") < Pos(pad, "SDL_AddGamepadMappingsFromFile("),
          "missing database returns silently before reading");
    Check(Pos(pad, "if (SDL_IsGamepad(ids[j]) || !sos_unmapped_first_sight(&seen,ids[j])) continue;") !=
              std::string::npos,
          "mapped gamepads and already-seen unmapped joysticks are skipped");

    for (const char* forbidden : {"SDL_SetGamepadMapping", "SDL_ReloadGamepadMappings", "SDL_OpenJoystick",
                                  "SDL_GetJoystickSerial", "SDL_GetJoystickPath", "SDL_GetGamepadSerial",
                                  "SDL_GetGamepadPath"}) {
        Check(pad.find(forbidden) == std::string::npos, std::string("runtime_pad.c avoids forbidden API ") + forbidden);
    }

    Check(Pos(loader, "%s/sons_of_sparta.ini") < Pos(loader, "runtime_pad_set_settings(settings_file);") &&
              Pos(loader, "runtime_pad_set_settings(settings_file);") < Pos(loader, "runtime_start("),
          "loader passes canonical user settings path to runtime_pad before runtime startup");
}

} // namespace

int main() {
    TestGamepadDbPath();
    TestLoadLineFormatting();
    TestUnmappedJoystickLine();
    TestUnmappedSeenLifetime();
    TestRuntimeWiringInvariants();
    std::printf("Controller database unit checks: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
