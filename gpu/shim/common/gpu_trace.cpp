// dcp: see gpu_trace.h.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "common/gpu_trace.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Common::GpuTrace {
namespace {

constexpr s32 DeviceLostResult = DeviceLost::DeviceLostResult;

// One ring slot. `seq` is 0 while a writer fills the slot and the event's sequence number once it is complete; `check`
// covers every field, so a slot two writers raced over (the ring wrapped under a stalled writer) is never printed.
struct Entry {
    std::atomic<u64> seq{};
    std::atomic<u64> check{};
    std::atomic<u32> event{};
    std::atomic<u32> thread{};
    std::atomic<u64> args[4]{};
};

std::array<Entry, Capacity> ring;
std::atomic<u64> next_seq{1};

struct GraphicsUse {
    std::atomic<u64> vs{}, gs{}, ps{};
};

struct DispatchUse {
    std::atomic<u64> cs{}, groups{};
};

// The pipelines of one draw submission. Every distinct compute pipeline is kept: a hung dispatch is usually one of them.
struct ShaderSet {
    std::atomic<u64> tick{}; // 0 while recording
    std::atomic<u32> compute_count{};
    std::atomic<u32> graphics_count{};
    std::atomic<u64> dispatch_count{};
    std::array<std::atomic<u64>, ComputePerSet> compute{};
    std::array<GraphicsUse, GraphicsPerSet> graphics{};
    std::array<DispatchUse, DispatchesPerSet> dispatches{};
};

// The set being recorded plus the last KeptSubmissions submitted ones. A submission only moves the recording index, so
// the hot path never copies a set.
constexpr u32 SetCount = KeptSubmissions + 1;
std::array<ShaderSet, SetCount> sets;
std::atomic<u32> recording_index{};
std::atomic<u64> shader_generation{1};

std::atomic<u64> pending_operations{}, queued_record_chunks{};
std::atomic<u64> last_vs{}, last_gs{}, last_ps{}, last_cs{}, last_groups{};
std::atomic<bool> dumped{};

char device_identity[192];

u64 Mix(u64 seq, u32 event, u32 thread, const u64 (&args)[4]) noexcept {
    u64 hash = (seq + 0x9E3779B97F4A7C15ull) ^ (u64(event) << 32 | thread);
    for (const u64 arg : args) {
        hash = (hash ^ arg) * 0x100000001B3ull;
        hash ^= hash >> 29;
    }
    return hash | 1; // never 0
}

u32 OsThreadId() noexcept {
#ifdef _WIN32
    return u32(GetCurrentThreadId());
#else
    return u32(syscall(SYS_gettid));
#endif
}

u32 ThreadId() noexcept {
    static thread_local const u32 tid = OsThreadId();
    return tid;
}

void CopyText(char* out, size_t size, const char* text) noexcept {
    DeviceLost::CopyDriverText(out, size, text, text ? std::strlen(text) : 0);
}

bool ReadEntry(u64 seq, EventRecord& out) noexcept {
    const Entry& entry = ring[seq % Capacity];
    if (entry.seq.load(std::memory_order_acquire) != seq) {
        return false;
    }
    const u32 event = entry.event.load(std::memory_order_relaxed);
    const u32 thread = entry.thread.load(std::memory_order_relaxed);
    const u64 args[4] = {entry.args[0].load(std::memory_order_relaxed), entry.args[1].load(std::memory_order_relaxed),
                         entry.args[2].load(std::memory_order_relaxed), entry.args[3].load(std::memory_order_relaxed)};
    const u64 check = entry.check.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (entry.seq.load(std::memory_order_relaxed) != seq || check != Mix(seq, event, thread, args) ||
        event >= u32(Event::Count)) {
        return false;
    }
    out.seq = seq;
    out.event = Event(event);
    out.thread = thread;
    std::copy(std::begin(args), std::end(args), out.args);
    return true;
}

const char* TimelineText(u64 timeline) noexcept {
    return timeline < u64(Timeline::Count) ? DeviceLost::TimelineName(Timeline(timeline)) : "unknown";
}

void TagText(char (&out)[9], u64 tag) noexcept {
    for (u32 i = 0; i < 8; ++i) {
        const char c = char((tag >> (i * 8)) & 0xFF);
        out[i] = c >= 0x20 && c < 0x7F ? c : (c == 0 ? '\0' : '?');
        if (c == 0) {
            break;
        }
    }
    out[8] = '\0';
}

void GroupsText(char* out, size_t size, u64 groups) noexcept {
    std::snprintf(out, size, "%llux%llux%llu", (unsigned long long)(groups & 0x1FFFFF),
                  (unsigned long long)((groups >> 21) & 0x1FFFFF), (unsigned long long)((groups >> 42) & 0x1FFFFF));
}

// --- the dump: every line through Line(), which prefixes, bounds and counts it ---

struct Output {
    u32 lines = 0;
    bool limited = false;
};
Output output;

constexpr u32 ContentLines = MaxDumpLines - 2; // the limit note and the closing line are always room

void WriteLine(const char* text) noexcept {
    std::fputs(text, stderr);
    std::fputc('\n', stderr);
    ++output.lines;
}

void Line(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Line(const char* format, ...) {
    if (output.lines >= ContentLines) {
        if (!output.limited) {
            output.limited = true;
            char note[96];
            std::snprintf(note, sizeof(note), "%soutput limit of %u lines reached, the rest is not shown", LinePrefix,
                          MaxDumpLines);
            WriteLine(note);
        }
        return;
    }
    char raw[MaxLineChars + 1];
    const size_t prefix = std::strlen(LinePrefix);
    std::memcpy(raw, LinePrefix, prefix);
    va_list args;
    va_start(args, format);
    std::vsnprintf(raw + prefix, sizeof(raw) - prefix, format, args);
    va_end(args);
    char line[MaxLineChars + 1];
    CopyText(line, sizeof(line), raw); // one line, no control characters
    WriteLine(line);
}

u32 PipelineCount(const ShaderSet& set) noexcept {
    return set.compute_count.load(std::memory_order_relaxed) + set.graphics_count.load(std::memory_order_relaxed);
}

void PrintSet(const ShaderSet& set) noexcept {
    const u32 compute = set.compute_count.load(std::memory_order_relaxed);
    const u32 compute_kept = std::min(compute, ComputePerSet);
    for (u32 i = 0; i < compute_kept; i += 8) {
        char hashes[MaxLineChars];
        size_t used = 0;
        hashes[0] = '\0';
        for (u32 j = i; j < std::min(i + 8, compute_kept) && used < sizeof(hashes); ++j) {
            const int written = std::snprintf(hashes + used, sizeof(hashes) - used, " 0x%08llx",
                                              (unsigned long long)set.compute[j].load(std::memory_order_relaxed));
            used += written > 0 ? size_t(written) : 0;
        }
        Line("  cs%s", hashes);
    }
    if (compute > compute_kept) {
        Line("  and %u more compute pipelines", compute - compute_kept);
    }
    const u32 graphics = set.graphics_count.load(std::memory_order_relaxed);
    const u32 graphics_shown = std::min({graphics, GraphicsPrinted, GraphicsPerSet});
    for (u32 i = 0; i < graphics_shown; ++i) {
        const GraphicsUse& use = set.graphics[i];
        Line("  vs 0x%08llx gs 0x%08llx ps 0x%08llx", (unsigned long long)use.vs.load(std::memory_order_relaxed),
             (unsigned long long)use.gs.load(std::memory_order_relaxed),
             (unsigned long long)use.ps.load(std::memory_order_relaxed));
    }
    if (graphics > graphics_shown) {
        Line("  and %u more graphics pipelines", graphics - graphics_shown);
    }
    const u64 dispatches = set.dispatch_count.load(std::memory_order_relaxed);
    if (dispatches == 0) {
        return;
    }
    const u64 first = dispatches > DispatchesPerSet ? dispatches - DispatchesPerSet : 0;
    Line("  dispatches %llu..%llu of %llu (oldest first):", (unsigned long long)first + 1,
         (unsigned long long)dispatches, (unsigned long long)dispatches);
    for (u64 i = first; i < dispatches; i += 4) {
        char uses[MaxLineChars];
        size_t used = 0;
        uses[0] = '\0';
        for (u64 j = i; j < std::min<u64>(i + 4, dispatches) && used < sizeof(uses); ++j) {
            const DispatchUse& use = set.dispatches[j % DispatchesPerSet];
            char groups[48];
            GroupsText(groups, sizeof(groups), use.groups.load(std::memory_order_relaxed));
            const int written = std::snprintf(uses + used, sizeof(uses) - used, " cs 0x%08llx %s",
                                              (unsigned long long)use.cs.load(std::memory_order_relaxed), groups);
            used += written > 0 ? size_t(written) : 0;
        }
        Line("   %s", uses);
    }
}

// The submitted draw sets the GPU had not finished, oldest first.
u32 UnfinishedSets(u64 submitted, u64 completed, std::array<u32, SetCount>& found) noexcept {
    const u32 recording = recording_index.load(std::memory_order_acquire) % SetCount;
    std::array<u64, SetCount> ticks{};
    u32 count = 0;
    for (u32 i = 0; i < SetCount; ++i) {
        const u64 tick = sets[i].tick.load(std::memory_order_acquire);
        if (i != recording && tick != 0 && tick > completed && tick <= submitted) {
            ticks[count] = tick;
            found[count++] = i;
        }
    }
    // Insertion sort by tick: at most SetCount entries.
    for (u32 i = 1; i < count; ++i) {
        for (u32 j = i; j > 0 && ticks[j - 1] > ticks[j]; --j) {
            std::swap(ticks[j - 1], ticks[j]);
            std::swap(found[j - 1], found[j]);
        }
    }
    return count;
}

// "cs 0x... 0x... vs 0x... ps 0x...": the first pipelines of a set, for the summary line.
void FirstPipelines(const ShaderSet& set, char* out, size_t size) noexcept {
    size_t used = 0;
    out[0] = '\0';
    const auto add = [&](const char* format, unsigned long long value) {
        if (used + 1 < size) {
            const int written = std::snprintf(out + used, size - used, format, value);
            used = written > 0 ? std::min(size - 1, used + size_t(written)) : used;
        }
    };
    const u32 compute = std::min(set.compute_count.load(std::memory_order_relaxed), ComputePerSet);
    for (u32 i = 0; i < std::min(compute, 2u); ++i) {
        add(i == 0 ? "cs 0x%08llx" : " 0x%08llx", set.compute[i].load(std::memory_order_relaxed));
    }
    const u32 graphics = std::min(set.graphics_count.load(std::memory_order_relaxed), GraphicsPerSet);
    if (graphics) {
        add(used ? ", vs 0x%08llx" : "vs 0x%08llx", set.graphics[0].vs.load(std::memory_order_relaxed));
        add(" ps 0x%08llx", set.graphics[0].ps.load(std::memory_order_relaxed));
    }
    if (!used) {
        std::snprintf(out, size, "none");
    }
}

EventRecord dump_events[Capacity]; // the dump runs once: no large stack frame on a thread that met the loss

void PrintThreadNames(const EventRecord* events, u32 count) noexcept {
    constexpr u32 MaxThreads = 16;
    u32 seen[MaxThreads];
    u32 known = 0;
    for (u32 i = 0; i < count && known < MaxThreads; ++i) {
        const u32 tid = events[i].thread;
        if (tid != 0 && std::find(seen, seen + known, tid) == seen + known) {
            seen[known++] = tid;
        }
    }
    for (u32 i = 0; i < known; ++i) {
        char name[64];
        ThreadName(seen[i], name, sizeof(name));
        Line("thread %u is %s", seen[i], name);
    }
}

} // namespace

void Record(Event event, u64 a, u64 b, u64 c, u64 d) noexcept {
    const u64 seq = next_seq.fetch_add(1, std::memory_order_relaxed);
    Entry& entry = ring[seq % Capacity];
    const u32 thread = ThreadId();
    const u64 args[4] = {a, b, c, d};
    entry.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    entry.event.store(u32(event), std::memory_order_relaxed);
    entry.thread.store(thread, std::memory_order_relaxed);
    for (u32 i = 0; i < 4; ++i) {
        entry.args[i].store(args[i], std::memory_order_relaxed);
    }
    entry.check.store(Mix(seq, u32(event), thread, args), std::memory_order_relaxed);
    entry.seq.store(seq, std::memory_order_release);
}

u32 ReadEvents(EventRecord* out, u32 capacity) noexcept {
    const u64 last = next_seq.load(std::memory_order_acquire) - 1;
    const u64 window = std::min<u64>(capacity, Capacity);
    const u64 first = last >= window ? last - window + 1 : 1;
    u32 count = 0;
    for (u64 seq = first; seq <= last && count < capacity; ++seq) {
        if (ReadEntry(seq, out[count])) {
            ++count;
        }
    }
    return count;
}

void DescribeEvent(const EventRecord& record, char* out, size_t size) noexcept {
    const u64* a = record.args;
    char tag[9];
    switch (record.event) {
    case Event::Submit:
        std::snprintf(out, size, "%s submit tick %llu, %llu waits (first %llu), result %d", TimelineText(a[3] >> 32),
                      (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2], s32(u32(a[3])));
        return;
    case Event::SparseBind:
        std::snprintf(out, size, "sparse bind tick %llu, %llu buffers, result %d", (unsigned long long)a[0],
                      (unsigned long long)a[1], s32(u32(a[2])));
        return;
    case Event::BlockingWait:
        std::snprintf(out, size, "blocking wait for %s tick %llu (completed %llu)", TimelineText(a[2]),
                      (unsigned long long)a[0], (unsigned long long)a[1]);
        return;
    case Event::ShaderCompile:
        TagText(tag, a[1]);
        std::snprintf(out, size, "compile %s shader 0x%016llx", tag[0] ? tag : "?", (unsigned long long)a[0]);
        return;
    case Event::PipelineFirstUse:
        if (a[0] == u64(PipelineKind::Compute)) {
            char groups[48];
            GroupsText(groups, sizeof(groups), a[2]);
            std::snprintf(out, size, "first use of compute pipeline cs 0x%08llx, groups %s", (unsigned long long)a[1],
                          groups);
        } else {
            std::snprintf(out, size, "first use of graphics pipeline vs 0x%08llx gs 0x%08llx ps 0x%08llx",
                          (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3]);
        }
        return;
    case Event::ArenaCreated:
        std::snprintf(out, size, "buffer arena at 0x%llx, %llu MiB", (unsigned long long)a[0],
                      (unsigned long long)(a[1] >> 20));
        return;
    case Event::GeometryOutputClamp:
        std::snprintf(out, size, "gs 0x%08llx output vertices %llu -> %llu (%llu components each)",
                      (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2],
                      (unsigned long long)a[3]);
        return;
    case Event::RenderingMismatch:
        std::snprintf(out, size, "unpaired dynamic rendering (%s), corrected",
                      a[0] == 0 ? "end without begin" : a[0] == 1 ? "begin while open" : "open at submission");
        return;
    case Event::AllocationFallback:
        switch (Fallback(a[2])) {
        case Fallback::ImageOverBudget:
        case Fallback::ImageInSystemMemory:
            std::snprintf(out, size, "image %llux%llu allocated %s", (unsigned long long)a[0], (unsigned long long)a[1],
                          Fallback(a[2]) == Fallback::ImageOverBudget ? "over the memory budget" : "in system memory");
            return;
        case Fallback::BufferOverBudget:
            TagText(tag, a[1]);
            std::snprintf(out, size, "%s buffer of %llu MiB allocated over the memory budget", tag[0] ? tag : "?",
                          (unsigned long long)(a[0] >> 20));
            return;
        case Fallback::SparseBackingInSystemMemory:
            std::snprintf(out, size, "buffer arena backing of %llu KiB in memory type %llu (not the preferred one)",
                          (unsigned long long)(a[0] >> 10), (unsigned long long)a[1]);
            return;
        }
        std::snprintf(out, size, "allocation fallback kind %llu", (unsigned long long)a[2]);
        return;
    case Event::VulkanFailure:
        TagText(tag, a[0]);
        std::snprintf(out, size, "%s at %s, result %d, %s tick %llu",
                      s32(u32(a[1])) == DeviceLostResult ? "DEVICE LOST" : "Vulkan failure", tag[0] ? tag : "?",
                      s32(u32(a[1])), TimelineText(a[3]), (unsigned long long)a[2]);
        return;
    case Event::HeapBudget:
        std::snprintf(out, size, "heap %llu %s: %llu MiB used of %llu MiB budget", (unsigned long long)a[0],
                      a[3] ? "went over its budget" : "is back within its budget", (unsigned long long)(a[1] >> 20),
                      (unsigned long long)(a[2] >> 20));
        return;
    case Event::Count:
        break;
    }
    std::snprintf(out, size, "unknown event");
}

u64 ShaderGeneration() noexcept {
    return shader_generation.load(std::memory_order_relaxed);
}

void NoteShaders(PipelineKind kind, u64 first, u64 second, u64 third) noexcept {
    ShaderSet& set = sets[recording_index.load(std::memory_order_acquire) % SetCount];
    if (kind == PipelineKind::Compute) {
        const u32 index = set.compute_count.fetch_add(1, std::memory_order_relaxed);
        if (index < ComputePerSet) {
            set.compute[index].store(first, std::memory_order_relaxed);
        }
        return;
    }
    const u32 index = set.graphics_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= GraphicsPerSet) {
        return;
    }
    GraphicsUse& use = set.graphics[index];
    use.vs.store(first, std::memory_order_relaxed);
    use.gs.store(second, std::memory_order_relaxed);
    use.ps.store(third, std::memory_order_relaxed);
}

void NoteDispatch(u64 cs, u32 x, u32 y, u32 z) noexcept {
    const u64 groups = PackGroups(x, y, z);
    last_cs.store(cs, std::memory_order_relaxed);
    last_groups.store(groups, std::memory_order_relaxed);
    ShaderSet& set = sets[recording_index.load(std::memory_order_acquire) % SetCount];
    const u64 index = set.dispatch_count.fetch_add(1, std::memory_order_relaxed);
    DispatchUse& use = set.dispatches[index % DispatchesPerSet];
    use.cs.store(cs, std::memory_order_relaxed);
    use.groups.store(groups, std::memory_order_relaxed);
}

void SetLastGraphics(u64 vs, u64 gs, u64 ps) noexcept {
    last_vs.store(vs, std::memory_order_relaxed);
    last_gs.store(gs, std::memory_order_relaxed);
    last_ps.store(ps, std::memory_order_relaxed);
}

void NoteSubmitted(Timeline timeline, u64 tick) noexcept {
    if (timeline != Timeline::Draw) {
        return;
    }
    // The set recorded so far now belongs to `tick`; the oldest kept set becomes the new recording set.
    const u32 current = recording_index.load(std::memory_order_relaxed) % SetCount;
    const u32 next = (current + 1) % SetCount;
    ShaderSet& fresh = sets[next];
    fresh.tick.store(0, std::memory_order_relaxed);
    fresh.compute_count.store(0, std::memory_order_relaxed);
    fresh.graphics_count.store(0, std::memory_order_relaxed);
    fresh.dispatch_count.store(0, std::memory_order_relaxed);
    sets[current].tick.store(tick, std::memory_order_release);
    recording_index.store(next, std::memory_order_release);
    shader_generation.fetch_add(1, std::memory_order_relaxed);
}

void SetPendingWork(u64 operations, u64 record_chunks) noexcept {
    pending_operations.store(operations, std::memory_order_relaxed);
    queued_record_chunks.store(record_chunks, std::memory_order_relaxed);
}

void SetDeviceIdentity(const char* identity) noexcept {
    CopyText(device_identity, sizeof(device_identity), identity);
}

u32 CurrentThreadId() noexcept {
    return ThreadId();
}

void ThreadName(u32 tid, char* out, size_t size) noexcept {
    if (size == 0) {
        return;
    }
    CopyText(out, size, "unnamed");
    if (tid == 0) {
        return;
    }
#ifdef _WIN32
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    if (!get_description) {
        return;
    }
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!thread) {
        return;
    }
    PWSTR description = nullptr;
    if (SUCCEEDED(get_description(thread, &description)) && description) {
        char name[128];
        if (description[0] &&
            WideCharToMultiByte(CP_UTF8, 0, description, -1, name, int(sizeof(name)), nullptr, nullptr) > 0) {
            CopyText(out, size, name);
        }
        LocalFree(description);
    }
    CloseHandle(thread);
#else
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%u/comm", tid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    char name[64];
    const ssize_t read_bytes = read(fd, name, sizeof(name) - 1);
    close(fd);
    if (read_bytes > 0) {
        name[read_bytes] = '\0';
        if (char* newline = std::strchr(name, '\n')) {
            *newline = '\0';
        }
        if (name[0]) {
            CopyText(out, size, name);
        }
    }
#endif
}

void VulkanFailure(const char* site, s32 vk_result, u64 tick, Timeline timeline) noexcept {
    Record(Event::VulkanFailure, Tag(site), u64(u32(vk_result)), tick, u64(timeline));
    Dump(vk_result == DeviceLostResult ? "Vulkan device lost" : "Vulkan operation failed");
}

void Dump(const char* reason) noexcept {
    if (dumped.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const u32 count = ReadEvents(dump_events, Capacity);
    const DeviceLost::Monitor& monitor = DeviceLost::Global();
    const DeviceLost::TimelineState draw = monitor.State(Timeline::Draw);
    std::array<u32, SetCount> unfinished{};
    const u32 unfinished_count = UnfinishedSets(draw.submitted, draw.completed, unfinished);
    const ShaderSet& recording = sets[recording_index.load(std::memory_order_acquire) % SetCount];

    char detecting[64];
    ThreadName(ThreadId(), detecting, sizeof(detecting));
    Line("%s, detected on thread %u (%s)", reason ? reason : "GPU failure", ThreadId(), detecting);

    // The one line the Bug Report reads: the last event and the work the GPU had not finished.
    {
        char last[128] = "none";
        if (count) {
            DescribeEvent(dump_events[count - 1], last, sizeof(last));
        }
        char flight[112];
        if (unfinished_count) {
            const ShaderSet& oldest = sets[unfinished[0]];
            char first[80];
            FirstPipelines(oldest, first, sizeof(first));
            std::snprintf(flight, sizeof(flight), "unfinished draw tick %llu (%u unfinished), %u pipelines: %s",
                          (unsigned long long)oldest.tick.load(std::memory_order_relaxed), unfinished_count,
                          PipelineCount(oldest), first);
        } else {
            char first[80];
            FirstPipelines(recording, first, sizeof(first));
            std::snprintf(flight, sizeof(flight), "no unfinished draw tick, %u pipelines since the last submission: %s",
                          PipelineCount(recording), first);
        }
        Line("summary: %s; last event: %s; %s", reason ? reason : "GPU failure", last, flight);
    }

    Line("device %s", device_identity[0] ? device_identity : "unknown");
    for (u32 timeline = 0; timeline < u32(Timeline::Count); ++timeline) {
        const DeviceLost::TimelineState state = monitor.State(Timeline(timeline));
        if (timeline != u32(Timeline::Draw) && state.submitted == 0 && state.completed == 0) {
            continue;
        }
        Line("%s timeline: last submitted tick %llu, last completed tick %llu", TimelineText(timeline),
             (unsigned long long)state.submitted, (unsigned long long)state.completed);
    }
    Line("at the last draw submission %llu operations waited for GPU ticks, %llu recorded chunks were queued",
         (unsigned long long)pending_operations.load(std::memory_order_relaxed),
         (unsigned long long)queued_record_chunks.load(std::memory_order_relaxed));
    char groups[48];
    GroupsText(groups, sizeof(groups), last_groups.load(std::memory_order_relaxed));
    Line("last recorded draw vs 0x%08llx gs 0x%08llx ps 0x%08llx; dispatch cs 0x%08llx groups %s",
         (unsigned long long)last_vs.load(std::memory_order_relaxed),
         (unsigned long long)last_gs.load(std::memory_order_relaxed),
         (unsigned long long)last_ps.load(std::memory_order_relaxed),
         (unsigned long long)last_cs.load(std::memory_order_relaxed), groups);

    for (u32 i = 0; i < unfinished_count; ++i) {
        const ShaderSet& set = sets[unfinished[i]];
        Line("draw tick %llu was not completed; it recorded %u pipelines (%u compute, %u graphics), %llu dispatches",
             (unsigned long long)set.tick.load(std::memory_order_relaxed), PipelineCount(set),
             set.compute_count.load(std::memory_order_relaxed), set.graphics_count.load(std::memory_order_relaxed),
             (unsigned long long)set.dispatch_count.load(std::memory_order_relaxed));
        PrintSet(set);
    }
    Line("%u pipelines recorded since the last draw submission, %llu dispatches", PipelineCount(recording),
         (unsigned long long)recording.dispatch_count.load(std::memory_order_relaxed));
    PrintSet(recording);

    PrintThreadNames(dump_events, count);
    if (count) {
        Line("events %llu..%llu (oldest first, %u shown)", (unsigned long long)dump_events[0].seq,
             (unsigned long long)dump_events[count - 1].seq, count);
    } else {
        Line("no events recorded");
    }
    for (u32 i = 0; i < count; ++i) {
        char text[MaxLineChars];
        DescribeEvent(dump_events[i], text, sizeof(text));
        Line("  #%llu tid %u %s", (unsigned long long)dump_events[i].seq, dump_events[i].thread, text);
    }
    char closing[64];
    std::snprintf(closing, sizeof(closing), "%send (%u lines)", LinePrefix, output.lines + 1);
    WriteLine(closing);
    std::fflush(stderr);
}

} // namespace Common::GpuTrace
