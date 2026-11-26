// Persistent Vulkan pipeline cache rules on the production headers (pipeline_cache_policy.h, cache_files.h,
// cache_request_queue.h, common/serdes.h, vk_warmup_inbox.h): blob seal against truncated, zero, bit-flipped and
// trailing-garbage blobs; serdes bounds; atomic writes that fail; orphan temporary files; the bounded IO queue (exact
// counts, bounded drain, Submit after Close, a throwing task, a full queue); one store per game build (1.00 and 1.01) and
// the take-over of the old single-folder store; the build gate that keeps game builds ahead of warm-up builds; warm-up
// failure modes. Plus source contracts of the wiring (cache_storage.cpp, vk_pipeline_*.cpp, dcpgpu, loader.c).
// Every file it writes is in a scratch folder under the system temp folder: never the user's cache or the game.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/serdes.h"
#include "video_core/cache_files.h"
#include "video_core/cache_request_queue.h"
#include "video_core/pipeline_cache_policy.h"
#include "video_core/renderer_vulkan/vk_warmup_inbox.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

namespace {

std::atomic<int> g_checks{0}; // CHECK runs on test threads too
std::atomic<int> g_failures{0};

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            ++g_failures;                                                                          \
            std::printf("  [fail] line %d: %s\n", __LINE__, #condition);                           \
        }                                                                                          \
    } while (0)

using namespace PipelineCachePolicy;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

constexpr const char* Serial = "CUSA57767";
constexpr const char* V100 = "1.00";
constexpr const char* V101 = "1.01";

/// A scratch folder of this process under the system temp folder, removed afterwards.
struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) {
        static std::atomic<int> sequence{0};
        path = fs::temp_directory_path() /
               ("sos-pipeline-cache-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(sequence++) + "-" + name);
        std::error_code error;
        fs::remove_all(path, error);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

Bytes Pattern(std::size_t size, std::uint8_t seed = 1) {
    Bytes bytes(size);
    std::uint32_t state = 0x9e3779b9u * seed;
    for (auto& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = std::uint8_t(state >> 24);
    }
    return bytes;
}

Bytes Sealed(Bytes bytes) {
    Seal(bytes);
    return bytes;
}

Bytes ReadAll(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

unsigned CountFiles(const fs::path& directory) {
    unsigned count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        count += entry.is_regular_file();
    }
    return count;
}

unsigned CountTemps(const fs::path& directory) {
    unsigned count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        count += PipelineCacheFiles::IsTempFile(entry.path());
    }
    return count;
}

void Touch(const fs::path& file, const std::string& text = "x") {
    std::ofstream(file, std::ios::binary) << text;
}

// ---------------------------------------------------------------------------------------------
// Blob integrity

void TestSealRoundTrips() {
    for (const std::size_t size : {0, 1, 15, 16, 17, 255, 4096, 70000}) {
        const auto payload = Pattern(size, std::uint8_t(size));
        auto blob = Sealed(payload);
        CHECK(blob.size() == payload.size() + SealSize);
        CHECK(Unseal(blob));
        CHECK(blob == payload);
    }
}

void TestTruncatedBlobsAreRejected() {
    const auto blob = Sealed(Pattern(300));
    for (std::size_t cut = 0; cut < blob.size(); ++cut) {
        Bytes shorter(blob.begin(), blob.begin() + std::ptrdiff_t(cut));
        const auto before = shorter;
        CHECK(!Unseal(shorter));
        CHECK(shorter == before);
    }
    Bytes without_first(blob.begin() + 1, blob.end());
    CHECK(!Unseal(without_first));
}

void TestEmptyAndZeroFilledBlobsAreRejected() {
    Bytes empty;
    CHECK(!Unseal(empty));
    for (std::size_t size = 0; size <= 96; ++size) {
        Bytes zeros(size, 0);
        CHECK(!Unseal(zeros));
    }
    Bytes zeros(4096, 0);
    CHECK(!Unseal(zeros));
}

void TestEverySingleBitFlipIsRejected() {
    const auto blob = Sealed(Pattern(40));
    for (std::size_t bit = 0; bit < blob.size() * 8; ++bit) {
        auto flipped = blob;
        flipped[bit / 8] ^= std::uint8_t(1u << (bit % 8));
        CHECK(!Unseal(flipped));
    }
}

void TestTrailingGarbageIsRejected() {
    const auto blob = Sealed(Pattern(100));
    for (const std::size_t extra : {1, 2, 15, 16, 17, 100}) {
        auto longer = blob;
        longer.insert(longer.end(), extra, std::uint8_t(0xa5));
        CHECK(!Unseal(longer));
    }
    // Two sealed blobs back to back are not one blob either.
    auto doubled = blob;
    doubled.insert(doubled.end(), blob.begin(), blob.end());
    CHECK(!Unseal(doubled));
}

void TestOversizedBlobIsRejected() {
    // A correctly sealed blob one byte past the largest blob is refused: the size bound alone
    // rejects it (no reader allocates from a size beyond it).
    Bytes huge(MaxBlobBytes + 1, 0);
    Seal(huge);
    CHECK(huge.size() == MaxBlobBytes + 1 + SealSize);
    CHECK(!Unseal(huge));
    CHECK(huge.size() == MaxBlobBytes + 1 + SealSize);
    Bytes largest(MaxBlobBytes, 0);
    Seal(largest);
    CHECK(Unseal(largest) && largest.size() == MaxBlobBytes);
}

// ---------------------------------------------------------------------------------------------
// Serialization bounds

Bytes WithCount(std::uint64_t count, std::size_t payload_bytes) {
    Bytes bytes(sizeof(count) + payload_bytes, 0x7f);
    std::memcpy(bytes.data(), &count, sizeof(count));
    return bytes;
}

template <typename Container>
bool ReadThrowsCorrupt(Bytes bytes, Container& out) {
    Serialization::Archive ar{std::move(bytes)};
    Serialization::Reader in{ar};
    try {
        in.Read(out);
    } catch (const Serialization::CorruptData&) {
        return true;
    }
    return false;
}

void TestCorruptCountsAndLengthsThrowInsteadOfAllocating() {
    for (const std::uint64_t count : {std::uint64_t{0xffffffffffffffff}, std::uint64_t{1} << 62,
                                      std::uint64_t{1} << 40, std::uint64_t{0x7fffffff}}) {
        std::vector<std::uint32_t> list;
        CHECK(ReadThrowsCorrupt(WithCount(count, 16), list));
        CHECK(list.capacity() <= 16);
        std::string text;
        CHECK(ReadThrowsCorrupt(WithCount(count, 16), text));
        CHECK(text.capacity() < 1024);
    }
    // One element more than the bytes hold throws; exactly what they hold reads.
    std::vector<std::uint32_t> list;
    CHECK(ReadThrowsCorrupt(WithCount(5, 16), list));
    std::vector<std::uint32_t> exact;
    CHECK(!ReadThrowsCorrupt(WithCount(4, 16), exact));
    CHECK(exact.size() == 4);
    std::string text;
    CHECK(!ReadThrowsCorrupt(WithCount(16, 16), text));
    CHECK(text.size() == 16);
    std::string longer;
    CHECK(ReadThrowsCorrupt(WithCount(17, 16), longer));
}

void TestReadsNeverPassTheEnd() {
    const Bytes ten(10, 1);
    {
        Serialization::Archive ar{Bytes(ten)};
        Serialization::Reader in{ar};
        std::uint32_t value{};
        in.Read(value);
        in.Read(value);
        bool corrupt = false;
        try {
            in.Read(value);
        } catch (const Serialization::CorruptData&) {
            corrupt = true;
        }
        CHECK(corrupt);
    }
    for (const std::size_t size : {std::size_t(7), ~std::size_t(0), ~std::size_t(0) - 3}) {
        Serialization::Archive ar{Bytes(ten)};
        Serialization::Reader in{ar};
        std::uint32_t value{};
        in.Read(value);
        bool corrupt = false;
        try {
            char sink[16];
            in.Read(sink, size);
        } catch (const Serialization::CorruptData&) {
            corrupt = true;
        }
        CHECK(corrupt);
        corrupt = false;
        try {
            ar.Advance(size);
        } catch (const Serialization::CorruptData&) {
            corrupt = true;
        }
        CHECK(corrupt && ar.Remaining() == 6);
    }
    // A whole entry cut anywhere is corrupt data, never a process-ending assertion.
    Serialization::Archive writer;
    Serialization::Writer out{writer};
    out.Write(std::uint32_t{5});
    out.Write(std::vector<std::uint32_t>{1, 2, 3, 4});
    out.Write(std::string{"entry"});
    const auto entry = writer.TakeOff();
    for (std::size_t cut = 0; cut < entry.size(); ++cut) {
        Serialization::Archive ar{Bytes(entry.begin(), entry.begin() + std::ptrdiff_t(cut))};
        Serialization::Reader in{ar};
        bool corrupt = false;
        try {
            std::uint32_t version{};
            std::vector<std::uint32_t> values;
            std::string name;
            in.Read(version);
            in.Read(values);
            in.Read(name);
        } catch (const Serialization::CorruptData&) {
            corrupt = true;
        }
        CHECK(corrupt);
    }
    Serialization::Archive whole{Bytes(entry)};
    Serialization::Reader in{whole};
    std::uint32_t version{};
    std::vector<std::uint32_t> values;
    std::string name;
    in.Read(version);
    in.Read(values);
    in.Read(name);
    CHECK(version == 5 && values.size() == 4 && values[3] == 4 && name == "entry" && whole.IsEoS());
}

// ---------------------------------------------------------------------------------------------
// Atomic writes and temporary files

bool WriteHalfThenFail(const fs::path& file, std::span<const std::uint8_t> bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size() / 2));
    return false;
}

bool FailToReplace(const fs::path&, const fs::path&) {
    return false;
}

void TestFailedWritesKeepThePreviousFile() {
    TempDir dir{"atomic-fail"};
    const auto file = dir.path / "g_0x1.key";
    const auto previous = Sealed(Pattern(500, 1));
    CHECK(PipelineCacheFiles::WriteFileAtomically(file, previous));

    const auto next = Sealed(Pattern(900, 2));
    CHECK(!PipelineCacheFiles::WriteFileAtomically(file, next, {.write = &WriteHalfThenFail}));
    CHECK(ReadAll(file) == previous);
    auto loaded = ReadAll(file);
    CHECK(Unseal(loaded) && loaded == Pattern(500, 1));
    CHECK(!PipelineCacheFiles::WriteFileAtomically(file, next, {.replace = &FailToReplace}));
    CHECK(ReadAll(file) == previous);
    CHECK(CountTemps(dir.path) == 0 && CountFiles(dir.path) == 1);

    // No previous file: a failed write leaves nothing a reader could load.
    const auto fresh = dir.path / "g_0x2.key";
    CHECK(!PipelineCacheFiles::WriteFileAtomically(fresh, next, {.write = &WriteHalfThenFail}));
    CHECK(!fs::exists(fresh) && CountTemps(dir.path) == 0);

    // A folder that does not exist is a failed write too, not a crash.
    CHECK(!PipelineCacheFiles::WriteFileAtomically(dir.path / "missing" / "x.key", next));
}

void TestSuccessfulWritesReplaceExactly() {
    TempDir dir{"atomic-ok"};
    const auto file = dir.path / "0x1.meta";
    for (const std::size_t size : {600, 40, 0, 5000, 5000}) {
        const auto bytes = Pattern(size, std::uint8_t(size));
        CHECK(PipelineCacheFiles::WriteFileAtomically(file, bytes));
        CHECK(ReadAll(file) == bytes);
        CHECK(CountFiles(dir.path) == 1);
    }
}

void TestConcurrentWritersDoNotShareTemporaryFiles() {
    std::set<std::string> names;
    std::mutex names_mutex;
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 300; ++i) {
                    const auto temp = PipelineCacheFiles::TempPathFor("dir/g_0x1.key").string();
                    std::scoped_lock lock{names_mutex};
                    names.insert(temp);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }
    CHECK(names.size() == 1200);
    TempDir dir{"atomic-concurrent"};
    const auto file = dir.path / "g_0x1.key";
    std::vector<std::thread> writers;
    for (int t = 0; t < 6; ++t) {
        writers.emplace_back([&, t] {
            for (int i = 0; i < 25; ++i) {
                PipelineCacheFiles::WriteFileAtomically(file, Bytes(2000 + std::size_t(t) * 37,
                                                                     std::uint8_t(t + 1)));
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    const auto content = ReadAll(file);
    bool one_writer = !content.empty();
    for (const auto byte : content) {
        one_writer = one_writer && byte == content.front();
    }
    CHECK(one_writer && content.size() == 2000 + std::size_t(content.front() - 1) * 37);
    CHECK(CountTemps(dir.path) == 0 && CountFiles(dir.path) == 1);
}

void TestOrphanTemporaryFilesAreCleaned() {
    TempDir dir{"orphans"};
    const auto keep = dir.path / "g_0x1.key";
    CHECK(PipelineCacheFiles::WriteFileAtomically(keep, Sealed(Pattern(64))));
    const auto old_temp = dir.path / "g_0x2.key.4242.7.tmp";
    const auto older_style = dir.path / "g_0x3.key.tmp";
    const auto fresh_temp = dir.path / "g_0x4.key.4243.1.tmp";
    for (const auto& temp : {old_temp, older_style, fresh_temp}) {
        Touch(temp, "partial");
    }
    const auto long_ago = fs::file_time_type::clock::now() - 2h;
    fs::last_write_time(old_temp, long_ago);
    fs::last_write_time(older_style, long_ago);

    // A write another process (the other game build) is making right now is not an orphan.
    CHECK(PipelineCacheFiles::RemoveLeftoverTemps(dir.path) == 2);
    CHECK(!fs::exists(old_temp) && !fs::exists(older_style));
    CHECK(fs::exists(fresh_temp) && fs::exists(keep));
    CHECK(PipelineCacheFiles::RemoveLeftoverTemps(dir.path, 0s) == 1);
    CHECK(!fs::exists(fresh_temp) && fs::exists(keep) && CountFiles(dir.path) == 1);
    CHECK(PipelineCacheFiles::RemoveLeftoverTemps(dir.path / "absent", 0s) == 0);
}

// ---------------------------------------------------------------------------------------------
// The bounded IO queue

void TestQueueCountsStayExactUnderConcurrentProducers() {
    constexpr int Producers = 8;
    constexpr int PerProducer = 400;
    auto queue = std::make_shared<Storage::RequestQueue>(1u << 20, std::size_t{1} << 30);
    std::atomic<int> ran{0};
    std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
    std::vector<std::thread> producers;
    std::atomic<bool> consistent{true};
    for (int p = 0; p < Producers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < PerProducer; ++i) {
                if (i % 50 == 7) {
                    queue->Submit([] { throw std::runtime_error{"write failed"}; });
                } else if (i % 50 == 9) {
                    queue->Submit([] { throw 1; }); // not even an exception type
                } else {
                    queue->Submit([&] { ++ran; });
                }
                if (queue->Completed() + queue->Failed() > queue->Submitted()) {
                    consistent = false;
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (queue->Completed() + queue->Failed() < std::uint64_t(Producers) * PerProducer &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    constexpr int Throwing = Producers * (PerProducer / 50) * 2;
    CHECK(consistent.load());
    CHECK(queue->Submitted() == std::uint64_t(Producers) * PerProducer);
    CHECK(queue->Pending() == 0 && queue->Dropped() == 0 && queue->Overflowed() == 0);
    CHECK(queue->Failed() == Throwing);
    CHECK(queue->Completed() == std::uint64_t(Producers) * PerProducer - Throwing);
    CHECK(ran.load() == Producers * PerProducer - Throwing);
}

void TestCloseDrainsForABoundedTimeAndCountsTheRest() {
    {
        auto queue = std::make_shared<Storage::RequestQueue>();
        std::atomic<int> ran{0};
        for (int i = 0; i < 4; ++i) {
            CHECK(queue->Submit([&] {
                std::this_thread::sleep_for(5ms);
                ++ran;
            }));
        }
        std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
        queue->Close(10s);
        worker.join();
        CHECK(ran.load() == 4 && queue->Dropped() == 0 && queue->Pending() == 0);
        CHECK(queue->Completed() == 4);
    }
    {
        auto queue = std::make_shared<Storage::RequestQueue>();
        std::atomic<int> ran{0};
        for (int i = 0; i < 20; ++i) {
            queue->Submit([&] {
                std::this_thread::sleep_for(40ms);
                ++ran;
            });
        }
        const auto started = std::chrono::steady_clock::now();
        std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
        queue->Close(60ms);
        worker.join();
        const auto took = std::chrono::steady_clock::now() - started;
        CHECK(ran.load() >= 1 && ran.load() < 20);
        CHECK(queue->Dropped() > 0);
        CHECK(queue->Completed() + queue->Failed() + queue->Dropped() == queue->Submitted());
        CHECK(queue->Pending() == 0 && queue->PendingBytes() == 0);
        CHECK(took < 600ms); // 20 x 40 ms would be 800 ms
    }
}

void TestSubmitAfterCloseIsRefused() {
    auto queue = std::make_shared<Storage::RequestQueue>();
    std::atomic<int> ran{0};
    CHECK(queue->Submit([&] { ++ran; }));
    queue->Close(1s);
    CHECK(!queue->Submit([&] { ++ran; }));
    CHECK(!queue->Submit([&] { ++ran; }, 10));
    CHECK(queue->Submitted() == 1 && queue->Pending() == 1);
    std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
    worker.join();
    CHECK(ran.load() == 1 && queue->Completed() == 1 && queue->Dropped() == 0);
    queue->Close(1s); // a second Close changes nothing
    CHECK(!queue->Submit([] {}));
}

void TestStopAbandonsTheQueueWithoutHanging() {
    auto queue = std::make_shared<Storage::RequestQueue>();
    std::atomic<bool> started{false}, release{false};
    queue->Submit([&] {
        started = true;
        while (!release) {
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 3; ++i) {
        queue->Submit([] {});
    }
    std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
    while (!started) {
        std::this_thread::yield();
    }
    worker.request_stop();
    release = true;
    worker.join();
    CHECK(queue->Completed() == 1 && queue->Dropped() == 3 && queue->Pending() == 0);

    // An idle worker wakes for a stop request.
    auto idle = std::make_shared<Storage::RequestQueue>();
    std::jthread idle_worker{[idle](std::stop_token stoken) { idle->Run(stoken); }};
    std::this_thread::sleep_for(10ms);
    idle_worker.request_stop();
    idle_worker.join();
    CHECK(idle->Dropped() == 0);
}

void TestAFullQueueRefusesAndCountsWrites() {
    // By task count: nothing runs yet (no worker), so the queue fills.
    {
        auto queue = std::make_shared<Storage::RequestQueue>(3, std::size_t{1} << 20);
        int accepted = 0;
        for (int i = 0; i < 10; ++i) {
            accepted += queue->Submit([] {});
        }
        CHECK(accepted == 3 && queue->Submitted() == 3 && queue->Overflowed() == 7);
        CHECK(queue->Pending() == 3);
        std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
        queue->Close(5s);
        worker.join();
        CHECK(queue->Completed() == 3 && queue->Dropped() == 0 && queue->Overflowed() == 7);
    }
    // By bytes: the blobs a queue holds are bounded too, and a write that ran frees its share.
    {
        auto queue = std::make_shared<Storage::RequestQueue>(100, 1000);
        CHECK(queue->Submit([] {}, 600));
        CHECK(!queue->Submit([] {}, 600));
        CHECK(queue->Submit([] {}, 400));
        CHECK(!queue->Submit([] {}, 1));
        CHECK(!queue->Submit([] {}, 5000)); // larger than the whole budget
        CHECK(queue->PendingBytes() == 1000 && queue->Overflowed() == 3);
        std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (queue->Completed() < 2 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        CHECK(queue->PendingBytes() == 0);
        CHECK(queue->Submit([] {}, 1000));
        queue->Close(5s);
        worker.join();
        CHECK(queue->Completed() == 3 && queue->Submitted() == 3);
    }
    // The production queue has a bound at all.
    CHECK(Storage::RequestQueue::DefaultMaxTasks > 0 && Storage::RequestQueue::DefaultMaxTasks <= 65536);
    CHECK(Storage::RequestQueue::DefaultMaxBytes <= (std::size_t{1} << 30));
}

// ---------------------------------------------------------------------------------------------
// One store per game build

const StoreFormat Format{0x10de, 0x2204, 4, 9, 8, 6};

void TestEachBuildHasItsOwnFolderAndIdentity() {
    const auto a = CurrentStoreIdentity(Serial, V100, Format);
    const auto b = CurrentStoreIdentity(Serial, V101, Format);
    CHECK(a != b && a.game != b.game);
    CHECK(CheckStore(EncodeStoreIdentity(a), b) == StoreVerdict::Incompatible);
    CHECK(CheckStore(EncodeStoreIdentity(b), a) == StoreVerdict::Incompatible);
    CHECK(CheckStore(EncodeStoreIdentity(a), a) == StoreVerdict::Reuse);
    CHECK(CheckStore(EncodeStoreIdentity(b), b) == StoreVerdict::Reuse);
    CHECK(DecideStoreAction(StoreVerdict::Incompatible, true, true, false) == StoreAction::Rebuild);
    // The build key is derived from the version string, never written out.
    CHECK(CacheFolderName(Serial, V100) == std::string{Serial} + "-" + V100);
    CHECK(CacheFolderName(Serial, V101) == std::string{Serial} + "-" + V101);
    CHECK(CacheFolderName(Serial, V100) != CacheFolderName(Serial, V101));
    CHECK(CacheFolderName(Serial, "") == Serial);
    CHECK(CacheFolderName("../x", "a/b\\c:d") == ".._x-a_b_c_d");
    CHECK(GameIdentity("AB", "C") != GameIdentity("A", "BC"));
    CHECK(GameIdentity(Serial, "") != GameIdentity(Serial, V100));
    // A format change is a rebuild for every build.
    for (const auto& changed :
         {StoreFormat{0x10de, 0x2204, 4, 10, 8, 6}, StoreFormat{0x10de, 0x2204, 4, 9, 9, 6},
          StoreFormat{0x10de, 0x2204, 4, 9, 8, 7}, StoreFormat{0x1002, 0x73bf, 1, 9, 8, 6}}) {
        CHECK(CheckStore(EncodeStoreIdentity(a), CurrentStoreIdentity(Serial, V100, changed)) ==
              StoreVerdict::Incompatible);
    }
    // What the store does: the record is sealed on disk and unsealed on load.
    auto on_disk = Sealed(EncodeStoreIdentity(b));
    CHECK(Unseal(on_disk) && CheckStore(on_disk, b) == StoreVerdict::Reuse);
}

/// The identity record as the single-folder store wrote it: magic, record version 1, 24 bytes of
/// vendor, device, driver and the three serialization versions.
Bytes OldSingleFolderRecord(const StoreFormat& format) {
    Bytes bytes;
    const std::uint32_t words[] = {StoreIdentityMagic,           1u,
                                   24u,                          format.vendor_id,
                                   format.device_id,             format.driver_id,
                                   format.shader_binary_version, format.shader_meta_version,
                                   format.pipeline_key_version};
    for (const auto word : words) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&word);
        bytes.insert(bytes.end(), p, p + 4);
    }
    return bytes;
}

void TestTheSingleFolderStoreIsKeptNotDeleted() {
    const auto old_record = OldSingleFolderRecord(Format);
    CHECK(EncodeLegacyStoreIdentity(Format) == old_record);
    for (const char* version : {V100, V101, ""}) {
        const auto current = CurrentStoreIdentity(Serial, version, Format);
        CHECK(CheckStore(old_record, current) == StoreVerdict::Legacy);
        // Same GPU, format and shader profile: kept and stamped with this build.
        CHECK(DecideStoreAction(StoreVerdict::Legacy, true, true, false) == StoreAction::Adopt);
        // After the stamp, the next session of this build reuses it; the other build does not.
        CHECK(CheckStore(EncodeStoreIdentity(current), current) == StoreVerdict::Reuse);
    }
    CHECK(CheckStore(EncodeStoreIdentity(CurrentStoreIdentity(Serial, V100, Format)),
                     CurrentStoreIdentity(Serial, V101, Format)) == StoreVerdict::Incompatible);
    // Another GPU or format, or a changed shader profile: rebuilt (and the log says why).
    auto other = Format;
    other.shader_binary_version = 8;
    CHECK(CheckStore(OldSingleFolderRecord(other), CurrentStoreIdentity(Serial, V101, Format)) ==
          StoreVerdict::Incompatible);
    CHECK(DecideStoreAction(StoreVerdict::Legacy, true, false, false) == StoreAction::Rebuild);
    CHECK(!ToString(StoreVerdict::Legacy).empty() && ToString(StoreVerdict::Legacy) != "unknown");

    // Which store takes the old folder over: the first build that has none of its own.
    CHECK(AdoptLegacyFolder(true, true, false));
    CHECK(!AdoptLegacyFolder(true, true, true));   // this build already has its store
    CHECK(!AdoptLegacyFolder(true, false, false)); // nothing to take over
    CHECK(!AdoptLegacyFolder(false, true, false)); // unknown build: it is the same folder
}

void TestTheSingleFolderStoreMovesToTheFirstBuild() {
    TempDir dir{"legacy-move"};
    const auto legacy = dir.path / Serial;
    const auto first = dir.path / CacheFolderName(Serial, V101);
    const auto second = dir.path / CacheFolderName(Serial, V100);
    fs::create_directories(legacy / "pipeline-binaries");
    const auto binary = legacy / "pipeline-binaries" / "ab12.bin";
    const auto binary_bytes = Pattern(300, 9);
    CHECK(PipelineCacheFiles::WriteFileAtomically(binary, binary_bytes));
    const char* store[] = {"g_0x1.key", "c_0x2.key", "0x1.meta", "0x1_0.spv", "profile.bin",
                           "identity.bin", "warmup.bin"};
    for (const char* name : store) {
        CHECK(PipelineCacheFiles::WriteFileAtomically(legacy / name, Sealed(Pattern(20))));
    }
    Touch(legacy / "readme.txt");
    Touch(legacy / "g_0x9.key.77.1.tmp", "partial");

    CHECK(PipelineCacheFiles::HasStoreFiles(legacy));
    CHECK(!PipelineCacheFiles::HasStoreFiles(first));
    CHECK(AdoptLegacyFolder(true, PipelineCacheFiles::HasStoreFiles(legacy),
                            PipelineCacheFiles::HasStoreFiles(first)));
    const auto moved = PipelineCacheFiles::MoveStoreFiles(legacy, first);
    CHECK(moved.moved == 7 && moved.failed == 0);
    for (const char* name : store) {
        CHECK(fs::exists(first / name) && !fs::exists(legacy / name));
    }
    auto identity = ReadAll(first / "identity.bin");
    CHECK(Unseal(identity) && identity == Pattern(20));
    // The shared binaries, other files and unfinished writes stay where they were.
    CHECK(ReadAll(binary) == binary_bytes && !fs::exists(first / "pipeline-binaries"));
    CHECK(fs::exists(legacy / "readme.txt") && fs::exists(legacy / "g_0x9.key.77.1.tmp"));
    CHECK(!PipelineCacheFiles::HasStoreFiles(legacy));

    // The other build finds nothing left to take over and starts its own store.
    CHECK(!AdoptLegacyFolder(true, PipelineCacheFiles::HasStoreFiles(legacy),
                             PipelineCacheFiles::HasStoreFiles(second)));
    // A second run of the first build keeps its store as it is.
    CHECK(!AdoptLegacyFolder(true, true, PipelineCacheFiles::HasStoreFiles(first)));

    // A file the target already has is not replaced.
    fs::create_directories(dir.path / "again");
    CHECK(PipelineCacheFiles::WriteFileAtomically(dir.path / "again" / "g_0x1.key", Pattern(5)));
    CHECK(PipelineCacheFiles::WriteFileAtomically(dir.path / "g_0x1.key", Pattern(6)));
    const auto again = PipelineCacheFiles::MoveStoreFiles(dir.path, dir.path / "again");
    CHECK(again.moved == 0 && ReadAll(dir.path / "again" / "g_0x1.key") == Pattern(5));
    CHECK(fs::exists(dir.path / "g_0x1.key"));
}

void TestCorruptOrMissingIdentityDecisions() {
    const auto current = CurrentStoreIdentity(Serial, V101, Format);
    const auto stored = EncodeStoreIdentity(current);
    CHECK(CheckStore({}, current) == StoreVerdict::Missing);
    CHECK(CheckStore(Bytes(stored.size(), 0), current) == StoreVerdict::Corrupt);
    for (std::size_t cut = 1; cut < stored.size(); ++cut) {
        const auto verdict = CheckStore(std::span{stored}.first(cut), current);
        CHECK(verdict == StoreVerdict::Corrupt);
    }
    auto longer = stored;
    longer.push_back(0);
    CHECK(CheckStore(longer, current) == StoreVerdict::Corrupt);
    for (std::size_t byte = 0; byte < 12; ++byte) {
        auto changed = stored;
        changed[byte] ^= 1;
        CHECK(CheckStore(changed, current) == StoreVerdict::Corrupt);
    }
    CHECK(DecideStoreAction(StoreVerdict::Corrupt, true, true, false) == StoreAction::Rebuild);
    CHECK(DecideStoreAction(StoreVerdict::Reuse, true, true, false) == StoreAction::Reuse);
    CHECK(DecideStoreAction(StoreVerdict::Reuse, true, false, false) == StoreAction::Rebuild);
    // A new store is filled, not cleared; a folder with files and no identity is rebuilt.
    CHECK(DecideStoreAction(StoreVerdict::Missing, false, false, true) == StoreAction::Create);
    CHECK(DecideStoreAction(StoreVerdict::Missing, false, false, false) == StoreAction::Rebuild);
    CHECK(DecideStoreAction(StoreVerdict::Missing, true, true, true) == StoreAction::Rebuild);
}

// ---------------------------------------------------------------------------------------------
// Warm-up mode, the build gate and failures

const GpuIdentity Gpu{0x10de, 0x2204, 4, 0x8c9a2000, 0x403150};

void TestParallelFailureRecordsSerialOnlyForThatGpuAndDriver() {
    const auto policy = EncodeWarmupPolicy({Gpu, 1});
    CHECK(ChooseWarmupMode(policy, Gpu) == WarmupMode::Serial);
    auto updated = Gpu;
    updated.driver_version += 1;
    CHECK(ChooseWarmupMode(policy, updated) == WarmupMode::Parallel);
    auto other = Gpu;
    other.device_id = 0x2484;
    CHECK(ChooseWarmupMode(policy, other) == WarmupMode::Parallel);
    CHECK(ChooseWarmupMode({}, Gpu) == WarmupMode::Parallel);
    CHECK(ChooseWarmupMode(EncodeWarmupPolicy({Gpu, 0}), Gpu) == WarmupMode::Parallel);
    for (std::size_t cut = 0; cut < policy.size(); ++cut) {
        CHECK(ChooseWarmupMode(std::span{policy}.first(cut), Gpu) == WarmupMode::Parallel);
    }
}

void TestWarmupFailureOnlySkipsThatEntry() {
    for (const bool retried : {false, true}) {
        CHECK(OnPipelineFailure(PipelineOrigin::Preload, retried) == FailureAction::DropCachedEntry);
    }
    CHECK(OnWarmupBuildFailure(false).drop_entry && OnWarmupBuildFailure(true).drop_entry);
    CHECK(!OnWarmupBuildFailure(false).record_serial && OnWarmupBuildFailure(true).record_serial);

    // A build that ran alone says nothing about parallel builds; one that overlapped does.
    BuildGate gate;
    {
        BuildGate::Slot alone{gate, BuildGate::Kind::Warmup};
        CHECK(alone.Parallel() && !alone.Overlapped());
        CHECK(!OnWarmupBuildFailure(alone.Overlapped()).record_serial);
    }
    BuildGate::Slot first{gate, BuildGate::Kind::Warmup};
    CHECK(!first.Overlapped());
    {
        BuildGate::Slot second{gate, BuildGate::Kind::Game};
        CHECK(first.Overlapped() && second.Overlapped());
    }
    CHECK(first.Overlapped() && OnWarmupBuildFailure(first.Overlapped()).record_serial);
    CHECK(gate.EnableSerial() && !gate.EnableSerial() && gate.Serial());
}

struct Fatal {};

void TestGamePipelineGetsOneRetryThenIsFatal() {
    {
        BuildGate gate;
        int made = 0, paused = 0, fatal = 0;
        const auto value = BuildGamePipeline(
            gate,
            [&] {
                ++made;
                return 7;
            },
            [&](const BuildFailed&) { ++paused; }, [&](const BuildFailed&) { ++fatal; });
        CHECK(value == 7 && made == 1 && paused == 0 && fatal == 0);
    }
    {
        BuildGate gate;
        int made = 0, paused = 0, fatal = 0;
        const auto value = BuildGamePipeline(
            gate,
            [&] {
                if (++made == 1) {
                    throw BuildFailed{"refused"};
                }
                return std::string{"built"};
            },
            [&](const BuildFailed& error) {
                ++paused;
                CHECK(std::string{error.what()} == "refused");
                CHECK(gate.WaitForParallelBuilds(0ms)); // the failed build's slot is released
            },
            [&](const BuildFailed&) { ++fatal; });
        CHECK(value == "built" && made == 2 && paused == 1 && fatal == 0);
    }
    {
        BuildGate gate;
        int made = 0, paused = 0, fatal = 0;
        bool reached_fatal = false;
        try {
            BuildGamePipeline(
                gate,
                [&]() -> int {
                    ++made;
                    throw BuildFailed{"refused"};
                },
                [&](const BuildFailed&) { ++paused; },
                [&](const BuildFailed& error) {
                    ++fatal;
                    CHECK(std::string{error.what()} == "refused");
                    throw Fatal{};
                });
        } catch (const Fatal&) {
            reached_fatal = true;
        }
        CHECK(reached_fatal && made == 2 && paused == 1 && fatal == 1);
    }
    {
        // Not a driver refusal: no retry, no pause.
        BuildGate gate;
        int made = 0;
        bool other = false;
        try {
            BuildGamePipeline(
                gate,
                [&]() -> int {
                    ++made;
                    throw std::logic_error{"not a driver refusal"};
                },
                [](const BuildFailed&) {}, [](const BuildFailed&) {});
        } catch (const std::logic_error&) {
            other = true;
        }
        CHECK(other && made == 1);
    }
    CHECK(OnPipelineFailure(PipelineOrigin::Game, false) == FailureAction::RetryWithWarmUpPaused);
    CHECK(OnPipelineFailure(PipelineOrigin::Game, true) == FailureAction::Fatal);
}

void TestSerialBuildsRunOneAtATime() {
    BuildGate gate;
    CHECK(gate.EnableSerial());
    std::atomic<int> running{0}, peak{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 20; ++i) {
                BuildGate::Slot slot{gate, t % 2 ? BuildGate::Kind::Game : BuildGate::Kind::Warmup};
                CHECK(!slot.Parallel());
                const int now = ++running;
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {
                }
                std::this_thread::yield();
                --running;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(peak.load() == 1);
}

void TestGameBuildsAreNotQueuedBehindWarmupBuilds() {
    using K = BuildGate::Kind;
    // The serial rule itself: one build at a time, and a waiting game build before any warm-up one.
    CHECK(BuildGate::MayStartSerial(K::Game, 0, false, 0) && BuildGate::MayStartSerial(K::Warmup, 0, false, 0));
    CHECK(BuildGate::MayStartSerial(K::Game, 0, false, 3));
    CHECK(!BuildGate::MayStartSerial(K::Warmup, 0, false, 1));
    for (const auto kind : {K::Game, K::Warmup}) {
        CHECK(!BuildGate::MayStartSerial(kind, 0, true, 0));
        CHECK(!BuildGate::MayStartSerial(kind, 2, false, 0));
    }

    // A warm-up build runs; four more warm-up builds queue at the gate, then the game asks for a
    // pipeline. When the running build ends, the game's goes first, every time.
    for (int round = 0; round < 3; ++round) {
        BuildGate gate;
        gate.EnableSerial();
        std::mutex order_mutex;
        std::vector<char> order;
        auto holder = std::make_unique<BuildGate::Slot>(gate, BuildGate::Kind::Warmup);
        std::vector<std::thread> warmups;
        for (int i = 0; i < 4; ++i) {
            warmups.emplace_back([&] {
                BuildGate::Slot slot{gate, BuildGate::Kind::Warmup};
                std::scoped_lock lock{order_mutex};
                order.push_back('w');
            });
        }
        std::this_thread::sleep_for(40ms);
        CHECK(gate.GamesWaiting() == 0);
        std::thread game([&] {
            BuildGamePipeline(
                gate,
                [&] {
                    std::scoped_lock lock{order_mutex};
                    order.push_back('g');
                    return 0;
                },
                [](const BuildFailed&) {}, [](const BuildFailed&) {});
        });
        // The game's build waits as a game build, whatever the scheduler does next.
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (gate.GamesWaiting() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        CHECK(gate.GamesWaiting() == 1);
        {
            std::scoped_lock lock{order_mutex};
            CHECK(order.empty()); // everyone waits for the running build
        }
        holder.reset();
        for (auto& warmup : warmups) {
            warmup.join();
        }
        game.join();
        CHECK(order.size() == 5 && order.front() == 'g');
        CHECK(gate.GamesWaiting() == 0);
    }
}

void TestPauseWaitsForParallelBuildsAndTimesOut() {
    BuildGate gate;
    std::atomic<bool> release{false}, entered{false};
    std::thread build([&] {
        BuildGate::Slot slot{gate, BuildGate::Kind::Warmup};
        entered = true;
        while (!release) {
            std::this_thread::yield();
        }
    });
    while (!entered) {
        std::this_thread::yield();
    }
    CHECK(gate.EnableSerial());
    const auto started = std::chrono::steady_clock::now();
    CHECK(!gate.WaitForParallelBuilds(30ms));
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    release = true;
    build.join();
    CHECK(gate.WaitForParallelBuilds(5s));
}

void TestOnDemandKnob() {
    for (const char* env : {"1", "2", "true", "yes", "00", " 0", "anything"}) {
        CHECK(PipelineOnDemandRequested(env));
    }
    CHECK(!PipelineOnDemandRequested(nullptr));
    CHECK(!PipelineOnDemandRequested(""));
    CHECK(!PipelineOnDemandRequested("0"));
}

void TestWarmupInboxAndPublishFirst() {
    CHECK(Vulkan::WarmupThreadCount(2) == 1 && Vulkan::WarmupThreadCount(4) == 1);
    CHECK(Vulkan::WarmupThreadCount(8) == 6 && Vulkan::WarmupThreadCount(32) == 6);
    Vulkan::WarmupInbox<int> inbox;
    for (int i = 0; i < 10; ++i) {
        inbox.Push(i);
    }
    const auto first = inbox.Take(4);
    CHECK((first == std::vector<int>{0, 1, 2, 3}) && !inbox.Empty());
    CHECK(inbox.Take(100).size() == 6 && inbox.Empty());
    std::unordered_map<int, int> published{{2, 200}};
    CHECK(!Vulkan::PublishFirst(published, 2, 999) && published[2] == 200);
    CHECK(Vulkan::PublishFirst(published, 3, 300) && published[3] == 300);
}

// ---------------------------------------------------------------------------------------------
// Source contracts of the wiring

std::string Source(const char* path) {
    std::ifstream in(std::string(SOS_STRING(SOS_SOURCE_DIR)) + "/" + path, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(in), {}};
    std::string unix_text;
    for (const char c : text) {
        if (c != '\r') {
            unix_text += c;
        }
    }
    CHECK(!unix_text.empty());
    return unix_text;
}

int Count(const std::string& text, const std::string& part) {
    int count = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

bool Before(const std::string& text, const std::string& first, const std::string& second) {
    const size_t a = text.find(first);
    const size_t b = text.find(second);
    return a != std::string::npos && b != std::string::npos && a < b;
}

void TestStorageWiring() {
    const auto storage = Source("gpu/shadps4/video_core/cache_storage.cpp");
    // The IO thread is the bounded queue: drained for a bounded time, summarised once, never a
    // stop request that drops what is queued, never an unbounded join.
    CHECK(Has(storage, "constexpr auto CloseDrainLimit = std::chrono::milliseconds(1500);"));
    CHECK(Count(storage, "queue->Close(CloseDrainLimit);") == 1);
    CHECK(Before(storage, "queue->Close(CloseDrainLimit);", "io_worker.join();"));
    CHECK(!Has(storage, "request_stop()") && !Has(storage, "packaged_task"));
    CHECK(Has(storage, "worker_queue->Run(stoken);"));
    CHECK(Count(storage, "Pipeline cache IO: ") == 1);
    CHECK(Has(storage, "queue->Dropped()") && Has(storage, "queue->Overflowed()") &&
          Has(storage, "queue->Failed()"));
    CHECK(Has(storage, "throw std::runtime_error{\"cache write failed\"};"));
    // One folder per build, named from the version the GPU side was given.
    CHECK(Has(storage, "CacheFolderName(serial, game_info.AppVer())"));
    CHECK(Has(storage, "cache_path = root / folder;"));
    CHECK(Has(storage, "root / std::filesystem::path{folder + \".zip\"}"));
    CHECK(!Has(storage, "replace_extension(\".zip\")"));
    CHECK(Before(storage, "cache_path = root / folder;", "AdoptLegacyStore(root, serial, folder);"));
    CHECK(Has(storage, "PipelineCacheFiles::MoveStoreFiles(legacy, cache_path)"));
    CHECK(Has(storage, "PipelineCachePolicy::AdoptLegacyFolder("));
    // Sealed on the way out, unsealed on the way in; atomic writes; temps cleaned off-thread.
    CHECK(Count(storage, "PipelineCachePolicy::Seal(bytes);") == 1);
    CHECK(Count(storage, "PipelineCachePolicy::Unseal(") == 4);
    CHECK(Count(storage, "PipelineCacheFiles::WriteFileAtomically(path, bytes)") == 1);
    CHECK(Has(storage, "PipelineCacheFiles::RemoveLeftoverTemps(current)"));
    CHECK(Has(storage, "PipelineCacheFiles::RemoveLeftoverTemps(binaries)"));
    CHECK(Has(storage, "data.size() * sizeof(T) > PipelineCachePolicy::MaxBlobBytes"));
}

void TestPipelineWiring() {
    const auto cache = Source("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp");
    const auto serialization =
        Source("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    const auto compute = Source("gpu/shadps4/video_core/renderer_vulkan/vk_compute_pipeline.cpp");
    const auto graphics = Source("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp");
    const auto graphics_h = Source("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.h");

    // The game's pipelines, graphics and compute, take game slots and keep their fatal end.
    CHECK(Count(cache, "PipelineCachePolicy::BuildGamePipeline(") == 1);
    CHECK(Count(cache, "CreateGamePipeline(\"graphics\"") == 1);
    CHECK(Count(cache, "CreateGamePipeline(\"compute\"") == 1);
    CHECK(Count(cache, "ASSERT_MSG(false, \"Failed to create {} pipeline: {}\"") == 1);
    CHECK(Has(cache, "PauseParallelWarmUp();"));
    CHECK(!Has(cache, "BuildGate::Slot slot{build_gate}"));
    CHECK(Has(graphics_h, "struct PipelineCreationFailed : PipelineCachePolicy::BuildFailed"));

    // Warm-up builds take warm-up slots; a failure only skips (and drops) that entry.
    CHECK(Count(serialization, "build_gate, PipelineCachePolicy::BuildGate::Kind::Warmup}") == 1);
    CHECK(Count(serialization, "PipelineCachePolicy::OnWarmupBuildFailure(") == 3);
    CHECK(Has(serialization, "OnWarmupBuildFailure(slot.Overlapped())"));
    CHECK(Count(serialization, "DropCachedPipeline(") >= 4);
    CHECK(Has(serialization, "build_gate.WaitForParallelBuilds(WarmupPauseLimit)"));
    CHECK(Has(serialization, "std::getenv(\"SOS_PIPELINE_ON_DEMAND\")"));
    CHECK(Before(serialization, "bool PipelineCache::StartBackgroundWarmUp() {",
                 "std::getenv(\"SOS_PIPELINE_ON_DEMAND\")"));
    CHECK(Before(serialization, "std::getenv(\"SOS_PIPELINE_ON_DEMAND\")", "background = new BackgroundWarmup;"));

    // The store identity names the game build; the decision keeps a legacy store and logs a rebuild.
    CHECK(Has(serialization, "CurrentStoreIdentity(game.GameSerial(), game.AppVer(),"));
    CHECK(Has(serialization, "DecideStoreAction(verdict, !profile_data.empty(), profile_match,"));
    CHECK(Has(serialization, "if (action == StoreAction::Adopt) {"));
    CHECK(Has(serialization, "std::printf(\"Pipeline cache: %s, rebuilding\\n\","));
    CHECK(Before(serialization, "std::printf(\"Pipeline cache: %s, rebuilding\\n\",", "db.Clear();"));

    // Compute pipelines get the clean retry and throw instead of asserting.
    CHECK(Before(compute, "createComputePipelineUnique(pipeline_cache", "createComputePipelineUnique(nullptr"));
    CHECK(Count(compute, "throw PipelineCreationFailed") == 1);
    CHECK(!Has(compute, "Failed to create compute pipeline: {}"));
    CHECK(Has(graphics, "Storage::PipelineBinaryDirectory()"));
}

void TestVersionReachesTheGpuSide() {
    const auto header = Source("src/dcpgpu.h");
    const auto shim = Source("gpu/shim/dcpgpu.cpp");
    const auto loader = Source("src/loader.c");
    CHECK(Before(header, "int32_t display_width, display_height;", "const char *version;"));
    CHECK(Before(header, "const char *version;", "} DcpGpuConfig;"));
    CHECK(Has(shim, "info.app_ver = config.version ? config.version : \"\";"));
    CHECK(Has(loader, "sos_game_build(paths.image_dir, &cache_build) ? cache_build.version : \"\""));
    CHECK(Has(loader, "render_target.display_height, game_version };"));
    // Derived from the version string, never a build literal.
    CHECK(!Has(Source("gpu/shadps4/video_core/cache_storage.cpp"), std::string{Serial} + "-"));
}

} // namespace

int main() {
    TestSealRoundTrips();
    TestTruncatedBlobsAreRejected();
    TestEmptyAndZeroFilledBlobsAreRejected();
    TestEverySingleBitFlipIsRejected();
    TestTrailingGarbageIsRejected();
    TestOversizedBlobIsRejected();
    TestCorruptCountsAndLengthsThrowInsteadOfAllocating();
    TestReadsNeverPassTheEnd();
    TestFailedWritesKeepThePreviousFile();
    TestSuccessfulWritesReplaceExactly();
    TestConcurrentWritersDoNotShareTemporaryFiles();
    TestOrphanTemporaryFilesAreCleaned();
    TestQueueCountsStayExactUnderConcurrentProducers();
    TestCloseDrainsForABoundedTimeAndCountsTheRest();
    TestSubmitAfterCloseIsRefused();
    TestStopAbandonsTheQueueWithoutHanging();
    TestAFullQueueRefusesAndCountsWrites();
    TestEachBuildHasItsOwnFolderAndIdentity();
    TestTheSingleFolderStoreIsKeptNotDeleted();
    TestTheSingleFolderStoreMovesToTheFirstBuild();
    TestCorruptOrMissingIdentityDecisions();
    TestParallelFailureRecordsSerialOnlyForThatGpuAndDriver();
    TestWarmupFailureOnlySkipsThatEntry();
    TestGamePipelineGetsOneRetryThenIsFatal();
    TestSerialBuildsRunOneAtATime();
    TestGameBuildsAreNotQueuedBehindWarmupBuilds();
    TestPauseWaitsForParallelBuildsAndTimesOut();
    TestOnDemandKnob();
    TestWarmupInboxAndPublishFirst();
    TestStorageWiring();
    TestPipelineWiring();
    TestVersionReachesTheGpuSide();
    std::printf("pipeline cache tests: %d checks, %d failures\n", g_checks.load(), g_failures.load());
    return g_failures.load() == 0 ? 0 : 1;
}
