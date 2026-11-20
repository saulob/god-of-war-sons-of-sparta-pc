// Headless proof of the graceful device-lost path on the production monitor and the production semaphore, call and wait
// templates, with a fake driver: the first VK_ERROR_DEVICE_LOST owns the root cause, nothing reaches a lost device,
// waits leave at once, the driver's fault is asked once and bounded, and every Vulkan call site of the renderer goes
// through the guard. The same checks run against mutated copies of the sources from tools/test_device_lost.ps1.
// No window, GPU, launcher or game.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "common/device_lost.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

extern "C" FILE* sos_stderr_stream(void) {
    return stdout;
}

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

namespace fs = std::filesystem;
namespace DL = Common::DeviceLost;

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

std::string Source(const char* path) {
    std::ifstream in(fs::path(SOS_STRING(SOS_SOURCE_DIR)) / path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::erase(text, '\r');
    return text;
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

struct Recorder {
    std::vector<std::string> lines;
    std::atomic<int> shutdowns{0};
    std::atomic<int> parks{0};
    std::atomic<int> fault_queries{0};
    std::string reason;
    int status = 0;
    DL::FaultInfo fault_to_report;
};

Recorder g_rec;

void Print(const char* line) {
    g_rec.lines.emplace_back(line);
}
void Shutdown(const char* summary, int status) {
    g_rec.reason = summary;
    g_rec.status = status;
    ++g_rec.shutdowns;
}
void Park() {
    ++g_rec.parks;
}
void QueryFault(DL::FaultInfo& info) {
    ++g_rec.fault_queries;
    info = g_rec.fault_to_report;
}

void ResetRecorder() {
    g_rec.lines.clear();
    g_rec.shutdowns = 0;
    g_rec.parks = 0;
    g_rec.fault_queries = 0;
    g_rec.reason.clear();
    g_rec.status = 0;
    g_rec.fault_to_report = DL::FaultInfo{};
}

DL::Hooks TestHooks() {
    return DL::Hooks{.print = Print, .query_fault = QueryFault, .shutdown = Shutdown, .park = Park};
}

struct Driver {
    int submits = 0;
    int sparse = 0;
    int counter_reads = 0;
    int semaphore_waits = 0;
    int fence_waits = 0;
    int fence_timeouts_left = 0;
    s32 next_result = DL::Success;
    u64 counter = 0;
};

constexpr s32 OutOfDeviceMemory = -2;
constexpr s32 OutOfHostMemory = -1;

void TestHealthyCallsRunUnchanged() {
    ResetRecorder();
    DL::Monitor monitor{TestHooks()};
    Driver driver;
    const s32 result = DL::CallWhileHealthy(monitor, DL::Operation::QueueSubmit, "submit", 3, [&] {
        ++driver.submits;
        return vk::Result::eSuccess;
    });
    Check(result == DL::Success && driver.submits == 1, "healthy submission executes once and succeeds");
    Check(!monitor.IsLost() && g_rec.lines.empty() && g_rec.fault_queries == 0 && g_rec.shutdowns == 0,
          "a healthy call prints nothing, asks no fault and ends nothing");
    Check(std::atomic<DL::Phase>::is_always_lock_free, "the healthy-path check is a lock-free atomic read");
}

void TestFirstFaultWins() {
    ResetRecorder();
    DL::Monitor monitor{TestHooks()};
    Driver driver;
    const s32 first = DL::CallWhileHealthy(monitor, DL::Operation::QueueSubmit, "submit", 41, [&] {
        ++driver.submits;
        return vk::Result::eErrorDeviceLost;
    });
    Check(first == DL::DeviceLostResult && monitor.IsLost(), "the first loss is reported");
    Check(monitor.CurrentPhase() == DL::Phase::Reported, "the report is complete before it returns");
    const auto& root = monitor.Root();
    Check(root.operation == DL::Operation::QueueSubmit && std::strcmp(root.site, "submit") == 0 &&
              root.result == DL::DeviceLostResult && root.tick == 41,
          "the root cause names the operation, site, result and tick");
    Check(root.thread_id == DL::CurrentThreadId(), "the root cause names the thread");

    const bool second_owner = monitor.Report(DL::Operation::QueuePresent, "present", DL::DeviceLostResult, 99);
    Check(!second_owner, "a second fault does not become the owner");
    Check(monitor.Root().operation == DL::Operation::QueueSubmit && std::strcmp(monitor.Root().site, "submit") == 0 &&
              monitor.Root().tick == 41,
          "a second fault cannot replace the first");
    Check(Contains(monitor.Summary(), "vkQueueSubmit") && !Contains(monitor.Summary(), "vkQueuePresentKHR"),
          "the summary keeps the first operation");
    Check(g_rec.shutdowns == 1 && g_rec.status == DL::ExitStatus && g_rec.reason == monitor.Summary(),
          "one shutdown with the summary as its reason and the device-lost status");
}

void TestPostLossSuppression() {
    ResetRecorder();
    DL::Monitor monitor{TestHooks()};
    Driver driver;
    monitor.Report(DL::Operation::QueueSubmit, "submit", DL::DeviceLostResult, 5);

    const s32 submit = DL::CallWhileHealthy(monitor, DL::Operation::QueueSubmit, "submit", 6, [&] {
        ++driver.submits;
        return vk::Result::eSuccess;
    });
    Check(submit == DL::DeviceLostResult && driver.submits == 0, "a submission after the loss is rejected");
    const s32 sparse = DL::CallWhileHealthy(monitor, DL::Operation::QueueBindSparse, "sparse", 7, [&] {
        ++driver.sparse;
        return vk::Result::eSuccess;
    });
    Check(sparse == DL::DeviceLostResult && driver.sparse == 0, "a sparse bind after the loss is rejected");

    Vulkan::TimelineTicks ticks{};
    ticks.gpu_tick = 5;
    std::vector<std::string> failed;
    const auto read = [&](u64& counter) {
        ++driver.counter_reads;
        counter = 9;
        return vk::Result::eSuccess;
    };
    const auto fail = [&](const char* site, s32 result, u64) { failed.push_back(site + std::to_string(result)); };
    const bool refreshed = Vulkan::RefreshKnownTick(ticks, monitor, read, fail);
    Check(!refreshed && driver.counter_reads == 0 && failed.size() == 1 && failed[0] == "semread-4",
          "a semaphore read after the loss returns the terminal result without a driver call");
    Check(ticks.gpu_tick.load() == 5, "nothing counts as completed on a lost device");

    const auto wait = [&] {
        ++driver.semaphore_waits;
        return driver.semaphore_waits < 1000 ? vk::Result::eTimeout : vk::Result::eSuccess;
    };
    failed.clear();
    Vulkan::WaitForTick(ticks, monitor, 10, wait, read, fail);
    Check(driver.semaphore_waits == 0 && driver.counter_reads == 0 && failed.size() == 1,
          "a semaphore wait after the loss leaves at once without blocking or a driver call");
    Check(ticks.gpu_tick.load() == 5, "a wait after the loss does not advance the known tick");

    const s32 fence = DL::WaitWhileHealthy(monitor, DL::Operation::WaitForFences, "frmfence", 0, [&] {
        ++driver.fence_waits;
        return driver.fence_waits < 1000 ? vk::Result::eTimeout : vk::Result::eSuccess;
    });
    Check(fence == DL::DeviceLostResult && driver.fence_waits == 0, "a fence wait after the loss leaves at once");

    const s32 acquire = DL::CallWhileHealthy(monitor, DL::Operation::AcquireNextImage, "acquire", 0,
                                             [&] { return vk::Result::eSuccess; });
    const s32 present = DL::CallWhileHealthy(monitor, DL::Operation::QueuePresent, "present", 0,
                                             [&] { return vk::Result::eSuccess; });
    Check(acquire == DL::DeviceLostResult && present == DL::DeviceLostResult, "acquire and present are rejected");
    Check(monitor.Root().operation == DL::Operation::QueueSubmit && g_rec.shutdowns == 1 && g_rec.fault_queries == 1,
          "the consequences change neither the root cause nor the shutdown nor the fault query");
}

struct StopToken {
    bool requested;
    bool stop_requested() const {
        return requested;
    }
};

void TestWorkersObserveTheLoss() {
    ResetRecorder();
    DL::Monitor monitor{TestHooks()};
    Check(!DL::ShouldStop(monitor, StopToken{false}), "a healthy worker keeps processing");
    Check(DL::ShouldStop(monitor, StopToken{true}), "a stop request still stops a worker");
    monitor.Report(DL::Operation::WaitForFences, "prsfence", DL::DeviceLostResult, 0);
    Check(DL::ShouldStop(monitor, StopToken{false}), "the worker stop predicate observes the device loss");
}

void TestLossSeenByAnotherThreadEndsAWait() {
    ResetRecorder();
    DL::Monitor monitor{TestHooks()};
    Driver driver;
    driver.fence_timeouts_left = 3;
    const s32 result = DL::WaitWhileHealthy(monitor, DL::Operation::WaitForFences, "prsfence", 0, [&] {
        ++driver.fence_waits;
        if (driver.fence_timeouts_left-- == 2) {
            monitor.Report(DL::Operation::QueueSubmit, "submit", DL::DeviceLostResult, 12);
        }
        return vk::Result::eTimeout;
    });
    Check(result == DL::DeviceLostResult && driver.fence_waits == 2,
          "a wait that keeps timing out ends within one slice of another thread's report");

    ResetRecorder();
    DL::Monitor healthy{TestHooks()};
    Driver slow;
    slow.fence_timeouts_left = 4;
    const s32 waited = DL::WaitWhileHealthy(healthy, DL::Operation::WaitForFences, "frmfence", 0, [&] {
        ++slow.fence_waits;
        return slow.fence_timeouts_left-- > 0 ? vk::Result::eTimeout : vk::Result::eSuccess;
    });
    Check(waited == DL::Success && slow.fence_waits == 5 && !healthy.IsLost(),
          "a healthy wait repeats its slices until the fence signals");
}

void TestHealthyWaitAndRead() {
    ResetRecorder();
    DL::Monitor monitor{TestHooks()};
    Driver driver;
    Vulkan::TimelineTicks ticks{};
    ticks.gpu_tick = 2;
    ticks.current_tick = 20;
    driver.counter = 8;
    const auto read = [&](u64& counter) {
        ++driver.counter_reads;
        counter = driver.counter;
        return vk::Result::eSuccess;
    };
    const auto fail = [&](const char*, s32, u64) { ++g_failures; };
    Vulkan::RefreshKnownTick(ticks, monitor, read, fail);
    Check(ticks.gpu_tick.load() == 8 && driver.counter_reads == 1, "a healthy read advances the known tick");
    Vulkan::WaitForTick(ticks, monitor, 6, [&] { return vk::Result::eSuccess; }, read, fail);
    Check(driver.counter_reads == 1, "a tick already reached needs no wait and no read");
    int slices = 0;
    Vulkan::WaitForTick(ticks, monitor, 12, [&] {
        ++slices;
        driver.counter = 12;
        return slices < 3 ? vk::Result::eTimeout : vk::Result::eSuccess;
    }, read, fail);
    Check(slices == 3 && ticks.gpu_tick.load() == 12, "a healthy wait reaches the tick and refreshes it");
}

void TestFaultQueryRunsOnce() {
    ResetRecorder();
    g_rec.fault_to_report.status = DL::FaultStatus::Captured;
    std::strcpy(g_rec.fault_to_report.description, "page fault");
    DL::Monitor monitor{TestHooks()};
    monitor.Report(DL::Operation::QueueSubmit, "submit", DL::DeviceLostResult, 1);
    monitor.Report(DL::Operation::QueuePresent, "present", DL::DeviceLostResult, 2);
    monitor.Observe("vkQueueSubmit");
    DL::CallWhileHealthy(monitor, DL::Operation::QueueSubmit, "submit", 3, [] { return vk::Result::eSuccess; });
    Check(g_rec.fault_queries == 1 && monitor.FaultQueries() == 1, "VK_EXT_device_fault is asked exactly once");
    Check(monitor.Fault().status == DL::FaultStatus::Captured && Contains(monitor.Summary(), "device fault: page fault"),
          "a captured fault reaches the summary");
}

void TestFaultStatuses() {
    {
        ResetRecorder();
        g_rec.fault_to_report.status = DL::FaultStatus::Unsupported;
        DL::Monitor monitor{TestHooks()};
        monitor.Report(DL::Operation::QueueSubmit, "submit", DL::DeviceLostResult, 1);
        Check(Contains(monitor.Summary(), "VK_EXT_device_fault unavailable") && g_rec.shutdowns == 1,
              "an unsupported VK_EXT_device_fault is recorded and the run still ends cleanly");
    }
    {
        ResetRecorder();
        g_rec.fault_to_report.status = DL::FaultStatus::Failed;
        g_rec.fault_to_report.result = -5;
        DL::Monitor monitor{TestHooks()};
        monitor.Report(DL::Operation::QueueSubmit, "submit", DL::DeviceLostResult, 1);
        Check(Contains(monitor.Summary(), "query failed (result -5)") && g_rec.shutdowns == 1,
              "a failed fault query records its result and the run still ends cleanly");
    }
    {
        ResetRecorder();
        DL::Hooks hooks = TestHooks();
        hooks.query_fault = nullptr;
        DL::Monitor monitor{hooks};
        monitor.Report(DL::Operation::QueueSubmit, "submit", DL::DeviceLostResult, 1);
        Check(monitor.Fault().status == DL::FaultStatus::NotQueried && g_rec.shutdowns == 1,
              "no registered device is recorded as not queried, not as another failure");
    }
}

void TestFaultStorageIsBounded() {
    ResetRecorder();
    auto& fault = g_rec.fault_to_report;
    fault.status = DL::FaultStatus::Captured;
    fault.address_count = 100000;
    fault.vendor_count = 100000;
    fault.vendor_binary_size = 1ull << 30;
    std::memset(fault.description, 'x', sizeof(fault.description) - 1);
    for (u32 i = 0; i < DL::MaxFaultAddresses; ++i) {
        fault.addresses[i] = {1, 0x1000u + i, 0x10};
    }
    for (u32 i = 0; i < DL::MaxVendorFaults; ++i) {
        fault.vendors[i] = {0x20u + i, 1, "vendor"};
    }
    DL::Monitor monitor{TestHooks()};
    monitor.Report(DL::Operation::QueueSubmit, "submit", DL::DeviceLostResult, 1);
    Check(sizeof(DL::FaultInfo::addresses) / sizeof(DL::FaultInfo::Address) == DL::MaxFaultAddresses &&
              sizeof(DL::FaultInfo::vendors) / sizeof(DL::FaultInfo::Vendor) == DL::MaxVendorFaults,
          "address and vendor records have fixed storage");
    Check(g_rec.lines.size() <= 8, "a large driver report prints a bounded number of lines");
    Check(std::strlen(monitor.Summary()) < 1024 && std::strlen(monitor.RootLine()) < 320, "the summary is bounded");

    char text[16];
    DL::CopyDriverText(text, sizeof(text), "a\nb\tc\x01" "dddddddddddddddddddd", 40);
    Check(std::strlen(text) == 15 && !std::strchr(text, '\n') && !std::strchr(text, '\t'),
          "driver text is cut to its buffer and loses control characters");
    DL::CopyDriverText(text, sizeof(text), "short", 5);
    Check(std::strcmp(text, "short") == 0, "short driver text is kept");
}

void TestOtherErrorsStayDistinct() {
    ResetRecorder();
    DL::Monitor monitor{TestHooks()};
    Driver driver;
    for (const s32 code : {OutOfDeviceMemory, OutOfHostMemory, -12, -1000001004}) {
        const s32 result = DL::CallWhileHealthy(monitor, DL::Operation::QueueSubmit, "submit", 1, [&] {
            ++driver.submits;
            return vk::Result(code);
        });
        Check(result == code && !monitor.IsLost(), "a Vulkan error other than device loss stays the caller's");
    }
    Check(driver.submits == 4 && g_rec.fault_queries == 0 && g_rec.shutdowns == 0 && g_rec.lines.empty(),
          "other errors report nothing and query no fault");
    const s32 timeout = DL::CallWhileHealthy(monitor, DL::Operation::QueueSubmit, "submit", 1,
                                             [] { return vk::Result::eTimeout; });
    Check(timeout == DL::Timeout && !monitor.IsLost(), "a timeout is not a loss");
}

void TestSecondaryLinesAreBounded() {
    ResetRecorder();
    DL::Monitor monitor{TestHooks()};
    monitor.Report(DL::Operation::QueueSubmit, "submit", DL::DeviceLostResult, 1);
    const size_t before = g_rec.lines.size();
    for (int i = 0; i < 200; ++i) {
        monitor.Observe("vkQueueSubmit");
    }
    Check(g_rec.lines.size() - before <= DL::MaxSecondaryLines + 1, "hundreds of consequences print a handful of lines");
    Check(monitor.SecondaryReports() == 200, "every consequence is still counted");
}

void TestRaceHasOneOwner() {
    ResetRecorder();
    DL::Monitor monitor{TestHooks()};
    std::atomic<int> owners{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            if (monitor.Report(DL::Operation(i % 3), "race", DL::DeviceLostResult, u64(i))) {
                ++owners;
            }
        });
    }
    go = true;
    for (auto& thread : threads) {
        thread.join();
    }
    Check(owners == 1, "exactly one of eight racing threads becomes the owner");
    Check(g_rec.fault_queries == 1 && g_rec.shutdowns == 1, "a race asks the fault once and ends once");
    Check(monitor.Root().tick < 8 && u64(monitor.Root().operation) == monitor.Root().tick % 3 &&
              std::strcmp(monitor.Root().site, "race") == 0,
          "the root cause is one thread's report, not a mix");
}

void TestSimulationSeam() {
    DL::Operation operation{};
    u64 calls = 0;
    Check(DL::ParseSimulation("submit:600", operation, calls) && operation == DL::Operation::QueueSubmit && calls == 600,
          "SOS_SIMULATE_DEVICE_LOST parses an operation and a call count");
    Check(DL::ParseSimulation("fence", operation, calls) && operation == DL::Operation::WaitForFences && calls == 1,
          "the count defaults to the first call");
    Check(!DL::ParseSimulation("submit:0", operation, calls) && !DL::ParseSimulation("bogus", operation, calls) &&
              !DL::ParseSimulation("submit:x", operation, calls) && !DL::ParseSimulation("", operation, calls),
          "a bad simulation value arms nothing");

    ResetRecorder();
    DL::Monitor monitor{TestHooks()};
    Driver driver;
    monitor.ArmSimulation(DL::Operation::QueueSubmit, 3);
    for (int i = 0; i < 2; ++i) {
        DL::CallWhileHealthy(monitor, DL::Operation::QueueSubmit, "submit", 1, [&] {
            ++driver.submits;
            return vk::Result::eSuccess;
        });
    }
    DL::CallWhileHealthy(monitor, DL::Operation::QueuePresent, "present", 1, [&] { return vk::Result::eSuccess; });
    Check(!monitor.IsLost() && driver.submits == 2, "a simulation waits for its call and operation");
    const s32 third = DL::CallWhileHealthy(monitor, DL::Operation::QueueSubmit, "submit", 1, [&] {
        ++driver.submits;
        return vk::Result::eSuccess;
    });
    Check(third == DL::DeviceLostResult && driver.submits == 2, "the simulated call reports a loss without the driver");
    Check(monitor.Fault().status == DL::FaultStatus::Skipped && g_rec.fault_queries == 0 &&
              Contains(monitor.RootLine(), "simulated"),
          "a simulated loss asks no driver and is marked as simulated");
}

void TestRootLineIsConcise() {
    ResetRecorder();
    DL::Monitor monitor{TestHooks()};
    monitor.NoteSubmitted(77);
    monitor.Report(DL::Operation::QueueSubmit, "submit", DL::DeviceLostResult, 78);
    const std::string line = monitor.RootLine();
    Check(Contains(line, "GPU device lost") && Contains(line, "vkQueueSubmit") && Contains(line, "site submit") &&
              Contains(line, "VK_ERROR_DEVICE_LOST") && Contains(line, "tick 78") && Contains(line, "last submitted tick 77"),
          "the root line names the operation, site, result, ticks and thread");
    Check(g_rec.lines.size() <= 3, "a loss without driver fault text prints at most three lines");
}

struct Site {
    const char* path;
    const char* call;
    const char* guard;
};

bool SiteGuarded(const std::string& text, const std::string& call, const std::string& guard) {
    size_t at = 0;
    bool any = false;
    while ((at = text.find(call, at)) != std::string::npos) {
        any = true;
        const size_t begin = at > 700 ? at - 700 : 0;
        if (text.substr(begin, at - begin).find(guard) == std::string::npos) {
            return false;
        }
        at += call.size();
    }
    return any;
}

std::vector<std::string> UnguardedSites(const std::vector<std::pair<const Site*, std::string>>& sources) {
    std::vector<std::string> bad;
    for (const auto& [site, text] : sources) {
        if (!SiteGuarded(text, site->call, site->guard)) {
            bad.push_back(std::string(site->path) + " " + site->call);
        }
    }
    return bad;
}

constexpr Site Sites[] = {
    {"gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp", "GetGraphicsQueue().submit(", "Operation::QueueSubmit"},
    {"gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp", "GetGraphicsQueue().bindSparse(", "Operation::QueueBindSparse"},
    {"gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp", "presentKHR(", "Operation::QueuePresent"},
    {"gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp", "acquireNextImageKHR(", "Operation::AcquireNextImage"},
    {"gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp", "waitIdle(", "Operation::DeviceWaitIdle"},
    {"gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp", "waitForFences(", "Operation::WaitForFences"},
    {"gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp", "waitSemaphores(", "WaitForTick("},
    {"gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp", "ReadCounter(counter); }", "RefreshKnownTick("},
    {"gpu/shadps4/video_core/renderer_vulkan/vk_gpu_profiler.cpp", "getQueryPoolResults(", "Operation::QueryPoolResults"},
};

void TestEveryCallSiteIsGuarded() {
    std::vector<std::pair<const Site*, std::string>> sources;
    for (const Site& site : Sites) {
        sources.emplace_back(&site, Source(site.path));
    }
    const auto bad = UnguardedSites(sources);
    for (const auto& entry : bad) {
        std::printf("  unguarded: %s\n", entry.c_str());
    }
    Check(bad.empty(), "every submit, sparse bind, present, acquire, idle, fence, semaphore and query call is guarded");

    for (auto& [site, text] : sources) {
        const std::string raw = std::string(site->call);
        const auto call_at = text.find(raw);
        if (call_at == std::string::npos) {
            continue;
        }
        std::string mutated = text;
        mutated.replace(0, call_at, std::string(call_at, ' '));
        std::vector<std::pair<const Site*, std::string>> one{{site, mutated}};
        Check(!UnguardedSites(one).empty(),
              std::string("the call-site gate fails when the guard is removed before ") + site->call);
    }

    const std::string raw_files[] = {"gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp",
                                     "gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp",
                                     "gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp"};
    for (const auto& path : raw_files) {
        Check(!Contains(Source(path.c_str()), "ReportDeviceFault"), path + " no longer queries the fault per site");
    }
}

void TestFailurePathsRouteThroughTheOwner() {
    const std::string assert_cpp = Source("gpu/shim/common/assert.cpp");
    const auto lost = assert_cpp.find("DeviceLost::IsLost()");
    const auto exit = assert_cpp.find("_Exit(23)");
    Check(lost != std::string::npos && exit != std::string::npos && lost < exit &&
              Contains(assert_cpp, "Global().Observe("),
          "a GPU assertion after the loss is a consequence of the first report, not a new crash reason");

    const std::string platform = Source("gpu/shadps4/video_core/renderer_vulkan/vk_platform.h");
    const auto helper = platform.find("ReportIfDeviceLost(vk::Result r)");
    const auto first_check = platform.find("static void Check(vk::Result r)");
    const auto second_check = platform.find("static T Check(vk::ResultValue<T> r)");
    Check(helper != std::string::npos && first_check != std::string::npos && second_check != std::string::npos &&
              Contains(platform.substr(first_check, 120), "ReportIfDeviceLost(r);") &&
              Contains(platform.substr(second_check, 120), "ReportIfDeviceLost(r.result);"),
          "Vulkan::Check routes VK_ERROR_DEVICE_LOST to the owner before its assertion");

    const std::string scheduler = Source("gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp");
    const auto function = scheduler.find("void Scheduler::SubmitExecution(SubmitInfo& info) {");
    const auto observe = scheduler.find("Observe(\"vkQueueSubmit\")", function);
    const auto tick = scheduler.find("work_semaphore.NextTick()", function);
    Check(function != std::string::npos && observe != std::string::npos && tick != std::string::npos && observe < tick,
          "a submission after the loss stops before it takes a tick");

    const std::string liverpool = Source("gpu/shadps4/video_core/amdgpu/liverpool.cpp");
    Check(Contains(liverpool, "ShouldStop(device_lost, stoken)") && Contains(liverpool, "&& !device_lost.IsLost()"),
          "the command processor loop stops on the loss, outside and inside its processing loop");

    const std::string instance = Source("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    const auto query = instance.find("void Instance::QueryDeviceFault(");
    const auto body = query == std::string::npos ? std::string{} : instance.substr(query, 2600);
    Check(Contains(body, "std::array<vk::DeviceFaultAddressInfoEXT, MaxFaultAddresses>") &&
              Contains(body, "std::array<vk::DeviceFaultVendorInfoEXT, MaxVendorFaults>") &&
              Contains(body, "std::min<u32>(counts.addressInfoCount, MaxFaultAddresses)") &&
              Contains(body, "std::min<u32>(counts.vendorInfoCount, MaxVendorFaults)") && !Contains(body, "std::vector"),
          "the production fault query has bounded storage and no driver-sized allocation");
    Check(Contains(instance, "SetFaultQuery(&QueryFaultOfRegisteredInstance)") &&
              Contains(instance, "RegisterFaultQuery(this);"),
          "the instance registers its fault query with the monitor");

    const std::string semaphore = Source("gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.h");
    Check(Contains(semaphore, "CallWhileHealthy(monitor, Operation::GetSemaphoreCounterValue") &&
              Contains(semaphore, "WaitWhileHealthy(monitor, Operation::WaitSemaphores"),
          "the timeline semaphore read and wait are guarded in the shared templates");

    const std::string header = Source("gpu/shim/common/device_lost.h") + Source("gpu/shim/common/device_lost.cpp");
    Check(!Contains(header, "mutex") && !Contains(header, "vulkan") && !Contains(header, "vk::"),
          "the monitor holds no lock and never calls Vulkan itself");
}

} // namespace

int main() {
    TestHealthyCallsRunUnchanged();
    TestFirstFaultWins();
    TestPostLossSuppression();
    TestWorkersObserveTheLoss();
    TestLossSeenByAnotherThreadEndsAWait();
    TestHealthyWaitAndRead();
    TestFaultQueryRunsOnce();
    TestFaultStatuses();
    TestFaultStorageIsBounded();
    TestOtherErrorsStayDistinct();
    TestSecondaryLinesAreBounded();
    TestRaceHasOneOwner();
    TestSimulationSeam();
    TestRootLineIsConcise();
    TestEveryCallSiteIsGuarded();
    TestFailurePathsRouteThroughTheOwner();
    std::printf("device lost tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
