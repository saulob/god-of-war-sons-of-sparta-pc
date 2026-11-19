// libSceAvPlayer through its import table, as the game calls it: handles, callbacks, the /app0 path mapping, worker
// shutdown and, when AVPLAYER_TEST_MEDIA names a small generated clip, demux, decode, seek and end of file.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <malloc.h>
#include <windows.h>
#include <tlhelp32.h>

#include "core/libraries/avplayer/avplayer.h"
#include "core/libraries/avplayer/avplayer_error.h"
#include "core/libraries/avplayer/avplayer_skip.h"
#include "core/libraries/libs.h"
#include "core/memory.h"

using namespace Libraries::AvPlayer;
using namespace std::chrono_literals;

static std::map<std::string, u64> g_symbols;
static int g_symbol_count;
static std::atomic<int> g_attached_workers;
static std::string g_media_dir;
static std::mutex g_paths_mutex;
static std::vector<std::string> g_translated;

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char*, const char*, SymbolType, u64 address) {
    g_symbols[nid] = address;
    ++g_symbol_count;
}
} // namespace Core::Loader

void Core::MemoryManager::InvalidateMemory(VAddr, u64) {}

extern "C" FILE* sos_stderr_stream(void) {
    return __acrt_iob_func(2);
}

extern "C" void runtime_thread_attach_host(const char* name) {
    if (name && std::strcmp(name, "AvPlayer") == 0) {
        ++g_attached_workers;
    }
}

extern "C" int runtime_file_translate(const char* guest, char* out, size_t size) {
    {
        std::lock_guard lock(g_paths_mutex);
        g_translated.emplace_back(guest);
    }
    static const char prefix[] = "/app0/";
    if (std::strncmp(guest, prefix, sizeof(prefix) - 1) != 0 || g_media_dir.empty()) {
        return -1;
    }
    std::snprintf(out, size, "%s\\%s", g_media_dir.c_str(), guest + sizeof(prefix) - 1);
    return 0;
}

static int failures;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok  " : "fail", what);
    if (!ok) {
        ++failures;
    }
}

// AvPlayer's workers name themselves "shadPS4:Av*"; the OS adds unrelated pool threads of its own.
static int thread_count() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{sizeof(entry)};
    int count = 0;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != GetCurrentProcessId()) {
                continue;
            }
            const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            PWSTR name = nullptr;
            if (thread && SUCCEEDED(GetThreadDescription(thread, &name)) && name && std::wcsncmp(name, L"shadPS4:Av", 10) == 0) {
                ++count;
            }
            if (name) {
                LocalFree(name);
            }
            if (thread) {
                CloseHandle(thread);
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

static bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds limit = 10s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

static bool threads_settle(int baseline) {
    const bool settled = wait_until([&] { return thread_count() <= baseline; }, 5s);
    if (!settled) {
        std::printf("    threads: %d, baseline %d\n", thread_count(), baseline);
    }
    return settled;
}

// Entry points, resolved by NID like the loader does.
template <class F>
static F api(const char* nid) {
    const auto it = g_symbols.find(nid);
    return it == g_symbols.end() ? nullptr : reinterpret_cast<F>(it->second);
}

using InitFn = AvPlayerHandle PS4_SYSV_ABI (*)(AvPlayerInitData*);
using InitExFn = s32 PS4_SYSV_ABI (*)(const AvPlayerInitDataEx*, AvPlayerHandle*);
using HandleFn = s32 PS4_SYSV_ABI (*)(AvPlayerHandle);
using HandleBoolFn = bool PS4_SYSV_ABI (*)(AvPlayerHandle);
using TimeFn = u64 PS4_SYSV_ABI (*)(AvPlayerHandle);
using SourceFn = s32 PS4_SYSV_ABI (*)(AvPlayerHandle, const char*);
using SourceExFn = s32 PS4_SYSV_ABI (*)(AvPlayerHandle, AvPlayerUriType, AvPlayerSourceDetails*);
using StreamFn = s32 PS4_SYSV_ABI (*)(AvPlayerHandle, u32);
using StreamInfoFn = s32 PS4_SYSV_ABI (*)(AvPlayerHandle, u32, AvPlayerStreamInfo*);
using VideoFn = bool PS4_SYSV_ABI (*)(AvPlayerHandle, AvPlayerFrameInfo*);
using VideoExFn = bool PS4_SYSV_ABI (*)(AvPlayerHandle, AvPlayerFrameInfoEx*);
using LoopFn = s32 PS4_SYSV_ABI (*)(AvPlayerHandle, bool);
using JumpFn = s32 PS4_SYSV_ABI (*)(AvPlayerHandle, u64);
using PostInitFn = s32 PS4_SYSV_ABI (*)(AvPlayerHandle, AvPlayerPostInitData*);
using SyncFn = s32 PS4_SYSV_ABI (*)(AvPlayerHandle, AvPlayerAvSyncMode);
using LogFn = s32 PS4_SYSV_ABI (*)(AvPlayerLogCallback, void*);

struct Api {
    InitFn init = api<InitFn>("aS66RI0gGgo");
    InitExFn init_ex = api<InitExFn>("o9eWRkSL+M4");
    PostInitFn post_init = api<PostInitFn>("HD1YKVU26-M");
    HandleFn close = api<HandleFn>("NkJwDzKmIlw");
    LogFn set_log = api<LogFn>("eBTreZ84JFY");
    SourceFn add_source = api<SourceFn>("KMcEa+rHsIo");
    SourceExFn add_source_ex = api<SourceExFn>("x8uvuFOPZhU");
    HandleFn start = api<HandleFn>("ET4Gr-Uu07s");
    HandleFn stop = api<HandleFn>("ZC17w3vB5Lo");
    HandleFn pause = api<HandleFn>("9y5v+fGN4Wk");
    HandleFn resume = api<HandleFn>("w5moABNwnRY");
    HandleBoolFn is_active = api<HandleBoolFn>("UbQoYawOsfY");
    TimeFn current_time = api<TimeFn>("wwM99gjFf1Y");
    HandleFn stream_count = api<HandleFn>("hdTyRzCXQeQ");
    StreamInfoFn stream_info = api<StreamInfoFn>("d8FcbzfAdQw");
    StreamFn enable_stream = api<StreamFn>("ODJK2sn9w4A");
    VideoFn video = api<VideoFn>("o3+RWnHViSg");
    VideoExFn video_ex = api<VideoExFn>("JdksQu8pNdQ");
    VideoFn audio = api<VideoFn>("Wnp1OVcrZgk");
    JumpFn jump = api<JumpFn>("XC9wM+xULz8");
    LoopFn looping = api<LoopFn>("OVths0xGfho");
    SyncFn sync_mode = api<SyncFn>("k-q+xOxdc3E");
};

// The game's allocator and file callbacks run guest code; here they are plain host functions.
static std::atomic<int> g_allocations;
static void* PS4_SYSV_ABI Alloc(void*, u32 align, u32 size) {
    ++g_allocations;
    return _aligned_malloc(size, align);
}
static void PS4_SYSV_ABI Free(void*, void* memory) {
    _aligned_free(memory);
}

// The game's request wait: an expected event or warning code, a counter and a semaphore. The callback signals a
// matching code; a signal that arrives before the wait is kept in the counter.
struct GameWait {
    std::atomic<s32> expected{0};
    std::atomic<s32> counter{0};
    std::mutex mutex;
    std::condition_variable cv;
    int semaphore = 0;

    void arm(s32 code) {
        expected = code;
    }
    void signal(s32 code) {
        if (expected != code) {
            return;
        }
        expected = -1;
        if (counter.fetch_add(1) < 0) {
            std::lock_guard lock(mutex);
            ++semaphore;
            cv.notify_one();
        }
    }
    bool wait(std::chrono::milliseconds limit) {
        if (counter.fetch_sub(1) <= 0) {
            std::unique_lock lock(mutex);
            if (!cv.wait_for(lock, limit, [this] { return semaphore > 0; })) {
                counter.fetch_add(1);
                return false;
            }
            --semaphore;
        }
        expected = 0;
        return true;
    }
};

struct Player {
    Api* api = nullptr;
    AvPlayerHandle handle = nullptr;
    std::atomic<int> ready{0}, play{0}, stop{0}, pause{0}, warnings{0}, events{0};
    std::atomic<int> reentrant_errors{0};
    std::atomic<bool> close_in_ready{false}, start_in_ready{true}, touch_every_event{false};
    std::atomic<s32> close_result{-1};
    std::atomic<s32> jump_warning{0};
    std::atomic<int> jump_count{0};
    GameWait wait;
    std::atomic<int> streams{-1};
    std::vector<AvPlayerStreamInfo> infos;
    std::mutex mutex;
};

static void PS4_SYSV_ABI OnEvent(void* opaque, AvPlayerEvents event, s32, void* data) {
    auto* p = static_cast<Player*>(opaque);
    ++p->events;
    p->wait.signal(event == AvPlayerEvents::WarningId && data ? *static_cast<s32*>(data) : s32(event));
    if (p->touch_every_event) {
        AvPlayerStreamInfo info{};
        if (p->api->stream_info(p->handle, 0, &info) != ORBIS_OK) {
            ++p->reentrant_errors;
        }
    }
    switch (event) {
    case AvPlayerEvents::StateReady: {
        if (p->close_in_ready) {
            p->close_result = p->api->close(p->handle);
            ++p->ready;
            return;
        }
        const s32 count = p->api->stream_count(p->handle);
        std::lock_guard lock(p->mutex);
        p->infos.clear();
        for (s32 i = 0; i < count; ++i) {
            AvPlayerStreamInfo info{};
            if (p->api->stream_info(p->handle, u32(i), &info) == ORBIS_OK) {
                p->infos.push_back(info);
                p->api->enable_stream(p->handle, u32(i));
            }
        }
        p->streams = count;
        if (p->start_in_ready) {
            p->api->start(p->handle);
        }
        ++p->ready;
        break;
    }
    case AvPlayerEvents::StatePlay:
        ++p->play;
        break;
    case AvPlayerEvents::StateStop:
        ++p->stop;
        break;
    case AvPlayerEvents::StatePause:
        ++p->pause;
        break;
    case AvPlayerEvents::WarningId:
        ++p->warnings;
        if (data && *static_cast<s32*>(data) == ORBIS_AVPLAYER_ERROR_WAR_JUMP_COMPLETE) {
            p->jump_warning = ORBIS_AVPLAYER_ERROR_WAR_JUMP_COMPLETE;
            ++p->jump_count;
        }
        break;
    default:
        break;
    }
}

static AvPlayerInitDataEx make_init(Player* p, bool event_callback = true) {
    AvPlayerInitDataEx data{};
    data.this_size = sizeof(data);
    data.memory_replacement = {p, &Alloc, &Free, &Alloc, &Free};
    data.event_replacement = {p, event_callback ? &OnEvent : nullptr};
    data.num_output_video_framebuffers = 6;
    data.auto_start = false;
    return data;
}

static AvPlayerHandle open_player(Api& a, Player& p) {
    p.api = &a;
    auto init = make_init(&p);
    AvPlayerHandle handle = nullptr;
    if (a.init_ex(&init, &handle) != ORBIS_OK) {
        return nullptr;
    }
    p.handle = handle;
    return handle;
}

static bool add_clip(Api& a, Player& p, const char* path = "/app0/tiny.mp4") {
    AvPlayerSourceDetails details{};
    details.uri = {path, u32(std::strlen(path))};
    details.source_type = AvPlayerSourceType::FileMp4;
    return a.add_source_ex(p.handle, AvPlayerUriType::Source, &details) == ORBIS_OK;
}

static void test_abi() {
    std::puts("structure layouts the game fills");
    check(sizeof(AvPlayerSourceDetails) == 0x80 && offsetof(AvPlayerSourceDetails, source_type) == 0x50,
          "source details: uri at 0, source type at 0x50");
    check(offsetof(AvPlayerInitDataEx, event_replacement) == 0x58 && offsetof(AvPlayerInitDataEx, default_language) == 0x68 &&
              offsetof(AvPlayerInitDataEx, debug_level) == 0x70 && offsetof(AvPlayerInitDataEx, num_output_video_framebuffers) == 0xa4 &&
              offsetof(AvPlayerInitDataEx, auto_start) == 0xa8,
          "init data: callbacks, language, debug level, buffer count and auto start offsets");
    check(offsetof(AvPlayerInitDataEx, file_replacement) == 0x30 && offsetof(AvPlayerInitDataEx, memory_replacement) == 0x08,
          "init data: allocator at 8, file replacement at 0x30");
    check(sizeof(AvPlayerStreamInfo) == 40 && offsetof(AvPlayerStreamInfo, duration) == 24 && offsetof(AvPlayerStreamInfo, start_time) == 32,
          "stream info is 40 bytes");
    check(sizeof(AvPlayerFrameInfoEx) == 104 && offsetof(AvPlayerFrameInfoEx, timestamp) == 16 && offsetof(AvPlayerFrameInfoEx, details) == 24,
          "extended frame info is 104 bytes");
    check(sizeof(AvPlayerFrameInfo) == 40 && offsetof(AvPlayerFrameInfo, details) == 24, "frame info is 40 bytes");
    check(offsetof(AvPlayerVideoEx, pitch) == 36 && offsetof(AvPlayerVideoEx, crop_bottom_offset) == 32,
          "extended video details: crop and pitch");
}

static void test_registry() {
    std::puts("import table");
    static const char* const imports[] = {"KMcEa+rHsIo", "x8uvuFOPZhU", "buMCiJftcfw", "NkJwDzKmIlw", "wwM99gjFf1Y", "ODJK2sn9w4A",
                                          "Wnp1OVcrZgk", "d8FcbzfAdQw", "JdksQu8pNdQ", "aS66RI0gGgo", "o9eWRkSL+M4", "UbQoYawOsfY",
                                          "XC9wM+xULz8", "9y5v+fGN4Wk", "HD1YKVU26-M", "w5moABNwnRY", "k-q+xOxdc3E", "eBTreZ84JFY",
                                          "OVths0xGfho", "av8Z++94rs0", "ET4Gr-Uu07s", "ZC17w3vB5Lo", "hdTyRzCXQeQ"};
    bool all = true;
    for (const char* nid : imports) {
        all = all && g_symbols.contains(nid) && g_symbols[nid] != 0;
    }
    check(all, "the 23 imports of the game are registered");
    check(size_t(g_symbol_count) == g_symbols.size(), "no NID is registered twice");
}

static void test_invalid_handles(Api& a) {
    std::puts("invalid handles");
    AvPlayerHandle bogus = reinterpret_cast<AvPlayerHandle>(uintptr_t(0x1234560));
    AvPlayerHandle none = nullptr;
    int wrong = 0;
    auto expect = [&](s32 result) { wrong += result != ORBIS_AVPLAYER_ERROR_INVALID_PARAMS; };
    AvPlayerStreamInfo info{};
    AvPlayerFrameInfo frame{};
    AvPlayerFrameInfoEx frame_ex{};
    AvPlayerPostInitData post{};
    for (AvPlayerHandle h : {none, bogus}) {
        expect(a.close(h));
        expect(a.add_source(h, "/app0/x.mp4"));
        expect(s32(a.current_time(h)));
        expect(a.start(h));
        expect(a.stop(h));
        expect(a.pause(h));
        expect(a.resume(h));
        expect(a.stream_count(h));
        expect(a.stream_info(h, 0, &info));
        expect(a.enable_stream(h, 0));
        expect(a.looping(h, true));
        expect(a.post_init(h, &post));
        expect(a.sync_mode(h, AvPlayerAvSyncMode::Default));
        wrong += a.is_active(h) || a.video(h, &frame) || a.video_ex(h, &frame_ex) || a.audio(h, &frame);
    }
    check(wrong == 0, "every call refuses a null or unknown handle");
    check(a.jump(none, 0) == ORBIS_AVPLAYER_ERROR_INVALID_PARAMS, "JumpToTime refuses a null handle");
    check(a.jump(bogus, 500) == ORBIS_OK, "JumpToTime accepts anything else, as the other ports do");

    Player p;
    AvPlayerHandle handle = open_player(a, p);
    check(handle != nullptr, "InitEx returns a handle");
    check(a.close(handle) == ORBIS_OK, "the first Close succeeds");
    check(a.close(handle) == ORBIS_AVPLAYER_ERROR_INVALID_PARAMS, "a second Close is an error");
    check(a.start(handle) == ORBIS_AVPLAYER_ERROR_INVALID_PARAMS && a.stream_count(handle) == ORBIS_AVPLAYER_ERROR_INVALID_PARAMS,
          "a closed handle is refused");
}

static void test_init(Api& a) {
    std::puts("Init and InitEx");
    check(a.init(nullptr) == nullptr, "Init without data returns no handle");
    AvPlayerInitData data{};
    check(a.init(&data) == nullptr, "Init without allocators returns no handle");
    Player p;
    data.memory_replacement = {&p, &Alloc, &Free, &Alloc, &Free};
    data.num_output_video_framebuffers = 4;
    AvPlayerHandle handle = a.init(&data);
    check(handle != nullptr, "Init returns a handle");
    check(a.close(handle) == ORBIS_OK, "the handle closes");
    AvPlayerHandle out = nullptr;
    check(a.init_ex(nullptr, &out) == ORBIS_AVPLAYER_ERROR_INVALID_PARAMS, "InitEx without data is refused");
    auto ex = make_init(&p);
    check(a.init_ex(&ex, nullptr) == ORBIS_AVPLAYER_ERROR_INVALID_PARAMS, "InitEx without a handle pointer is refused");
    ex.memory_replacement.allocate = nullptr;
    check(a.init_ex(&ex, &out) == ORBIS_AVPLAYER_ERROR_INVALID_PARAMS && out == nullptr, "InitEx without allocators is refused");
    p.api = &a;
    handle = open_player(a, p);
    AvPlayerPostInitData post{};
    check(a.post_init(handle, &post) == ORBIS_OK, "PostInit applies");
    check(a.sync_mode(handle, AvPlayerAvSyncMode::None) == ORBIS_OK, "SetAvSyncMode applies");
    check(a.start(handle) == ORBIS_AVPLAYER_ERROR_OPERATION_FAILED, "Start without a source fails");
    check(a.stop(handle) == ORBIS_AVPLAYER_ERROR_OPERATION_FAILED, "Stop before Start fails");
    check(a.stream_count(handle) == ORBIS_AVPLAYER_ERROR_OPERATION_FAILED, "no streams without a source");
    check(!a.is_active(handle) && a.current_time(handle) == 0, "an empty player is idle");
    check(a.close(handle) == ORBIS_OK, "an unused player closes");
}

static void test_log_callback(Api& a) {
    std::puts("SetLogCallback");
    AvPlayerLogCallback callback = nullptr;
    void* user = nullptr;
    GetLogCallback(callback, user);
    check(callback == nullptr && user == nullptr, "no callback before the first call");
    auto* fake = reinterpret_cast<AvPlayerLogCallback>(uintptr_t(0x1000));
    check(a.set_log(fake, reinterpret_cast<void*>(uintptr_t(0x2000))) == ORBIS_OK, "SetLogCallback succeeds");
    GetLogCallback(callback, user);
    check(callback == fake && user == reinterpret_cast<void*>(uintptr_t(0x2000)), "the callback and its user data are kept");
    check(a.set_log(nullptr, nullptr) == ORBIS_OK, "a null callback is accepted");
    GetLogCallback(callback, user);
    check(callback == nullptr && user == nullptr, "a null callback clears it");
}

static bool wait_ready(Player& p) {
    return wait_until([&] { return p.ready > 0; });
}

// Drains video and audio the way the game's player loop does until `frames` video frames arrived.
static int pump(Api& a, Player& p, int frames, u64* first_timestamp = nullptr, std::chrono::milliseconds limit = 15s) {
    int got = 0;
    const auto end = std::chrono::steady_clock::now() + limit;
    while (got < frames && std::chrono::steady_clock::now() < end) {
        AvPlayerFrameInfo audio{};
        a.audio(p.handle, &audio);
        AvPlayerFrameInfoEx frame{};
        if (a.video_ex(p.handle, &frame)) {
            if (got == 0 && first_timestamp) {
                *first_timestamp = frame.timestamp;
            }
            ++got;
        } else {
            std::this_thread::sleep_for(2ms);
        }
    }
    return got;
}

static void test_media(Api& a, int baseline) {
    std::puts("media: open through /app0, demux, decode");
    {
        Player p;
        open_player(a, p);
        check(!add_clip(a, p, "/elsewhere/tiny.mp4"), "a path outside /app0 does not open");
        check(a.close(p.handle) == ORBIS_OK, "the failed player closes");
    }
    {
        std::lock_guard lock(g_paths_mutex);
        check(!g_translated.empty() && g_translated.back() == "/elsewhere/tiny.mp4",
              "the guest path reaches the runtime mapper unchanged");
    }

    Player p;
    open_player(a, p);
    check(add_clip(a, p), "AddSourceEx opens /app0/tiny.mp4");
    check(!add_clip(a, p), "a second source is refused");
    {
        std::lock_guard lock(g_paths_mutex);
        check(g_translated.back() == "/app0/tiny.mp4", "the /app0 path is mapped by the runtime");
    }
    check(wait_ready(p), "the Ready event arrives");
    check(p.streams == 2, "two streams are found");
    {
        std::lock_guard lock(p.mutex);
        bool video = false, audio = false;
        for (const auto& info : p.infos) {
            if (info.type == AvPlayerStreamType::Video) {
                video = info.details.video.width == 128 && info.details.video.height == 96 && info.duration > 1000;
            } else if (info.type == AvPlayerStreamType::Audio) {
                audio = info.details.audio.sample_rate == 48000 && info.details.audio.channel_count >= 1;
            }
        }
        check(video, "the video stream reports 128x96 and its duration");
        check(audio, "the audio stream reports 48000 Hz");
    }
    check(wait_until([&] { return p.play > 0; }), "Start from the Ready callback reaches Play");
    check(g_attached_workers > 0, "the worker threads attached a guest thread record");
    check(a.is_active(p.handle), "the player is active");

    AvPlayerFrameInfoEx frame{};
    bool got = false;
    for (int i = 0; i < 6000 && !got; ++i) {
        AvPlayerFrameInfo audio{};
        a.audio(p.handle, &audio);
        got = a.video_ex(p.handle, &frame);
        if (!got) {
            std::this_thread::sleep_for(2ms);
        }
    }
    check(got, "GetVideoDataEx delivers a frame");
    check(frame.p_data != nullptr && frame.details.video.width == 128 && frame.details.video.height == 96 &&
              frame.details.video.pitch == 128 && frame.details.video.luma_bit_depth == 8,
          "the frame is 128x96 NV12 with its pitch and bit depth");
    check(frame.timestamp < 500, "the first frame is at the start of the clip");
    {
        const auto* luma = static_cast<const u8*>(frame.p_data);
        const auto* chroma = luma + 128 * 96;
        bool luma_varies = false, chroma_varies = false;
        for (int i = 1; i < 128 * 96; ++i) {
            luma_varies |= luma[i] != luma[0];
        }
        for (int i = 1; i < 128 * 48; ++i) {
            chroma_varies |= chroma[i] != chroma[0];
        }
        check(luma_varies && chroma_varies, "the returned NV12 buffer holds picture data, not a flat black frame");
    }
    u64 first = 0;
    check(pump(a, p, 5, &first) == 5, "more frames follow");
    AvPlayerFrameInfo legacy{};
    bool legacy_ok = false;
    for (int i = 0; i < 3000 && !legacy_ok; ++i) {
        AvPlayerFrameInfo audio{};
        a.audio(p.handle, &audio);
        legacy_ok = a.video(p.handle, &legacy);
        std::this_thread::sleep_for(2ms);
    }
    check(legacy_ok && legacy.p_data != nullptr && legacy.details.video.width == 128, "GetVideoData delivers a frame");

    AvPlayerFrameInfo audio{};
    bool audio_ok = false;
    for (int i = 0; i < 3000 && !audio_ok; ++i) {
        audio_ok = a.audio(p.handle, &audio);
        std::this_thread::sleep_for(2ms);
    }
    check(audio_ok && audio.p_data != nullptr && audio.details.audio.sample_rate == 48000 && audio.details.audio.size > 0,
          "GetAudioData delivers PCM");

    const u64 before = a.current_time(p.handle);
    pump(a, p, 4);
    check(a.current_time(p.handle) > before, "CurrentTime follows the media timeline");

    std::puts("media: pause, resume, jump, stop, restart");
    check(a.pause(p.handle) == ORBIS_OK, "Pause succeeds");
    check(wait_until([&] { return p.pause > 0; }, 3s), "the Pause event arrives");
    const u64 paused_at = a.current_time(p.handle);
    std::this_thread::sleep_for(150ms);
    AvPlayerFrameInfoEx none{};
    check(!a.video_ex(p.handle, &none) && a.current_time(p.handle) == paused_at, "no frames and no time while paused");
    check(a.resume(p.handle) == ORBIS_OK, "Resume succeeds");
    check(pump(a, p, 2) == 2, "frames follow a Resume");

    {
        const int events = p.events, allocations = g_allocations;
        const u64 time_before = a.current_time(p.handle);
        check(a.jump(p.handle, 0) == ORBIS_OK && a.jump(p.handle, 1000) == ORBIS_OK, "JumpToTime while playing succeeds");
        std::this_thread::sleep_for(150ms);
        check(wait_until([&] { return p.jump_count == 2; }, 3s), "each JumpToTime sends one jump-complete warning");
        std::this_thread::sleep_for(100ms);
        check(p.events == events + 2 && p.jump_count == 2, "the jump-complete warnings are the only events JumpToTime causes");
        check(g_allocations == allocations, "JumpToTime allocates no buffers");
        check(a.current_time(p.handle) >= time_before && a.is_active(p.handle), "JumpToTime leaves the playback running");
        check(pump(a, p, 2) == 2, "frames follow JumpToTime");

        check(a.pause(p.handle) == ORBIS_OK, "Pause succeeds before a jump");
        const int paused_events = p.events;
        const int plays = p.play;
        check(a.jump(p.handle, 500) == ORBIS_OK, "JumpToTime while paused succeeds");
        check(p.jump_count == 3 && p.events == paused_events + 2 && p.play == plays + 1,
              "JumpToTime while paused sends one warning and resumes the player");
        check(g_allocations == allocations, "JumpToTime while paused allocates nothing");
        check(pump(a, p, 2) == 2, "frames follow JumpToTime while paused");
        check(a.resume(p.handle) == ORBIS_OK, "the game's Resume after the jump succeeds");
        check(a.resume(p.handle) == ORBIS_AVPLAYER_ERROR_OPERATION_FAILED, "a further Resume on a playing player is still refused");
        check(a.pause(p.handle) == ORBIS_OK && a.resume(p.handle) == ORBIS_OK && pump(a, p, 2) == 2,
              "an ordinary Pause and Resume still work");
    }

    check(a.stop(p.handle) == ORBIS_OK, "Stop succeeds");
    check(a.stop(p.handle) == ORBIS_AVPLAYER_ERROR_OPERATION_FAILED, "a second Stop fails");
    check(a.start(p.handle) == ORBIS_OK, "Start after Stop restarts the pipeline");
    check(pump(a, p, 3, &first) == 3 && first < 500, "the restarted player begins at the start of the clip");

    std::puts("media: end of file");
    const int stops = p.stop;
    int delivered = 3;
    const auto end = std::chrono::steady_clock::now() + 20s;
    while (a.is_active(p.handle) && std::chrono::steady_clock::now() < end) {
        AvPlayerFrameInfo drained{};
        AvPlayerFrameInfoEx drained_video{};
        a.audio(p.handle, &drained);
        delivered += a.video_ex(p.handle, &drained_video);
        std::this_thread::sleep_for(2ms);
    }
    check(!a.is_active(p.handle), "the player goes inactive at the end of the clip");
    check(delivered == 30, "all 30 frames of the clip are delivered, including the ones the decoder holds back");
    check(wait_until([&] { return p.stop > stops; }, 5s), "the Stop event arrives after the last frame");
    check(a.pause(p.handle) == ORBIS_OK, "Pause at the end of the file succeeds");
    check(a.resume(p.handle) == ORBIS_OK, "Resume at the end of the file succeeds");
    check(a.stop(p.handle) == ORBIS_OK, "Stop at the end of the file succeeds");
    std::this_thread::sleep_for(100ms);
    check(p.stop == stops + 1, "Stop after the end of the file sends no second Stop event");
    check(a.stop(p.handle) == ORBIS_AVPLAYER_ERROR_OPERATION_FAILED, "a repeated Stop fails without side effects");
    check(a.start(p.handle) == ORBIS_OK, "Start after the end of the file restarts the pipeline");
    check(pump(a, p, 3, &first) == 3 && first < 500, "the player plays again from the start after the end of the file");
    check(a.close(p.handle) == ORBIS_OK, "Close after the end of the file succeeds");
    check(threads_settle(baseline), "no worker thread survives Close");
}

// Frames stay available after the end of the file is reached, and nothing stops the player until they are consumed.
static void test_eof_lifecycle(Api& a, int baseline) {
    std::puts("media: end of file with queued frames");
    Player p;
    open_player(a, p);
    check(add_clip(a, p), "AddSourceEx opens the clip");
    check(wait_until([&] { return p.play > 0; }), "the player reaches Play");
    std::this_thread::sleep_for(3s);
    check(a.is_active(p.handle), "the player stays active while frames wait to be consumed");
    check(p.stop == 0, "no Stop event arrives before the frames are consumed");
    check(a.pause(p.handle) == ORBIS_OK, "Pause with frames queued succeeds");
    check(a.resume(p.handle) == ORBIS_OK, "Resume succeeds");
    int delivered = 0;
    const auto end = std::chrono::steady_clock::now() + 20s;
    while (a.is_active(p.handle) && std::chrono::steady_clock::now() < end) {
        AvPlayerFrameInfo audio{};
        AvPlayerFrameInfoEx video{};
        a.audio(p.handle, &audio);
        delivered += a.video_ex(p.handle, &video);
        std::this_thread::sleep_for(2ms);
    }
    check(delivered == 30, "all 30 frames are delivered once consumption starts");
    check(wait_until([&] { return p.stop == 1; }, 5s), "exactly one Stop event follows the last frame");
    int pauses_failed = 0;
    for (int i = 0; i < 50; ++i) {
        pauses_failed += a.pause(p.handle) != ORBIS_OK;
    }
    check(pauses_failed == 0, "Pause repeated at the end of the file never fails");
    check(a.close(p.handle) == ORBIS_OK, "Close succeeds");
    check(threads_settle(baseline), "no worker thread survives the end-of-file test");

    int refused = 0;
    for (int round = 0; round < 4; ++round) {
        Player q;
        open_player(a, q);
        add_clip(a, q);
        wait_until([&] { return q.play > 0; });
        const auto limit = std::chrono::steady_clock::now() + 20s;
        while (a.is_active(q.handle) && std::chrono::steady_clock::now() < limit) {
            refused += a.pause(q.handle) != ORBIS_OK;
            refused += a.resume(q.handle) != ORBIS_OK;
            AvPlayerFrameInfo audio{};
            AvPlayerFrameInfoEx video{};
            a.audio(q.handle, &audio);
            a.video_ex(q.handle, &video);
            std::this_thread::sleep_for(1ms);
        }
        a.close(q.handle);
    }
    check(refused == 0, "Pause and Resume racing the end of the file are never refused");
    check(threads_settle(baseline), "no worker thread survives the Pause race");
}

// The game's own sequence: Pause, wait for the Pause event, JumpToTime, wait for the jump-complete warning, then Resume.
static void test_jump_complete(Api& a, int baseline) {
    std::puts("media: Pause, JumpToTime, jump-complete, Resume");
    Player p;
    open_player(a, p);
    add_clip(a, p);
    check(wait_until([&] { return p.play > 0; }), "the player reaches Play");
    check(pump(a, p, 3) == 3, "frames are delivered");
    p.wait.arm(s32(AvPlayerEvents::StatePause));
    check(a.pause(p.handle) == ORBIS_OK, "Pause succeeds");
    check(p.wait.wait(3s), "the Pause event completes the game's wait");

    const int allocations = g_allocations;
    const int threads = thread_count();
    const int plays = p.play;
    p.wait.arm(ORBIS_AVPLAYER_ERROR_WAR_JUMP_COMPLETE);
    check(a.jump(p.handle, 0) == ORBIS_OK, "JumpToTime succeeds");
    check(p.jump_count == 1, "the warning is delivered before JumpToTime returns");
    check(p.wait.wait(3s), "completion before the wait: the wait does not block");
    check(p.play == plays + 1, "the paused player was resumed once");
    check(pump(a, p, 3) == 3, "frames are delivered after the jump");
    check(a.resume(p.handle) == ORBIS_OK, "the game's Resume after the jump succeeds");
    std::this_thread::sleep_for(200ms);
    check(p.jump_count == 1 && p.play == plays + 1, "the warning arrives exactly once and Resume sends no second Play");
    check(g_allocations == allocations && thread_count() == threads, "no buffer is allocated and no worker is restarted");
    check(pump(a, p, 5) == 5, "frames keep arriving after the Resume");

    check(a.pause(p.handle) == ORBIS_OK, "Pause succeeds again");
    p.wait.arm(ORBIS_AVPLAYER_ERROR_WAR_JUMP_COMPLETE);
    std::atomic<bool> waiting{false}, completed{false};
    std::thread waiter([&] {
        waiting = true;
        completed = p.wait.wait(5s);
    });
    wait_until([&] { return waiting.load(); });
    std::this_thread::sleep_for(150ms);
    check(!completed, "the wait blocks until the warning arrives");
    check(a.jump(p.handle, 0) == ORBIS_OK, "JumpToTime succeeds with a waiter already blocked");
    waiter.join();
    check(completed, "wait before completion: the warning wakes the waiter");
    check(p.jump_count == 2, "a second jump sends exactly one more warning");
    check(pump(a, p, 3) == 3 && a.resume(p.handle) == ORBIS_OK, "the second jump also leaves a running player");

    p.wait.arm(ORBIS_AVPLAYER_ERROR_WAR_JUMP_COMPLETE);
    const int plays_before = p.play, pauses_before = p.pause;
    check(a.jump(p.handle, 2000) == ORBIS_OK && p.wait.wait(3s), "JumpToTime while playing completes its wait");
    check(p.jump_count == 3 && p.play == plays_before && p.pause == pauses_before,
          "a jump on a playing player changes no state");
    check(!p.wait.wait(50ms), "a wait without a signal times out");
    check(p.wait.counter == 0, "and leaves the counter intact");
    check(a.jump(nullptr, 0) == ORBIS_AVPLAYER_ERROR_INVALID_PARAMS, "a null handle is refused");
    std::this_thread::sleep_for(100ms);
    check(p.jump_count == 3, "a null handle sends no warning");
    a.close(p.handle);
    check(threads_settle(baseline), "no worker thread survives");
}

static void test_stop_is_not_eof(Api& a, int baseline) {
    std::puts("media: stop before the end of the file");
    Player p;
    open_player(a, p);
    add_clip(a, p);
    check(wait_until([&] { return p.play > 0; }), "the player reaches Play");
    check(a.stop(p.handle) == ORBIS_OK, "Stop succeeds before the end of the clip");
    std::this_thread::sleep_for(400ms);
    check(p.stop == 1, "exactly one Stop event is sent");
    check(a.pause(p.handle) == ORBIS_AVPLAYER_ERROR_OPERATION_FAILED, "the stopped player did not turn into an ended one");
    check(a.start(p.handle) == ORBIS_OK, "Start after Stop plays again");
    check(pump(a, p, 5) == 5, "the restarted player delivers frames");
    a.close(p.handle);
    check(threads_settle(baseline), "no worker thread survives");
}

// The game's own file callbacks (what Unity installs) instead of the runtime mapper.
struct GameFile {
    FILE* file = nullptr;
    std::atomic<int> opens{0}, reads{0}, closes{0};
    std::string name;
    std::atomic<DWORD> reader_thread{0};
};

static s32 PS4_SYSV_ABI GameOpen(void* opaque, const char* name) {
    auto* f = static_cast<GameFile*>(opaque);
    f->name = name;
    ++f->opens;
    static const char prefix[] = "/app0/";
    const std::string path = g_media_dir + "\\" + (name + sizeof(prefix) - 1);
    f->file = std::fopen(path.c_str(), "rb");
    return f->file ? 1 : -1;
}
static s32 PS4_SYSV_ABI GameClose(void* opaque) {
    auto* f = static_cast<GameFile*>(opaque);
    ++f->closes;
    if (f->file) {
        std::fclose(f->file);
        f->file = nullptr;
    }
    return 0;
}
static s32 PS4_SYSV_ABI GameRead(void* opaque, u8* buffer, u64 position, u32 length) {
    auto* f = static_cast<GameFile*>(opaque);
    ++f->reads;
    f->reader_thread = GetCurrentThreadId();
    std::fseek(f->file, long(position), SEEK_SET);
    return s32(std::fread(buffer, 1, length, f->file));
}
static u64 PS4_SYSV_ABI GameSize(void* opaque) {
    auto* f = static_cast<GameFile*>(opaque);
    std::fseek(f->file, 0, SEEK_END);
    return u64(std::ftell(f->file));
}

static void test_file_replacement(Api& a, int baseline) {
    std::puts("media: the game's file callbacks");
    Player p;
    GameFile file;
    p.api = &a;
    auto init = make_init(&p);
    init.file_replacement = {&file, &GameOpen, &GameClose, &GameRead, &GameSize};
    check(a.init_ex(&init, &p.handle) == ORBIS_OK, "InitEx with file callbacks");
    size_t translated;
    {
        std::lock_guard lock(g_paths_mutex);
        translated = g_translated.size();
    }
    check(add_clip(a, p), "AddSourceEx opens through the game's open callback");
    check(file.opens == 1 && file.name == "/app0/tiny.mp4", "the callback gets the guest path");
    {
        std::lock_guard lock(g_paths_mutex);
        check(g_translated.size() == translated, "the runtime mapper is not used");
    }
    check(wait_until([&] { return p.play > 0; }), "playback starts");
    u64 first = 0;
    check(pump(a, p, 5, &first) == 5, "frames arrive through the read callback");
    check(file.reads > 0 && file.reader_thread != GetCurrentThreadId(), "the clip is read on a worker thread");
    check(a.close(p.handle) == ORBIS_OK, "Close succeeds");
    check(file.closes == 1, "the game's close callback runs once");
    check(threads_settle(baseline), "no worker thread survives Close");
}

static void test_looping(Api& a, int baseline) {
    std::puts("media: looping");
    Player p;
    p.start_in_ready = false;
    open_player(a, p);
    add_clip(a, p);
    check(wait_ready(p), "the Ready event arrives");
    check(a.looping(p.handle, true) == ORBIS_OK, "SetLooping succeeds");
    check(a.start(p.handle) == ORBIS_OK, "Start follows");
    check(pump(a, p, 40, nullptr, 30s) == 40 && a.is_active(p.handle), "a looping clip keeps delivering past its end");
    check(a.close(p.handle) == ORBIS_OK, "Close succeeds");
    check(threads_settle(baseline), "no worker thread survives Close");
}

static void test_close_variants(Api& a, int baseline) {
    std::puts("close while busy");
    {
        Player p;
        open_player(a, p);
        add_clip(a, p);
        wait_until([&] { return p.play > 0; });
        pump(a, p, 2);
        check(a.close(p.handle) == ORBIS_OK, "Close during decode succeeds");
        check(threads_settle(baseline), "no worker thread survives Close during decode");
    }
    {
        Player p;
        p.close_in_ready = true;
        open_player(a, p);
        add_clip(a, p);
        check(wait_until([&] { return p.ready > 0; }), "the Ready callback runs");
        check(p.close_result == ORBIS_OK, "Close from the event callback succeeds");
        check(a.stream_count(p.handle) == ORBIS_AVPLAYER_ERROR_INVALID_PARAMS, "the handle is gone afterwards");
        check(threads_settle(baseline), "the controller thread ends after a Close from its own callback");
    }
    {
        Player p;
        p.start_in_ready = false;
        open_player(a, p);
        add_clip(a, p);
        wait_ready(p);
        check(a.stop(p.handle) == ORBIS_AVPLAYER_ERROR_OPERATION_FAILED, "Stop before Start fails");
        check(a.close(p.handle) == ORBIS_OK, "Close after a refused Stop succeeds");
        check(threads_settle(baseline), "no worker thread survives");
    }
    {
        Player p;
        p.start_in_ready = false;
        open_player(a, p);
        add_clip(a, p);
        wait_ready(p);
        const int events = p.events, allocations = g_allocations;
        check(a.jump(p.handle, 1000) == ORBIS_OK, "JumpToTime before Start is accepted");
        check(p.jump_count == 1 && g_allocations == allocations,
              "JumpToTime before Start sends one warning and allocates nothing");
        check(a.start(p.handle) == ORBIS_OK, "Start follows");
        u64 first = 0;
        check(pump(a, p, 2, &first) == 2 && first < 500, "playback still begins at the start of the clip");
        a.close(p.handle);
        check(threads_settle(baseline), "no worker thread survives");
    }
    int errors = 0;
    for (int round = 0; round < 25; ++round) {
        Player p;
        p.touch_every_event = true;
        open_player(a, p);
        add_clip(a, p);
        if (round % 2) {
            wait_until([&] { return p.events > 0; }, 1s);
        }
        errors += a.close(p.handle) != ORBIS_OK;
    }
    check(errors == 0, "Close races the callbacks that call back into the player 25 times without a failure");
    check(threads_settle(baseline), "no worker thread survives the races");
}

struct Drained {
    int frames = 0;
    bool audio_silent = true;
    int audio_frames = 0;
};

static Drained drain(Api& a, Player& p) {
    Drained result;
    const auto end = std::chrono::steady_clock::now() + 20s;
    while (a.is_active(p.handle) && std::chrono::steady_clock::now() < end) {
        AvPlayerFrameInfo audio{};
        AvPlayerFrameInfoEx video{};
        if (a.audio(p.handle, &audio)) {
            ++result.audio_frames;
            const auto* bytes = static_cast<const u8*>(static_cast<const void*>(audio.p_data));
            for (u32 i = 0; i < audio.details.audio.size; ++i) {
                result.audio_silent = result.audio_silent && bytes[i] == 0;
            }
        }
        result.frames += a.video_ex(p.handle, &video);
        std::this_thread::sleep_for(2ms);
    }
    return result;
}

// Skip Intro cuts the first source the game opens; the transitions are the natural end of the file.
static void test_skip_intro(Api& a, int baseline) {
    std::puts("media: Skip Intro");
    SkipIntro::Global().Configure(true);
    {
        Player p;
        open_player(a, p);
        check(add_clip(a, p), "the first source opens");
        check(wait_until([&] { return p.play > 0; }), "the first source reaches Play");
        const Drained cut = drain(a, p);
        check(cut.frames >= 2 && cut.frames < 10, "only the first moments of the startup video are delivered");
        check(cut.audio_frames > 0 && cut.audio_silent, "the audio that is delivered is silent");
        check(!a.is_active(p.handle), "the player ends without waiting for the full clip");
        check(wait_until([&] { return p.stop == 1; }, 5s), "exactly one Stop event follows, as at a natural end");
        check(a.pause(p.handle) == ORBIS_OK && a.resume(p.handle) == ORBIS_OK,
              "Pause and Resume after the cut are accepted like after a natural end");
        check(a.stop(p.handle) == ORBIS_OK, "Stop after the cut succeeds");
        check(p.stop == 1, "Stop sends no second Stop event");
        check(a.close(p.handle) == ORBIS_OK, "Close succeeds");
    }
    {
        Player q;
        open_player(a, q);
        check(add_clip(a, q), "the second source opens");
        check(wait_until([&] { return q.play > 0; }), "the second source reaches Play");
        const Drained full = drain(a, q);
        check(full.frames == 30, "later sources play completely");
        check(!full.audio_silent, "later sources keep their audio");
        a.close(q.handle);
    }
    SkipIntro::Global().Configure(false);
    {
        Player r;
        open_player(a, r);
        check(add_clip(a, r), "with Skip Intro off the first source opens");
        check(wait_until([&] { return r.play > 0; }), "with Skip Intro off the source reaches Play");
        check(drain(a, r).frames == 30, "with Skip Intro off the first source plays completely");
        a.close(r.handle);
    }
    check(threads_settle(baseline), "no worker thread survives the Skip Intro test");
}

static void test_skip_intro_policy() {
    std::puts("Skip Intro policy");
    SkipIntro::Policy policy;
    check(!policy.ClaimSource() && !policy.ClaimSource(), "nothing is claimed while the setting is off");
    policy.Configure(true);
    check(policy.ClaimSource(), "the first source is claimed");
    check(!policy.ClaimSource() && !policy.ClaimSource(), "later sources are not");
    policy.Configure(true);
    check(policy.ClaimSource(), "configuring again restarts the count");
    policy.Configure(false);
    check(!policy.ClaimSource(), "turning the setting off claims nothing");
    check(!SkipIntro::Policy::PastPlayableWindow(0) && !SkipIntro::Policy::PastPlayableWindow(SkipIntro::kPlayableMillis) &&
              SkipIntro::Policy::PastPlayableWindow(SkipIntro::kPlayableMillis + 1),
          "the playable window ends after its limit");
}

int main() {
    std::thread([] {
        std::this_thread::sleep_for(180s);
        std::puts("FAIL: the test hangs");
        std::fflush(stdout);
        std::_Exit(1);
    }).detach();

    if (const char* media = std::getenv("AVPLAYER_TEST_MEDIA")) {
        g_media_dir = media;
    }
    Core::Loader::SymbolsResolver resolver;
    RegisterLib(&resolver);
    Api a;

    test_abi();
    test_registry();
    test_log_callback(a);
    test_invalid_handles(a);
    test_init(a);
    test_skip_intro_policy();
    if (!g_media_dir.empty()) {
        std::this_thread::sleep_for(100ms);
        const int baseline = thread_count();
        test_media(a, baseline);
        test_eof_lifecycle(a, baseline);
        test_jump_complete(a, baseline);
        test_stop_is_not_eof(a, baseline);
        test_looping(a, baseline);
        test_file_replacement(a, baseline);
        test_close_variants(a, baseline);
        test_skip_intro(a, baseline);
    } else {
        std::puts("media tests skipped: AVPLAYER_TEST_MEDIA is not set");
    }
    std::printf(failures ? "%d failure(s)\n" : "all AvPlayer tests passed\n", failures);
    std::fflush(stdout);
    std::_Exit(failures ? 1 : 0);
}
