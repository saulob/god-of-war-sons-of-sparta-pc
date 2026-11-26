// Headless proof of the GPU crash trace (gpu/shim/common/gpu_trace.{h,cpp}): the lock-free ring keeps the newest events
// in order and never prints a torn one, recording allocates nothing, the dump is bounded and printed once, it names the
// pipelines of the draw submissions the GPU had not finished and the newest dispatches, the production device-lost
// monitor (SOS_SIMULATE_DEVICE_LOST) and a GPU assertion both print it before the process ends, and the Bug Report reads
// its summary line from the real output. The same checks run against mutated copies from tools/test_gpu_crash_trace.ps1.
// No window, GPU, launcher or game.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "bug_report.h"
#include "common/assert.h"
#include "common/device_lost.h"
#include "common/gpu_trace.h"

namespace {
FILE* g_stream = nullptr;
std::atomic<long long> g_allocations{0};
} // namespace

// Production stderr is the log; here it is a file the test reads back.
extern "C" FILE* sos_stderr_stream(void) {
    return g_stream ? g_stream : stdout;
}

// On Windows dcp_win_compat.h routes stderr through sos_stderr_stream. On Linux the production code writes to the real
// descriptors (the log pipe in the game), so the test points them at the capture file instead.
#ifndef _WIN32
#include <unistd.h>
namespace {
int g_saved_error = -1;
void CaptureDescriptors(FILE* file, bool output_too) {
    std::fflush(stdout);
    std::fflush(stderr);
    g_saved_error = dup(2);
    dup2(fileno(file), 2);
    if (output_too) {
        setvbuf(stdout, nullptr, _IONBF, 0); // the order of stdout and stderr lines is checked
        dup2(fileno(file), 1);
    }
}
void ReleaseDescriptors() {
    std::fflush(stderr);
    if (g_saved_error >= 0) {
        dup2(g_saved_error, 2);
        close(g_saved_error);
        g_saved_error = -1;
    }
}
} // namespace
#else
namespace {
void CaptureDescriptors(FILE*, bool) {}
void ReleaseDescriptors() {}
} // namespace
#endif

void* operator new(size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) {
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, size_t) noexcept {
    std::free(p);
}

namespace {

namespace GT = Common::GpuTrace;
namespace DL = Common::DeviceLost;

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

std::string ReadFile(const std::string& path) {
    std::string text;
    if (FILE* file = std::fopen(path.c_str(), "rb")) {
        char buffer[4096];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            text.append(buffer, read);
        }
        std::fclose(file);
    }
    std::erase(text, '\r');
    return text;
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = std::min(text.find('\n', at), text.size());
        lines.emplace_back(text.substr(at, end - at));
        at = end + 1;
    }
    return lines;
}

std::string LineStartingWith(const std::string& text, const std::string& prefix) {
    for (const std::string& line : Lines(text)) {
        if (line.starts_with(prefix)) {
            return line;
        }
    }
    return {};
}

int RunChild(const std::string& self, const char* argument) {
    std::string command = "\"" + self + "\" " + argument;
#ifdef _WIN32
    command = "\"" + command + "\""; // cmd.exe strips the outer pair
#endif
    std::fflush(stdout);
    int status = std::system(command.c_str());
#ifndef _WIN32
    status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return status;
}

void SetSimulationVariable(const char* value) {
#ifdef _WIN32
    _putenv_s("SOS_SIMULATE_DEVICE_LOST", value);
#else
    if (value[0]) {
        setenv("SOS_SIMULATE_DEVICE_LOST", value, 1);
    } else {
        unsetenv("SOS_SIMULATE_DEVICE_LOST");
    }
#endif
}

constexpr u64 RingMark = 0x5EED0000;

// The newest Capacity events stay, oldest first, with consecutive sequence numbers; older ones are overwritten.
void TestRingWrapsInOrder() {
    for (u64 i = 1; i <= 2000; ++i) {
        GT::Record(GT::Event::Submit, RingMark + i, 0, 0, 0);
    }
    static GT::EventRecord events[GT::Capacity];
    const u32 count = GT::ReadEvents(events, GT::Capacity);
    Check(count == GT::Capacity, "the ring keeps exactly its capacity of the newest events");
    bool consecutive = count > 0;
    for (u32 i = 1; i < count; ++i) {
        consecutive &= events[i].seq == events[i - 1].seq + 1 && events[i].args[0] == events[i - 1].args[0] + 1;
    }
    Check(consecutive, "the events come oldest first with consecutive sequence numbers");
    Check(count > 0 && events[count - 1].args[0] == RingMark + 2000, "the newest event is the last one recorded");
    Check(count > 0 && events[0].args[0] == RingMark + 2000 - GT::Capacity + 1,
          "the oldest kept event is Capacity events back: the ring wrapped over the older ones");
    GT::EventRecord few[8];
    const u32 some = GT::ReadEvents(few, 8);
    Check(some == 8 && few[7].args[0] == RingMark + 2000 && few[0].args[0] == RingMark + 1993,
          "a smaller read gets the newest events, still oldest first");
}

constexpr u64 Mixer = 0x9E3779B97F4A7C15ull;

bool Consistent(const GT::EventRecord& event) {
    return event.event == GT::Event::HeapBudget && event.thread != 0 &&
           event.args[2] == ((event.args[0] * Mixer) ^ event.args[1]) && event.args[3] == ~(event.args[0] ^ event.args[1]);
}

// Writers on many threads wrap the ring many times while it is read: every event read back is one writer's whole event.
void TestConcurrentWritersNeverTear() {
    constexpr u32 Writers = 8;
    constexpr u64 PerWriter = 60000;
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (u32 t = 0; t < Writers; ++t) {
        threads.emplace_back([&go, t] {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (u64 i = 0; i < PerWriter; ++i) {
                GT::Record(GT::Event::HeapBudget, t + 1, i, ((t + 1) * Mixer) ^ i, ~((t + 1) ^ i));
            }
        });
    }
    static GT::EventRecord events[GT::Capacity];
    u64 seen = 0, torn = 0, disordered = 0;
    go.store(true, std::memory_order_release);
    for (int round = 0; round < 400; ++round) {
        const u32 count = GT::ReadEvents(events, GT::Capacity);
        for (u32 i = 0; i < count; ++i) {
            if (events[i].event != GT::Event::HeapBudget) {
                continue; // an older event of another test, not yet overwritten
            }
            ++seen;
            torn += !Consistent(events[i]);
            disordered += i > 0 && events[i].seq <= events[i - 1].seq;
        }
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const u32 count = GT::ReadEvents(events, GT::Capacity);
    u32 final_consistent = 0;
    for (u32 i = 0; i < count; ++i) {
        final_consistent += Consistent(events[i]);
    }
    Check(seen > 0, "the reader saw events while the writers wrapped the ring");
    Check(torn == 0, "no event read during concurrent writes is torn (" + std::to_string(torn) + " of " +
                         std::to_string(seen) + ")");
    Check(disordered == 0, "events read during concurrent writes keep their order");
    Check(count == GT::Capacity && final_consistent == GT::Capacity,
          "after the writers the ring holds Capacity whole events");
}

// The record path is a few relaxed atomic stores: no allocation, whatever is recorded.
void TestRecordingDoesNotAllocate() {
    GT::Record(GT::Event::Submit); // the thread id is cached on first use
    const long long before = g_allocations.load();
    for (u64 i = 0; i < 20000; ++i) {
        GT::Record(GT::Event::BlockingWait, i, i, 0);
        GT::NoteShaders(GT::PipelineKind::Compute, i);
        GT::NoteShaders(GT::PipelineKind::Graphics, i, 0, i);
        GT::NoteDispatch(i, 1, 2, 3);
        GT::SetLastGraphics(i, 0, i);
        (void)GT::ShaderGeneration();
        if (i % 64 == 0) {
            GT::NoteSubmitted(DL::Timeline::Draw, 1000000 + i / 64); // far above the ticks of the dump test
        }
    }
    Check(g_allocations.load() == before, "recording events, shaders, dispatches and submissions allocates nothing");
}

// Draw submissions 100..102 and the one being recorded, with the monitor's draw ticks 102 submitted and 100 completed.
void PrepareSubmissions() {
    auto& monitor = DL::Global();
    // Leaves the ring sets of the previous test behind: three fresh draw submissions push them out.
    GT::NoteShaders(GT::PipelineKind::Compute, 0xaaa1);
    GT::NoteSubmitted(DL::Timeline::Draw, 100);
    const u64 generation = GT::ShaderGeneration();
    GT::NoteShaders(GT::PipelineKind::Compute, 0xbbb1);
    GT::NoteShaders(GT::PipelineKind::Graphics, 0xbbb2, 0, 0xbbb3);
    for (u32 i = 0; i < 40; ++i) {
        GT::NoteDispatch(0x3000 + i, i + 1, 2, 1);
    }
    // A present submission between a draw submission's pipelines and its submit neither keeps nor drops them.
    GT::NoteSubmitted(DL::Timeline::Present, 9000);
    monitor.NoteSubmitted(DL::Timeline::Present, 9000);
    monitor.NoteCompleted(DL::Timeline::Present, 8999);
    Check(GT::ShaderGeneration() == generation, "a present submission does not start a new shader generation");
    GT::NoteSubmitted(DL::Timeline::Draw, 101);
    Check(GT::ShaderGeneration() == generation + 1, "a draw submission starts a new shader generation");
    for (u64 i = 0; i < 300; ++i) {
        GT::NoteShaders(GT::PipelineKind::Compute, 0x11110000 + i);
    }
    for (u64 i = 0; i < 20; ++i) {
        GT::NoteShaders(GT::PipelineKind::Graphics, 0x2000 + i, 0, 0x5000);
    }
    GT::NoteSubmitted(DL::Timeline::Draw, 102);
    GT::NoteShaders(GT::PipelineKind::Compute, 0xccc1);
    GT::NoteDispatch(0xabcd, 8, 4, 1);
    monitor.NoteSubmitted(DL::Timeline::Draw, 102);
    monitor.NoteCompleted(DL::Timeline::Draw, 100);
    GT::SetPendingWork(3, 2);
    GT::SetDeviceIdentity("Test GPU (driver 1.0, Vulkan 1.3)");
    for (u64 tick = 1; tick <= 2000; ++tick) {
        GT::Record(GT::Event::Submit, tick, 1, tick - 1, u64(DL::Timeline::Draw) << 32);
    }
    GT::Record(GT::Event::BlockingWait, 102, 100, u64(DL::Timeline::Draw));
}

std::string DumpText(const std::string& path) {
    std::fflush(stdout);
    g_stream = std::fopen(path.c_str(), "wb");
    if (!g_stream) {
        return {};
    }
    CaptureDescriptors(g_stream, false);
    GT::VulkanFailure("semwait", DL::DeviceLostResult, 102, DL::Timeline::Draw);
    GT::Dump("a second dump is ignored");
    ReleaseDescriptors();
    std::fclose(g_stream);
    g_stream = nullptr;
    return ReadFile(path);
}

void TestDumpIsBoundedAndNamesUnfinishedWork(const std::string& path) {
    PrepareSubmissions();
    const std::string text = DumpText(path);
    std::remove(path.c_str());
    const std::vector<std::string> lines = Lines(text);

    bool prefixed = !lines.empty(), short_lines = true;
    for (const std::string& line : lines) {
        prefixed &= line.starts_with(GT::LinePrefix);
        short_lines &= line.size() <= GT::MaxLineChars;
    }
    Check(prefixed, "every dump line starts with \"GPU trace: \"");
    Check(short_lines, "every dump line is within MaxLineChars");
    Check(lines.size() <= GT::MaxDumpLines, "the dump is within MaxDumpLines (" + std::to_string(lines.size()) + ")");
    Check(text.size() <= size_t(GT::MaxDumpLines) * (GT::MaxLineChars + 1), "the dump is byte-bounded");
    Check(Count(text, "GPU trace: Vulkan device lost, detected on thread ") == 1 && !Has(text, "second dump"),
          "the trace prints once; a later dump prints nothing");
    Check(Has(text, "detected on thread " + std::to_string(GT::CurrentThreadId()) + " ("),
          "the trace names the detecting thread");
    Check(Has(text, "GPU trace: device Test GPU (driver 1.0, Vulkan 1.3)"), "the trace names the device");
    Check(Has(text, "GPU trace: draw timeline: last submitted tick 102, last completed tick 100") &&
              Has(text, "GPU trace: present timeline: last submitted tick 9000, last completed tick 8999") &&
              !Has(text, "flip timeline"),
          "the trace lists each timeline with its own ticks, the idle ones left out");
    Check(Has(text, "3 operations waited for GPU ticks, 2 recorded chunks were queued"), "pending work is reported");

    // Only the draw ticks after the last completed one are unfinished; the completed tick 100 is left out.
    Check(!Has(text, "draw tick 100 was not completed") && !Has(text, "0x0000aaa1"),
          "a completed submission's pipelines are left out");
    Check(Has(text, "GPU trace: draw tick 101 was not completed; it recorded 2 pipelines (1 compute, 1 graphics), "
                    "40 dispatches\n"
                    "GPU trace:   cs 0x0000bbb1\n"
                    "GPU trace:   vs 0x0000bbb2 gs 0x00000000 ps 0x0000bbb3\n"),
          "an unfinished submission names its compute and graphics pipelines");
    Check(Has(text, "GPU trace:   dispatches 9..40 of 40 (oldest first):\n"
                    "GPU trace:     cs 0x00003008 9x2x1 cs 0x00003009 10x2x1 cs 0x0000300a 11x2x1 cs 0x0000300b 12x2x1\n") &&
              Has(text, " cs 0x00003027 40x2x1\n") && !Has(text, "cs 0x00003007 "),
          "the newest 32 dispatches of a submission keep their order and group counts");
    Check(Has(text, "GPU trace: draw tick 102 was not completed; it recorded 320 pipelines (300 compute, 20 graphics)"),
          "the second unfinished submission is listed after the first");
    Check(Has(text, "GPU trace:   cs 0x11110000 0x11110001 0x11110002 0x11110003 0x11110004 0x11110005 0x11110006 "
                    "0x11110007\n") &&
              Has(text, " 0x111100ff\n") && !Has(text, "0x11110100") && Has(text, "and 44 more compute pipelines\n"),
          "every kept compute pipeline is named, eight per line, and the rest is counted");
    Check(Has(text, "vs 0x0000200f gs 0x00000000 ps 0x00005000\n") && !Has(text, "vs 0x00002010") &&
              Has(text, "and 4 more graphics pipelines\n"),
          "graphics pipelines are printed up to GraphicsPrinted and the rest is counted");
    Check(Has(text, "GPU trace: 1 pipelines recorded since the last draw submission, 1 dispatches\n"
                    "GPU trace:   cs 0x0000ccc1\n"),
          "the pipelines of the submission being recorded are listed");
    Check(Has(text, "GPU trace: summary: Vulkan device lost; last event: DEVICE LOST at semwait, result -4, draw tick "
                    "102; unfinished draw tick 101 (2 unfinished), 2 pipelines: cs 0x0000bbb1, vs 0x0000bbb2 ps "
                    "0x0000bbb3"),
          "the summary names the last event and the oldest unfinished submission");
    Check(Count(text, " submit tick ") <= GT::Capacity && Has(text, "draw submit tick 2000, 1 waits (first 1999), result 0") &&
              Has(text, "blocking wait for draw tick 102 (completed 100)"),
          "the newest events are printed and no more than the ring holds");
    Check(Has(text, "GPU trace: thread " + std::to_string(GT::CurrentThreadId()) + " is "),
          "the threads of the printed events are named");
    Check(Has(text, "GPU trace: end ("), "the trace closes with its end line");

    // The Bug Report reads the summary from this very output.
    const std::string log = "GPU device lost: first detected in vkWaitSemaphores (site semwait), VkResult "
                            "VK_ERROR_DEVICE_LOST (-4), thread 7, draw tick 102, last submitted draw tick 102\n" +
                            text + "Device fault: unavailable (no VK_EXT_device_fault)\n"
                                   "Runtime: GPU device lost: first detected in vkWaitSemaphores; the process ends with "
                                   "status 25\n";
    const sos::LogDiagnostics diag = sos::AnalyzeLogDiagnostics(log);
    const std::string summary = LineStartingWith(text, GT::SummaryPrefix).substr(std::string(GT::SummaryPrefix).size());
    Check(diag.device_lost && diag.crash_kind == sos::CrashKind::kDeviceLost, "the report still sees the device loss");
    Check(!summary.empty() && summary.starts_with(diag.gpu_trace_summary) && diag.gpu_trace_summary.size() >= 150,
          "the Bug Report extracts the trace summary line of the real dump");
    Check(diag.device_fault_summary.starts_with("GPU trace: Vulkan device lost; last event: DEVICE LOST at semwait") &&
              diag.device_fault_summary.size() <= 512,
          "the device-lost fault details carry the bounded trace summary");
    Check(diag.device_lost_reason.starts_with("GPU device lost: first detected in vkWaitSemaphores"),
          "the trace never replaces the device-lost root cause");
}

// SOS_SIMULATE_DEVICE_LOST through the production monitor: the root report, then the trace, then the fault and the
// process end with status 25.
int DeviceLostChild(const std::string& path) {
    g_stream = std::fopen(path.c_str(), "wb");
    if (!g_stream) {
        return 2;
    }
    CaptureDescriptors(g_stream, true); // the child ends here; nothing is restored
    DL::ArmSimulationFromEnvironment();
    auto& monitor = DL::Global();
    GT::NoteShaders(GT::PipelineKind::Compute, 0x5151);
    GT::NoteDispatch(0x5151, 4, 4, 1);
    const auto submit = [&](u64 tick) {
        return DL::CallWhileHealthy(monitor, DL::Operation::QueueSubmit, "submit", tick, DL::Timeline::Draw, [&] {
            GT::Record(GT::Event::Submit, tick, 0, 0, u64(DL::Timeline::Draw) << 32);
            return DL::Success;
        });
    };
    if (submit(5) != DL::Success) {
        return 3;
    }
    monitor.NoteSubmitted(DL::Timeline::Draw, 5);
    GT::NoteSubmitted(DL::Timeline::Draw, 5);
    monitor.NoteCompleted(DL::Timeline::Draw, 4);
    GT::NoteShaders(GT::PipelineKind::Graphics, 0x6161, 0, 0x6262);
    submit(6); // the second submission is simulated as lost: the monitor ends the process
    return 4;
}

void TestDeviceLostPrintsTheTrace(const std::string& self) {
    const std::string path = self + ".device_lost.txt";
    SetSimulationVariable("submit:2");
    const int status = RunChild(self, "--device-lost-child");
    SetSimulationVariable("");
    const std::string text = ReadFile(path);
    std::remove(path.c_str());
    Check(status == DL::ExitStatus, "a simulated device loss ends the process with status 25 (" + std::to_string(status) + ")");
    const size_t root = text.find("GPU device lost: first detected in vkQueueSubmit (site submit)");
    const size_t trace = text.find("GPU trace: Vulkan device lost (simulated by SOS_SIMULATE_DEVICE_LOST), detected on thread ");
    const size_t fault = text.find("Device fault: not queried (simulated device loss)");
    const size_t end = text.find("Runtime: GPU device lost: first detected in vkQueueSubmit");
    Check(root != std::string::npos && trace != std::string::npos && root < trace,
          "the device-lost monitor prints the GPU trace after its root report");
    Check(trace != std::string::npos && fault != std::string::npos && end != std::string::npos && trace < fault &&
              fault < end && Has(text, "ends with status 25"),
          "the trace comes before the fault report and the process end");
    Check(Count(text, "GPU trace: summary: ") == 1, "the device loss prints one trace");
    Check(Has(text, "GPU trace: draw tick 5 was not completed; it recorded 1 pipelines (1 compute, 0 graphics), 1 dispatches\n"
                    "GPU trace:   cs 0x00005151\n"),
          "the trace of a real device loss names the unfinished submission");
    Check(Has(text, "GPU trace: 1 pipelines recorded since the last draw submission, 0 dispatches\n"
                    "GPU trace:   vs 0x00006161 gs 0x00000000 ps 0x00006262\n"),
          "the trace names the pipelines of the submission that met the loss");
    Check(Has(text, "DEVICE LOST at submit, result -4, draw tick 6"), "the root cause is the last trace event");
    // The root report and the trace name the same thread.
    const std::string root_line = LineStartingWith(text, "GPU device lost: first detected in ");
    const size_t thread_at = root_line.find(", thread ");
    const std::string thread =
        thread_at == std::string::npos ? "?" : root_line.substr(thread_at + 9, root_line.find(',', thread_at + 9) - thread_at - 9);
    Check(Has(text, "detected on thread " + thread + " ("), "the root report and the trace name the same thread");
}

int AssertChild(const std::string& path) {
    g_stream = std::fopen(path.c_str(), "wb");
    if (!g_stream) {
        return 2;
    }
    CaptureDescriptors(g_stream, true); // the child ends here; nothing is restored
    GT::NoteShaders(GT::PipelineKind::Compute, 0x7171);
    GT::Record(GT::Event::HeapBudget, 1, 3ull << 30, 2ull << 30, 1);
    ASSERT_MSG(false, "gpu crash trace test assertion");
    return 3;
}

void TestAssertionPrintsTheTrace(const std::string& self) {
    const std::string path = self + ".assert.txt";
    const int status = RunChild(self, "--assert-child");
    const std::string text = ReadFile(path);
    std::remove(path.c_str());
    Check(status == 23, "a GPU assertion ends with status 23 (" + std::to_string(status) + ")");
    const size_t trace = text.find("GPU trace: GPU assertion failed, detected on thread ");
    const size_t stop = text.find("STOP: GPU library assertion failed");
    Check(trace != std::string::npos && stop != std::string::npos && trace < stop,
          "a GPU assertion prints the trace before its stop line");
    Check(Has(text, "GPU trace: summary: GPU assertion failed; last event: heap 1 went over its budget: 3072 MiB used of "
                    "2048 MiB budget; no unfinished draw tick, 1 pipelines since the last submission: cs 0x00007171"),
          "the assertion's trace summary names the last event and the recorded pipelines");
    const sos::LogDiagnostics diag = sos::AnalyzeLogDiagnostics(text);
    Check(diag.gpu_trace_summary.starts_with("GPU assertion failed; last event: heap 1") && !diag.device_lost &&
              diag.device_fault_summary.empty(),
          "the Bug Report reads an assertion's trace summary without calling it a device loss");
}

} // namespace

int main(int argc, char** argv) {
    const std::string self = argc > 0 ? argv[0] : "gpu_crash_trace_tests";
    if (argc == 2 && std::string{argv[1]} == "--device-lost-child") {
        return DeviceLostChild(self + ".device_lost.txt");
    }
    if (argc == 2 && std::string{argv[1]} == "--assert-child") {
        return AssertChild(self + ".assert.txt");
    }
    TestRingWrapsInOrder();
    TestConcurrentWritersNeverTear();
    TestRecordingDoesNotAllocate();
    TestDeviceLostPrintsTheTrace(self);
    TestAssertionPrintsTheTrace(self);
    TestDumpIsBoundedAndNamesUnfinishedWork(self + ".trace.txt");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
