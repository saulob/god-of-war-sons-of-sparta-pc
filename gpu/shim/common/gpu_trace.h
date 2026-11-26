// dcp: an always-on, bounded record of the significant GPU operations, printed once when the device is lost or a GPU
// assertion fails. Recording is lock-free and allocation-free: a few relaxed atomic stores per event, so it stays on in
// release builds. The ticks of each timeline are the device-lost monitor's (common/device_lost.h); this record adds what
// led up to the loss: the last events, the pipelines of the submissions the GPU had not finished, the newest dispatches.
// Every printed line starts with "GPU trace: " and the whole dump is bounded (MaxDumpLines lines of MaxLineChars).
#pragma once
#include <cstddef>
#include "common/device_lost.h"
#include "common/types.h"

namespace Common::GpuTrace {

using Timeline = DeviceLost::Timeline;

enum class Event : u32 {
    Submit,              // tick, waits, first wait tick, result | timeline << 32
    SparseBind,          // tick, buffers, result
    BlockingWait,        // tick, completed tick, timeline
    ShaderCompile,       // shader hash, stage tag
    PipelineFirstUse,    // PipelineKind, then cs and packed groups, or vs gs ps
    ArenaCreated,        // guest address, bytes
    GeometryOutputClamp, // gs hash, declared vertices, used vertices, components per vertex
    RenderingMismatch,   // kind
    AllocationFallback,  // see Fallback
    VulkanFailure,       // site tag, result, tick, timeline
    HeapBudget,          // heap, used bytes, budget bytes, 1 when it went over
    Count,
};

enum class PipelineKind : u64 { Graphics = 1, Compute = 2 };

// AllocationFallback: (width, height, ImageOverBudget | ImageInSystemMemory), (bytes, tag, BufferOverBudget) or
// (bytes, memory type, SparseBackingInSystemMemory).
enum class Fallback : u64 {
    ImageOverBudget = 1,
    ImageInSystemMemory = 2,
    BufferOverBudget = 3,
    SparseBackingInSystemMemory = 4,
};

inline constexpr u32 Capacity = 512;          // events kept, the newest ones
inline constexpr u32 ComputePerSet = 256;     // distinct compute pipelines kept per submission
inline constexpr u32 GraphicsPerSet = 64;     // distinct graphics pipelines kept per submission
inline constexpr u32 GraphicsPrinted = 16;    // of those, printed
inline constexpr u32 DispatchesPerSet = 32;   // the newest dispatches of a submission, in order
inline constexpr u32 KeptSubmissions = 4;     // draw submissions whose pipelines are kept
inline constexpr u32 MaxDumpLines = 1000;     // every dump stays within this many lines...
inline constexpr u32 MaxLineChars = 220;      // ...of at most this many characters
inline constexpr const char* LinePrefix = "GPU trace: ";
inline constexpr const char* SummaryPrefix = "GPU trace: summary: ";

void Record(Event event, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0) noexcept;

// Shaders recorded into the draw submission being built. A pipeline notes itself once per generation; a draw-timeline
// NoteSubmitted keeps the set under that tick and starts the next one.
u64 ShaderGeneration() noexcept;
void NoteShaders(PipelineKind kind, u64 first, u64 second = 0, u64 third = 0) noexcept;
// Also kept in order with the submission being built: its newest DispatchesPerSet dispatches are printed.
void NoteDispatch(u64 cs, u32 x, u32 y, u32 z) noexcept;
void SetLastGraphics(u64 vs, u64 gs, u64 ps) noexcept;
// After a successful submission of `tick` (the device-lost monitor keeps the ticks themselves).
void NoteSubmitted(Timeline timeline, u64 tick) noexcept;
void SetPendingWork(u64 pending_operations, u64 queued_record_chunks) noexcept;

void SetDeviceIdentity(const char* identity) noexcept;

// The OS thread id, also the device-lost monitor's; and the thread's description, or "unnamed".
u32 CurrentThreadId() noexcept;
void ThreadName(u32 tid, char* out, size_t size) noexcept;

// Records a fatal Vulkan result and prints the trace (once per process).
void VulkanFailure(const char* site, s32 vk_result, u64 tick = 0, Timeline timeline = Timeline::Draw) noexcept;
// Prints the trace once; later calls print nothing.
void Dump(const char* reason) noexcept;

// A consistent copy of a ring event. ReadEvents copies the newest events, oldest first, and skips any slot a writer is
// changing; it returns how many it copied.
struct EventRecord {
    u64 seq;
    Event event;
    u32 thread;
    u64 args[4];
};
u32 ReadEvents(EventRecord* out, u32 capacity) noexcept;
// One event as text, without its sequence number and thread ("draw submit tick 5, ...").
void DescribeEvent(const EventRecord& record, char* out, size_t size) noexcept;

constexpr u64 PackGroups(u32 x, u32 y, u32 z) noexcept {
    return u64(x & 0x1FFFFF) | (u64(y & 0x1FFFFF) << 21) | (u64(z & 0x1FFFFF) << 42);
}

// The first eight bytes of a short name, for stage and site tags in event payloads.
constexpr u64 Tag(const char* text) noexcept {
    u64 value = 0;
    for (u32 i = 0; text && i < 8 && text[i]; ++i) {
        value |= u64(u8(text[i])) << (i * 8);
    }
    return value;
}

} // namespace Common::GpuTrace
