// SPDX-License-Identifier: GPL-2.0-or-later
// Asks Gamescope to hand the touchscreen to the game as touch input instead of a pointer (see src/sos_touch_gamescope.h).
// The X11 calls go through XApi, loaded from libX11 on Linux and faked by tests/touchscreen_steamos_tests.cpp. Acquire and
// Maintain run on the window thread; Release also runs on whichever thread ends the process, so every call is serialised.
// Nothing here is torn down by a destructor: the process always ends with _Exit, and libX11 is never unloaded.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include "sos_touch_gamescope.h"

#ifdef __linux__
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#endif

namespace SosGamescopeTouch {

enum class Result { Unavailable, Applied };

// The few libX11 calls the claim needs, as libX11 declares them (Display* is opaque here).
struct XApi {
    using Display = void;
    Display* (*open)(const char*) = nullptr;
    int (*close)(Display*) = nullptr;
    unsigned long (*intern)(Display*, const char*, int) = nullptr;
    unsigned long (*default_root)(Display*) = nullptr;
    int (*get)(Display*, unsigned long, unsigned long, long, long, int, unsigned long, unsigned long*, int*,
               unsigned long*, unsigned long*, unsigned char**) = nullptr;
    int (*change)(Display*, unsigned long, unsigned long, unsigned long, int, int, const unsigned char*, int) = nullptr;
    int (*free)(void*) = nullptr;
    int (*flush)(Display*) = nullptr;

    bool Complete() const {
        return open && close && intern && default_root && get && change && free && flush;
    }
};

inline constexpr std::size_t kNameSize = 16;
inline constexpr int kMaxCandidates = 8;

// The local X displays to try: the game's own first, then every other local socket of this user.
struct Candidates {
    char name[kMaxCandidates][kNameSize]{};
    int count = 0;
    int own = -1; ///< index of the game's own display, -1 when DISPLAY is not a local one

    void Add(const char* display) {
        for (int i = 0; i < count; ++i) {
            if (!std::strcmp(name[i], display)) {
                return;
            }
        }
        if (count < kMaxCandidates) {
            std::snprintf(name[count++], kNameSize, "%s", display);
        }
    }
};

// ":N" for a local display name (":N", ":N.S", "unix:N"); anything with a host is left alone.
inline bool LocalDisplayName(const char* display, char* name, std::size_t size) {
    if (!display) {
        return false;
    }
    const char* at = display;
    if (!std::strncmp(at, "unix:", 5)) {
        at += 4;
    }
    if (*at != ':') {
        return false;
    }
    const char* digits = at + 1;
    int count = 0;
    while (count < 3 && digits[count] >= '0' && digits[count] <= '9') {
        ++count;
    }
    if (count == 0 || (digits[count] >= '0' && digits[count] <= '9')) {
        return false;
    }
    const char* rest = digits + count;
    if (*rest == '.') {
        int screen = 0;
        while (rest[1 + screen] >= '0' && rest[1 + screen] <= '9') {
            ++screen;
        }
        if (screen == 0) {
            return false;
        }
        rest += 1 + screen;
    }
    if (*rest) {
        return false;
    }
    std::snprintf(name, size, ":%.*s", count, digits);
    return true;
}

// Gamescope applies the newest STEAM_TOUCH_CLICK_MODE write from any of its Xwayland roots, so Passthrough is written on
// the game's own display and on Gamescope's other roots and rewritten while the game has focus. Each root keeps the value
// it gives back.
class Claim {
public:
    Claim() = default;
    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;

    // Window thread. Opens the roots on first use, claims each and writes Passthrough. `now_ms` starts the rewrite clock.
    Result Acquire(const XApi* api, const Candidates& candidates, std::uint64_t now_ms) {
        Lock lock{busy_};
        if (!api || !api->Complete()) {
            failure_ = SOS_TOUCH_CLAIM_NO_X11;
            return Result::Unavailable;
        }
        api_ = api;
        if (root_count_ == 0 && !OpenRoots(candidates)) {
            failure_ = SOS_TOUCH_CLAIM_NO_DISPLAY;
            return Result::Unavailable;
        }
        checked_ms_ = now_ms;
        claimed_ = true;
        if (!WriteAll()) {
            failure_ = SOS_TOUCH_CLAIM_NO_DISPLAY;
            return Result::Unavailable;
        }
        return Result::Applied;
    }

    // Window thread, while the game window has focus: rewrites Passthrough about once a second. True when written.
    bool Maintain(std::uint64_t now_ms) {
        Lock lock{busy_};
        if (!claimed_ || root_count_ == 0 || now_ms - checked_ms_ < SOS_GAMESCOPE_TOUCH_RECHECK_MS) {
            return false;
        }
        checked_ms_ = now_ms;
        return WriteAll();
    }

    // Focus loss and every exit path, any thread: each root gets its own value back (only while it still reads this
    // game's Passthrough) and its connection is closed. Returns the roots written back. Harmless when nothing is held.
    int Release() {
        Lock lock{busy_};
        int restored = 0;
        for (int i = 0; i < root_count_; ++i) {
            Root& root = roots_[i];
            bool have = false;
            std::uint32_t current = 0, value = 0;
            const bool read = Read(root, have, current);
            if (sos_touch_mode_release(&root.mode, read && have, current, &value) && Write(root, value)) {
                ++restored;
            }
            api_->close(root.display);
            root = Root{};
        }
        root_count_ = 0;
        claimed_ = false;
        return restored;
    }

    int Roots() const {
        return root_count_;
    }
    bool Claimed() const {
        return claimed_;
    }
    bool CompositorRoot() const {
        return compositor_;
    }
    SosTouchClaimFailure Failure() const {
        return failure_;
    }

private:
    static constexpr unsigned long kCardinal = 6; // XA_CARDINAL

    struct Root {
        XApi::Display* display = nullptr;
        unsigned long root = 0;
        unsigned long atom = 0;
        SosTouchModeRoot mode{};
    };

    // A spin lock: trivially destructible, so no static destructor exists to run at exit.
    struct Lock {
        explicit Lock(std::atomic_flag& flag) : flag(flag) {
            while (flag.test_and_set(std::memory_order_acquire)) {
            }
        }
        ~Lock() {
            flag.clear(std::memory_order_release);
        }
        std::atomic_flag& flag;
    };

    bool OpenRoots(const Candidates& candidates) {
        compositor_ = false;
        for (int i = 0; i < candidates.count && root_count_ < SOS_GAMESCOPE_TOUCH_MAX_ROOTS; ++i) {
            const bool mine = i == candidates.own;
            XApi::Display* const display = api_->open(candidates.name[i]);
            if (!display) {
                continue;
            }
            // Another X server is a desktop one unless it is one of Gamescope's roots; only_if_exists = 1.
            if (!mine && !api_->intern(display, SOS_GAMESCOPE_TOUCH_MARKER_ATOM, 1)) {
                api_->close(display);
                continue;
            }
            Root& root = roots_[root_count_++];
            root.display = display;
            root.root = api_->default_root(display);
            root.atom = api_->intern(display, SOS_GAMESCOPE_TOUCH_ATOM, 0);
            compositor_ |= !mine;
        }
        return root_count_ > 0;
    }

    // Passthrough on every root, whatever it reads: a competing write on another root wins Gamescope's single mode.
    bool WriteAll() {
        bool wrote = false;
        for (int i = 0; i < root_count_; ++i) {
            Root& root = roots_[i];
            bool have = false;
            std::uint32_t current = 0;
            if (!Read(root, have, current)) {
                continue;
            }
            sos_touch_mode_claim(&root.mode, have, current);
            wrote |= Write(root, SOS_GAMESCOPE_TOUCH_PASSTHROUGH);
        }
        return wrote;
    }

    bool Read(Root& root, bool& have, std::uint32_t& value) {
        unsigned long type = 0, count = 0, after = 0;
        int format = 0;
        unsigned char* data = nullptr;
        const bool ok = api_->get(root.display, root.root, root.atom, 0, 1, 0, kCardinal, &type, &format, &count, &after,
                                  &data) == 0;
        have = ok && type == kCardinal && format == 32 && count >= 1 && data;
        long word = 0;
        if (have) {
            std::memcpy(&word, data, sizeof(word));
        }
        value = have ? static_cast<std::uint32_t>(word & 0xffffffffL) : 0;
        if (data) {
            api_->free(data);
        }
        return ok;
    }

    bool Write(Root& root, std::uint32_t value) {
        const long data = static_cast<long>(value);
        if (!api_->change(root.display, root.root, root.atom, kCardinal, 32, 0,
                          reinterpret_cast<const unsigned char*>(&data), 1)) {
            return false;
        }
        api_->flush(root.display);
        return true;
    }

    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    const XApi* api_ = nullptr;
    Root roots_[SOS_GAMESCOPE_TOUCH_MAX_ROOTS];
    int root_count_ = 0;
    bool claimed_ = false;
    bool compositor_ = false;
    SosTouchClaimFailure failure_ = SOS_TOUCH_CLAIM_NO_X11;
    std::uint64_t checked_ms_ = 0;
};

#ifdef __linux__
// libX11, loaded once by the window thread and never unloaded (the process ends with _Exit). Null without libX11.
inline const XApi* LoadXApi() {
    static XApi api;
    static bool tried = false;
    if (tried) {
        return api.Complete() ? &api : nullptr;
    }
    tried = true;
    SDL_SharedObject* const library = SDL_LoadObject("libX11.so.6");
    if (!library) {
        return nullptr;
    }
    const auto load = [library](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(SDL_LoadFunction(library, name));
    };
    load(api.open, "XOpenDisplay");
    load(api.close, "XCloseDisplay");
    load(api.intern, "XInternAtom");
    load(api.default_root, "XDefaultRootWindow");
    load(api.get, "XGetWindowProperty");
    load(api.change, "XChangeProperty");
    load(api.free, "XFree");
    load(api.flush, "XFlush");
    return api.Complete() ? &api : nullptr;
}

// The game's own display (DISPLAY) first, then every local X socket of this user; none is contacted unless it answers.
inline Candidates LocalDisplays(const char* own_display) {
    Candidates list;
    char own[kNameSize] = {};
    if (LocalDisplayName(own_display, own, sizeof(own))) {
        list.Add(own);
        list.own = 0;
    }
    DIR* dir = opendir("/tmp/.X11-unix");
    if (!dir) {
        return list;
    }
    const uid_t user = getuid();
    while (const dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (name[0] != 'X') {
            continue;
        }
        int digits = 0;
        while (digits < 3 && name[1 + digits] >= '0' && name[1 + digits] <= '9') {
            ++digits;
        }
        if (digits == 0 || name[1 + digits]) {
            continue;
        }
        char path[64];
        std::snprintf(path, sizeof(path), "/tmp/.X11-unix/%s", name);
        struct stat info{};
        if (lstat(path, &info) != 0 || !S_ISSOCK(info.st_mode) || info.st_uid != user) {
            continue;
        }
        char display[kNameSize];
        std::snprintf(display, sizeof(display), ":%.*s", digits, name + 1);
        list.Add(display);
    }
    closedir(dir);
    return list;
}
#else
inline const XApi* LoadXApi() {
    return nullptr;
}
inline Candidates LocalDisplays(const char*) {
    return {};
}
#endif

} // namespace SosGamescopeTouch
