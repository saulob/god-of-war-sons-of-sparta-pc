// Headless proof of the buffer arenas on the production arena_layout.h: sparse arenas keep their layout on devices
// that support sparse buffers; without sparse binding/residency or a sparse-capable queue (or with
// SOS_FORCE_NON_SPARSE_ARENAS) the arenas are fully resident instead of stopping the game. Mode selection and the
// variable read through the production helper, page and block sizes within the driver limits, the BDA page table size,
// migration of fully resident arenas on fake arenas (copy, publish, repoint, retire, stale bindings followed),
// readback runs and the indirect draw bind order, plus the source contracts of the parts that need a Vulkan device.
// No window, GPU, launcher or game.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// The vendored interval_set.h compares signed and unsigned sizes.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "video_core/buffer_cache/arena_layout.h"
#pragma GCC diagnostic pop
#include "video_core/renderer_vulkan/vk_gpu_compat.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

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

namespace Layout = VideoCore::ArenaLayout;
using Layout::ArenaMode;

constexpr u64 MiB = u64{1} << 20;
constexpr u64 GiB = u64{1} << 30;

// ---------------------------------------------------------------------------------------------
// Layout and mode
// ---------------------------------------------------------------------------------------------

// BufferCache's sparse sizing before the fully resident arenas existed.
u64 OriginalSparsePageBits(u64 max_buffer_size) {
    u64 bits = 32;
    while (bits > 30 && (u64{2} << bits) > max_buffer_size) {
        --bits;
    }
    return bits;
}

u64 OriginalSparseBlockSize(u64 alignment) {
    return Common::AlignUp(std::max<u64>(alignment, 16_KB), alignment);
}

bool IsPow2(u64 value) {
    return value != 0 && (value & (value - 1)) == 0;
}

void TestSparseLayoutUnchanged() {
    bool same = true;
    for (const u64 max_buffer : {GiB, 2 * GiB, 3 * GiB, 4 * GiB, 8 * GiB, 16 * GiB, u64{1} << 40, ~u64{0}}) {
        for (const u64 max_allocation : {64 * MiB, GiB, 4 * GiB, ~u64{0}}) {
            same &= Layout::PageBits(true, max_buffer, max_allocation) == OriginalSparsePageBits(max_buffer);
        }
    }
    Check(same, "sparse page bits are the old ones for every buffer limit, whatever the allocation limit");
    Check(Layout::PageBits(true, u64{2} << 30, ~u64{0}) == 30 && Layout::PageBits(true, ~u64{0}, ~u64{0}) == 32,
          "sparse pages: 1 GiB under AMD's 2 GiB buffer limit, 4 GiB without a limit");
    bool blocks = true;
    for (u64 alignment = 1_KB; alignment <= 2_MB; alignment <<= 1) {
        blocks &= Layout::BlockSize(true, alignment) == OriginalSparseBlockSize(alignment);
    }
    Check(blocks, "sparse block sizes are the old ones for every alignment");
    Check(Layout::NumPages(32) == 256 && Layout::NumPages(30) == 1024, "sparse page counts over the 40-bit space");
}

void TestResidentLayoutRespectsDriverLimits() {
    Check(Layout::PageBits(false, ~u64{0}, ~u64{0}) == Layout::RESIDENT_PAGE_BITS && Layout::RESIDENT_PAGE_BITS == 24,
          "fully resident pages are 16 MiB without a limit");
    Check(Layout::PageBits(false, 8 * MiB, ~u64{0}) == 23 && Layout::PageBits(false, 3 * MiB, ~u64{0}) == 21,
          "the buffer size limit alone shrinks the page");
    Check(Layout::PageBits(false, ~u64{0}, 4 * MiB) == 22 && Layout::PageBits(false, 4 * GiB, 12 * MiB) == 23,
          "the allocation size limit alone shrinks the page");
    bool within = true;
    for (u64 max_buffer = MiB; max_buffer <= 8 * GiB; max_buffer += 3 * MiB + 4096) {
        for (const u64 max_allocation : {MiB, 5 * MiB, 64 * MiB, 2 * GiB, ~u64{0}}) {
            const u64 bits = Layout::PageBits(false, max_buffer, max_allocation);
            const u64 page = u64{1} << bits;
            within &= IsPow2(page) && bits >= Layout::MIN_RESIDENT_PAGE_BITS && bits <= Layout::RESIDENT_PAGE_BITS &&
                      page <= max_buffer && page <= max_allocation &&
                      Layout::ResidentPageFits(bits, max_buffer, max_allocation);
        }
    }
    Check(within, "every fully resident page is a power of two within both driver limits");
    bool block = true;
    for (u64 alignment = 1; alignment <= 64_KB; alignment <<= 1) {
        block &= Layout::BlockSize(false, alignment) == 64_KB;
    }
    Check(block && Layout::BlockSize(false, 256_KB) == 256_KB, "fully resident blocks are 64 KiB unless aligned coarser");
}

// A driver whose limits are below the smallest fully resident page is refused, never wrapped.
void TestImpossibleResidentLimitsAreRefused() {
    Check(Layout::PageBits(false, 64_KB, 64_KB) == Layout::MIN_RESIDENT_PAGE_BITS, "the page never shrinks below 1 MiB");
    Check(!Layout::ResidentPageFits(Layout::MIN_RESIDENT_PAGE_BITS, 64_KB, ~u64{0}) &&
              !Layout::ResidentPageFits(Layout::MIN_RESIDENT_PAGE_BITS, ~u64{0}, MiB - 1) &&
              !Layout::ResidentPageFits(Layout::MIN_RESIDENT_PAGE_BITS, 0, 0),
          "limits below 1 MiB are refused");
    Check(Layout::ResidentPageFits(Layout::MIN_RESIDENT_PAGE_BITS, MiB, MiB), "exactly 1 MiB fits");
    Check(Layout::ResidentArenaLimit(2 * GiB, 4 * GiB) == 2 * GiB && Layout::ResidentArenaLimit(~u64{0}, GiB) == GiB,
          "an arena is bounded by the lower of the two limits");
}

// The BDA page table holds one device address per block of the 40-bit space: 128 MiB at 64 KiB blocks, which a fully
// resident cache keeps on a small UMA carve-out (Steam Deck) instead of the 512 MiB 16 KiB blocks would cost.
void TestBdaPageTableSize() {
    Check(Layout::BdaPageTableSize(64_KB) == 128 * MiB, "128 MiB BDA page table at 64 KiB blocks");
    Check(Layout::BdaPageTableSize(Layout::BlockSize(false, 4)) == 128 * MiB &&
              Layout::BdaPageTableSize(Layout::BlockSize(false, 64_KB)) == 128 * MiB,
          "fully resident arenas keep the 128 MiB BDA page table for every small alignment");
    Check(Layout::BdaPageTableSize(Layout::BlockSize(true, 64_KB)) == 128 * MiB,
          "sparse arenas with 64 KiB sparse blocks keep their 128 MiB table");
    bool consistent = true;
    for (u64 bits = Layout::MIN_RESIDENT_PAGE_BITS; bits <= Layout::RESIDENT_PAGE_BITS; ++bits) {
        const u64 block = Layout::BlockSize(false, 256);
        const u64 per_page = (u64{1} << bits) / block;
        consistent &= per_page * Layout::NumPages(bits) * 8 == Layout::BdaPageTableSize(block);
    }
    Check(consistent, "blocks per page times pages is the BDA table BufferCache allocates");
}

void SetForceEnvironment(const char* value) {
#ifdef _WIN32
    _putenv_s(Layout::FORCE_NON_SPARSE_ENV, value ? value : "");
#else
    if (value) {
        setenv(Layout::FORCE_NON_SPARSE_ENV, value, 1);
    } else {
        unsetenv(Layout::FORCE_NON_SPARSE_ENV);
    }
#endif
}

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

void TestModeSelection() {
    Check(std::string{Layout::FORCE_NON_SPARSE_ENV} == "SOS_FORCE_NON_SPARSE_ARENAS", "the variable is SOS_FORCE_NON_SPARSE_ARENAS");
    const struct {
        bool supported;
        const char* env;
        ArenaMode expected;
    } cases[] = {
        {true, nullptr, ArenaMode::Sparse},
        {true, "", ArenaMode::Sparse},
        {true, "0", ArenaMode::Sparse},
        {true, "1", ArenaMode::FullyResidentForced},
        {true, "true", ArenaMode::FullyResidentForced},
        {true, "yes", ArenaMode::FullyResidentForced},
        {true, "00", ArenaMode::FullyResidentForced},
        {true, " ", ArenaMode::FullyResidentForced},
        {false, nullptr, ArenaMode::FullyResidentUnsupported},
        {false, "", ArenaMode::FullyResidentUnsupported},
        {false, "0", ArenaMode::FullyResidentUnsupported},
        {false, "1", ArenaMode::FullyResidentForced},
    };
    for (const auto& c : cases) {
        const std::string label = std::string(c.supported ? "supported" : "unsupported") + ", variable " +
                                  (c.env ? "\"" + std::string(c.env) + "\"" : std::string("unset"));
        Check(Layout::SelectArenaMode(c.supported, c.env) == c.expected, "mode: " + label);
        Check(Layout::IsFullyResident(c.expected) == (c.expected != ArenaMode::Sparse), "fully resident: " + label);
    }

    // The helper BufferCache calls reads the real variable.
    const char* saved = std::getenv(Layout::FORCE_NON_SPARSE_ENV);
    const std::string saved_copy = saved ? saved : "";
    for (const auto& c : cases) {
        SetForceEnvironment(c.env);
        const char* seen = std::getenv(Layout::FORCE_NON_SPARSE_ENV);
        Check(Layout::SelectArenaModeFromEnvironment(c.supported) == Layout::SelectArenaMode(c.supported, seen),
              "the environment helper agrees with the pure selection");
        if (c.env && *c.env) {
            Check(Layout::SelectArenaModeFromEnvironment(c.supported) == c.expected,
                  std::string("the environment value \"") + c.env + "\" selects its mode");
        }
    }
    SetForceEnvironment(nullptr);
    Check(Layout::SelectArenaModeFromEnvironment(true) == ArenaMode::Sparse, "unset: sparse on a sparse device");
    SetForceEnvironment("1");
    Check(Layout::SelectArenaModeFromEnvironment(true) == ArenaMode::FullyResidentForced, "1 forces fully resident arenas");
    SetForceEnvironment("0");
    Check(Layout::SelectArenaModeFromEnvironment(true) == ArenaMode::Sparse, "0 keeps sparse arenas");
    SetForceEnvironment(saved ? saved_copy.c_str() : nullptr);

    // The production capability check (Instance::IsSparseBufferBindingSupported): the device features and its
    // graphics queue all have to offer sparse binding.
    using Vulkan::GpuCompat::SparseBuffersSupported;
    const auto mode = [](bool binding, bool residency, bool queue) {
        return Layout::SelectArenaMode(SparseBuffersSupported(binding, residency, queue), nullptr);
    };
    Check(mode(true, true, true) == ArenaMode::Sparse, "a sparse device and queue keep sparse arenas");
    Check(mode(false, true, true) == ArenaMode::FullyResidentUnsupported, "no sparseBinding: fully resident");
    Check(mode(true, false, true) == ArenaMode::FullyResidentUnsupported, "no sparseResidencyBuffer: fully resident");
    Check(mode(true, true, false) == ArenaMode::FullyResidentUnsupported,
          "a queue without VK_QUEUE_SPARSE_BINDING_BIT: fully resident, never sparse");

    // The startup line names the reason and the arena size.
    const std::string unsupported = Layout::FullyResidentStartupLine(
        ArenaMode::FullyResidentUnsupported, 16, Vulkan::GpuCompat::MissingSparseRequirements(true, true, false));
    Check(Has(unsupported, "GPU: buffer cache: sparse buffers unavailable (missing ") &&
              Has(unsupported, "VK_QUEUE_SPARSE_BINDING_BIT") && Has(unsupported, "using 16 MiB fully resident arenas"),
          "the unsupported line names what is missing and the arena size: " + unsupported);
    const std::string forced = Layout::FullyResidentStartupLine(ArenaMode::FullyResidentForced, 4, "");
    Check(Has(forced, "SOS_FORCE_NON_SPARSE_ARENAS set") && Has(forced, "using 4 MiB fully resident arenas") &&
              !Has(forced, "missing"),
          "the forced line names the variable and the arena size: " + forced);
}

// ---------------------------------------------------------------------------------------------
// Fully resident arenas on fake arenas
// ---------------------------------------------------------------------------------------------

struct FakeArena {
    u64 cpu_addr;
    u64 size;
    u64 device_address;
    std::map<u64, u8> bytes; // written offsets only
    u64 CpuAddr() const {
        return cpu_addr;
    }
    u64 SizeBytes() const {
        return size;
    }
};

// Like BufferCache::Backing in fully resident arenas: adjacent ranges stay separate.
struct ResidentRange : Interval {
    constexpr bool CanMergeWith(const ResidentRange&) const noexcept {
        return false;
    }
    constexpr ResidentRange SubRange(u64 a, u64 b) const noexcept {
        return {{a, b}};
    }
};

// BufferCache's fully resident mode on fake arenas: GetResidentArena, EnsureResident, ObtainBuffer with
// FollowMigration, RefreshBinding and RetireMigratedArenas.
struct World {
    static constexpr u64 PageBits = Layout::RESIDENT_PAGE_BITS;
    static constexpr u64 BlockShift = 16;
    static constexpr u64 PageSize = u64{1} << PageBits;

    std::list<FakeArena> arenas;
    std::vector<FakeArena*> owners = std::vector<FakeArena*>(Layout::NumPages(PageBits));
    IntervalList<ResidentRange> resident;
    std::unordered_map<u64, u64> bda;
    std::vector<FakeArena*> retired;
    std::vector<FakeArena*> destroyed;
    std::vector<std::string> events;
    std::vector<std::pair<u64, u64>> copied_blocks;
    u64 next_device_address = 0x7000'0000'0000;
    u64 generation = 0;
    u64 limit = ~u64{0};
    u64 over_limit = 0;
    // Runs once inside the next Obtain, between residency and synchronization (a fault processed while that
    // ObtainBuffer waits).
    std::function<void()> nested;

    FakeArena* Create(u64 cpu_addr, u64 size) {
        over_limit += size > limit;
        arenas.push_back({cpu_addr, size, next_device_address, {}});
        next_device_address += size + GiB;
        events.push_back("create");
        return &arenas.back();
    }
    void Copy(FakeArena& from, FakeArena& to, u64 start, u64 end) {
        const u64 first = (start << BlockShift) - from.cpu_addr;
        const u64 last = (end << BlockShift) - from.cpu_addr;
        for (auto it = from.bytes.lower_bound(first); it != from.bytes.end() && it->first < last; ++it) {
            to.bytes[it->first + from.cpu_addr - to.cpu_addr] = it->second;
        }
        copied_blocks.emplace_back(start, end);
        events.push_back("copy");
    }
    void Publish(FakeArena& arena, u64 start, u64 end) {
        for (u64 block = start; block < end; ++block) {
            bda[block] = Layout::BlockDeviceAddress(arena.device_address, arena.cpu_addr, block, BlockShift);
        }
        events.push_back("publish");
    }
    void Retire(FakeArena& arena) {
        retired.push_back(&arena);
        events.push_back("retire");
    }

    FakeArena* GetArena(u64 first_block, u64 last_block) {
        const size_t before = retired.size();
        FakeArena* arena = Layout::ResidentArenaFor<FakeArena>(owners, resident, PageBits, BlockShift, first_block,
                                                               last_block, *this);
        generation += retired.size() != before;
        return arena;
    }

    void EnsureResident(FakeArena* arena, u64 first_block, u64 last_block) {
        std::vector<std::pair<u64, u64>> gaps;
        resident.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) { gaps.emplace_back(start, end); });
        for (const auto& [start, end] : gaps) {
            resident.Add(ResidentRange{{start, end}});
            Publish(*arena, start, end);
        }
    }

    std::pair<FakeArena*, u64> Obtain(u64 addr, u64 size, u8 write = 0) {
        const u64 first = addr >> BlockShift;
        const u64 last = (addr + size - 1) >> BlockShift;
        FakeArena* arena = GetArena(first, last);
        u64 seen = generation;
        EnsureResident(arena, first, last);
        if (auto hook = std::exchange(nested, nullptr)) {
            hook();
        }
        if (write) {
            Write(*arena, addr, size, write); // an upload recorded into the arena held
        }
        while (seen != generation) {
            seen = generation;
            arena = Follow(arena, addr, size, first, last);
        }
        return {arena, addr - arena->cpu_addr};
    }

    FakeArena* Follow(FakeArena* stale, u64 addr, u64 size, u64 first, u64 last) {
        FakeArena* current = GetArena(first, last);
        if (current == stale) {
            return current;
        }
        EnsureResident(current, first, last);
        for (u64 a = addr; a < addr + size; ++a) {
            if (const auto it = stale->bytes.find(a - stale->cpu_addr); it != stale->bytes.end()) {
                current->bytes[a - current->cpu_addr] = it->second;
            }
        }
        Publish(*current, first, last + 1);
        return current;
    }

    static u8 ByteAt(const FakeArena& arena, u64 addr) {
        const auto it = arena.bytes.find(addr - arena.cpu_addr);
        return it == arena.bytes.end() ? 0 : it->second;
    }

    static void Write(FakeArena& arena, u64 addr, u64 size, u8 value) {
        for (u64 a = addr; a < addr + size; ++a) {
            arena.bytes[a - arena.cpu_addr] = value;
        }
    }

    // What a shader reads through the BDA page table, or 0 when the block has no entry.
    u8 ReadThroughBda(u64 addr) const {
        const auto entry = bda.find(addr >> BlockShift);
        if (entry == bda.end()) {
            return 0;
        }
        const u64 device = entry->second + (addr & ((u64{1} << BlockShift) - 1));
        for (const auto& arena : arenas) {
            if (device >= arena.device_address && device < arena.device_address + arena.size) {
                const auto it = arena.bytes.find(device - arena.device_address);
                return it == arena.bytes.end() ? 0 : it->second;
            }
        }
        return 0xEE; // an address into no arena
    }

    std::pair<const FakeArena*, u64> Refresh(const FakeArena* arena, u64 offset) const {
        return Layout::CurrentBinding<FakeArena>(owners, retired, PageBits, arena, offset);
    }

    void RetireMigrated() {
        destroyed.insert(destroyed.end(), retired.begin(), retired.end());
        retired.clear();
    }
};

void TestRequestInsideOneArenaReusesIt() {
    World w;
    const u64 base = 0x10'0000'0000;
    auto [a, a_offset] = w.Obtain(base + 0x1000, 0x100);
    auto [b, b_offset] = w.Obtain(base + 0x20000, 0x4000);
    Check(a == b && w.arenas.size() == 1 && w.generation == 0, "requests in one page share its arena, no migration");
    Check(a->size == World::PageSize && a_offset == 0x1000 && b_offset == 0x20000,
          "a new arena is one 16 MiB page, offsets are guest-relative");
    Check(w.copied_blocks.empty() && w.retired.empty(), "nothing copied or retired without a migration");
}

// Two arenas on neighbouring pages and a buffer across their boundary.
void TestSpanningTwoArenasMigrates() {
    World w;
    const u64 boundary = 0x10'0100'0000;
    auto [low, low_offset] = w.Obtain(boundary - 0x30000, 0x10000);
    auto [high, high_offset] = w.Obtain(boundary + 0x50000, 0x10000);
    World::Write(*low, boundary - 0x30000, 0x10, 0xA1);
    World::Write(*high, boundary + 0x50000, 0x10, 0xB2);
    Check(low != high && w.arenas.size() == 2, "neighbouring pages get separate arenas");
    Check(w.bda[(boundary - 0x30000) >> World::BlockShift] == low->device_address + low_offset,
          "a resident block's BDA entry points into its arena");
    (void)high_offset;

    w.events.clear();
    w.GetArena((boundary - 0x8000) >> World::BlockShift, (boundary + 0x7FFF) >> World::BlockShift);
    const std::vector<std::string> ev = w.events;
    auto [merged, merged_offset] = w.Obtain(boundary - 0x8000, 0x10000);
    Check(merged != low && merged != high && w.arenas.size() == 3 && w.generation == 1,
          "a request across two arenas migrates them into a new one and bumps the generation once");
    Check(merged->cpu_addr == boundary - World::PageSize && merged->size == 2 * World::PageSize &&
              merged_offset == World::PageSize - 0x8000,
          "the new arena covers both pages");
    Check(w.owners[(boundary - 1) >> World::PageBits] == merged && w.owners[boundary >> World::PageBits] == merged,
          "both pages are repointed at the new arena");

    Check(World::ByteAt(*merged, boundary - 0x30000) == 0xA1 && World::ByteAt(*merged, boundary + 0x50000) == 0xB2,
          "resident data of both old arenas moved");
    Check(w.ReadThroughBda(boundary - 0x30000 + 0xF) == 0xA1 && w.ReadThroughBda(boundary + 0x50000 + 0x7) == 0xB2,
          "shaders reading through the BDA page table see the moved data");
    bool published = true;
    w.resident.ForEachInRange(0, ~u64{0} >> World::BlockShift, [&](const ResidentRange& range) {
        for (u64 block = range.start; block < range.end; ++block) {
            published &= w.bda.at(block) == merged->device_address + (block << World::BlockShift) - merged->cpu_addr;
        }
    });
    Check(published, "every moved block's BDA entry points at the new arena at its guest-relative offset");

    Check(w.retired.size() == 2, "both old arenas are retired");
    size_t first_retire = ev.size(), last_publish = 0;
    for (size_t i = 0; i < ev.size(); ++i) {
        if (ev[i] == "retire") {
            first_retire = std::min(first_retire, i);
        } else if (ev[i] == "publish") {
            last_publish = i;
        }
    }
    Check(!ev.empty() && ev.front() == "create" && first_retire < ev.size() && last_publish > 0 &&
              last_publish < first_retire && std::count(ev.begin(), ev.end(), "copy") == 2,
          "create, copy every source, publish, and only then retire");
}

// Pages never made resident are never copied as data.
void TestHolesAreNotCopied() {
    World w;
    const u64 boundary = 0x10'0300'0000;
    w.Obtain(boundary - 0x400000, 0x10000);
    w.Obtain(boundary + 0x200000, 0x20000);
    w.Obtain(boundary - 0x8000, 0x10000);
    u64 copied = 0;
    bool resident = true;
    for (const auto& [start, end] : w.copied_blocks) {
        copied += end - start;
        for (u64 block = start; block < end; ++block) {
            resident &= w.resident.Contains(block);
        }
    }
    Check(resident && copied == 1 + 2, "only resident blocks are copied in a migration");
}

// A command resolved arena A, then a fault processed while it waited merged A away.
void TestNestedMigrationFollowsStaleBinding() {
    World w;
    const u64 boundary = 0x10'0700'0000;
    const u64 addr = boundary - 0x200000;
    auto [first, first_offset] = w.Obtain(addr, 0x100);
    w.Obtain(boundary + 0x100000, 0x100);
    w.Obtain(boundary - 0x10, 0x20);
    const auto [current, offset] = w.Refresh(first, first_offset);
    Check(current != first && current == w.owners[addr >> World::PageBits] && current->cpu_addr + offset == addr,
          "a stale binding is followed to the arena now holding its address");
    const FakeArena* other = w.Obtain(0x20'0000'0000, 0x100).first;
    Check((w.Refresh(other, 0x40) == std::pair<const FakeArena*, u64>{other, 0x40}),
          "bindings into arenas no migration replaced are left alone");

    // A fault inside ObtainBuffer merges the arena it resolved: the upload it records into the stale arena reaches
    // the arena returned, and the shaders' BDA entries.
    World n;
    const u64 b2 = 0x10'0900'0000;
    n.Obtain(b2 + 0x10000, 0x100);
    n.nested = [&] { n.Obtain(b2 - 0x10, 0x20); };
    const auto [result, result_offset] = n.Obtain(b2 - 0x40000, 0x80, 0x5C);
    Check(n.generation == 1 && result == n.owners[(b2 - 1) >> World::PageBits] && result->size == 2 * World::PageSize &&
              result->cpu_addr + result_offset == b2 - 0x40000,
          "ObtainBuffer returns the arena a nested migration created");
    Check(World::ByteAt(*result, b2 - 0x40000) == 0x5C && n.ReadThroughBda(b2 - 0x40000 + 0x7F) == 0x5C,
          "the upload into the stale arena is carried into the new one and its BDA entries");

    // Chains: A into B into C still resolve to C.
    World c;
    const u64 b3 = 0x10'0B00'0000;
    auto [a, a_offset] = c.Obtain(b3 - 0x100000, 0x100);
    c.Obtain(b3 + 0x100000, 0x100);
    c.Obtain(b3 - 0x10, 0x20);
    c.Obtain(b3 + World::PageSize + 0x100, 0x100);
    c.Obtain(b3 + World::PageSize - 0x10, 0x20);
    const auto [chained, chained_offset] = c.Refresh(a, a_offset);
    Check(c.generation == 2 && chained == c.owners[b3 >> World::PageBits] && chained->cpu_addr + chained_offset == b3 - 0x100000,
          "a binding through two migrations resolves to the last arena");
}

void TestRetiredArenasLiveUntilRetirement() {
    World w;
    const u64 boundary = 0x10'0D00'0000;
    auto [low, low_offset] = w.Obtain(boundary - 0x10000, 0x100);
    w.Obtain(boundary + 0x10000, 0x100);
    w.Obtain(boundary - 0x10, 0x20);
    Check(w.destroyed.empty() && w.retired.size() == 2, "replaced arenas are retired, not destroyed, during the command list");
    Check(w.Refresh(low, low_offset).first != low, "until the command list ends a stale arena resolves to its replacement");
    w.RetireMigrated();
    Check(w.destroyed.size() == 2 && w.retired.empty(), "retirement between command lists frees them");
}

// Readback windows crossing pages without an arena and several owned arenas (BufferCache::DownloadOwnedRange).
void TestOwnedRunsForReadback() {
    World w;
    const u64 base = 0x10'1000'0000;
    w.Obtain(base + 0x100, 0x100);                       // page P
    w.Obtain(base + 2 * World::PageSize + 0x100, 0x100); // page P+2, P+1 has no arena
    w.Obtain(base + 3 * World::PageSize + 0x100, 0x100); // page P+3, its own arena
    std::span<FakeArena* const> owners{w.owners};
    std::vector<std::tuple<const FakeArena*, u64, u64>> runs;
    u64 start = base + 0x80000;
    const u64 end = base + 3 * World::PageSize + 0x2000;
    bool clipped = true;
    for (;;) {
        const auto run = Layout::NextOwnedRun<FakeArena>(owners, World::PageBits, start, end);
        if (!run.arena) {
            break;
        }
        clipped &= run.start >= run.arena->cpu_addr && run.end <= run.arena->cpu_addr + run.arena->size;
        runs.emplace_back(run.arena, run.start, run.end);
        start = run.end;
    }
    Check(clipped, "every readback run lies inside its arena");
    Check(runs.size() == 3 &&
              runs[0] == std::make_tuple(w.owners[base >> World::PageBits], base + 0x80000, base + World::PageSize) &&
              std::get<1>(runs[1]) == base + 2 * World::PageSize && std::get<2>(runs[1]) == base + 3 * World::PageSize &&
              std::get<1>(runs[2]) == base + 3 * World::PageSize && std::get<2>(runs[2]) == end,
          "readback skips the unowned page and splits at arena ends");
    Check(Layout::NextOwnedRun<FakeArena>(owners, World::PageBits, 0x30'0000'0000, 0x30'0400'0000).arena == nullptr,
          "nothing owned: no run");
    const u64 top = u64{1} << Layout::ADDRESS_SPACE_BITS;
    Check(Layout::NextOwnedRun<FakeArena>(owners, World::PageBits, top - 0x1000, top).arena == nullptr &&
              Layout::NextOwnedRun<FakeArena>(owners, World::PageBits, top - 0x1000, top + 0x1000).arena == nullptr,
          "the top of the address space and past it are safe");
}

// The highest guest pages: no wrapped arena size or page index.
void TestTopOfAddressSpace() {
    World w;
    const u64 top = u64{1} << Layout::ADDRESS_SPACE_BITS;
    auto [high, high_offset] = w.Obtain(top - 0x1000, 0x1000);
    auto [low, low_offset] = w.Obtain(top - World::PageSize - 0x1000, 0x1000);
    auto [merged, offset] = w.Obtain(top - World::PageSize - 0x10, 0x20);
    Check(high != low && merged->cpu_addr == top - 2 * World::PageSize && merged->cpu_addr + merged->size == top &&
              offset == World::PageSize - 0x10,
          "a migration at the top of the address space does not wrap");
    Check(w.bda.at((top - 0x1000) >> World::BlockShift) ==
              merged->device_address + (((top - 0x1000) >> World::BlockShift) << World::BlockShift) - merged->cpu_addr,
          "the top block is published at the merged arena");
    (void)high_offset;
    (void)low_offset;
}

// Random buffers over a GPU-visible direct memory block under AMD-like limits: every binding is exact, every resident
// block is published at its owner, and an arena past the driver limit is visible to the cache (which refuses it).
void TestRandomBuffersStayConsistent() {
    World w;
    w.limit = Layout::ResidentArenaLimit(u64{2} << 30, 4 * GiB);
    std::mt19937_64 rng{0x5050A2};
    std::uniform_int_distribution<u64> where(0x10'0000'0000, 0x11'2800'0000 - 0x100000);
    std::uniform_int_distribution<u64> size(4, 0x40000);
    u64 largest = 0;
    bool exact = true;
    for (int i = 0; i < 4000; ++i) {
        const u64 address = where(rng) & ~u64{3};
        const auto [arena, offset] = w.Obtain(address, size(rng));
        exact &= arena->cpu_addr + offset == address;
        largest = std::max(largest, arena->size);
    }
    std::printf("fully resident random buffers: %zu arenas created, %llu migrations, largest %llu MiB, %llu over the "
                "%llu MiB limit\n",
                w.arenas.size(), (unsigned long long)w.generation, (unsigned long long)(largest >> 20),
                (unsigned long long)w.over_limit, (unsigned long long)(w.limit >> 20));
    Check(exact, "every random binding resolves to its own guest address");
    Check((largest > w.limit) == (w.over_limit != 0), "an arena past the driver limit is always seen by Create");
    bool published = true;
    w.resident.ForEachInRange(0, ~u64{0} >> World::BlockShift, [&](const ResidentRange& range) {
        const FakeArena* owner = w.owners[(range.start << World::BlockShift) >> World::PageBits];
        published &= owner && w.bda.at(range.start) ==
                                  owner->device_address + (range.start << World::BlockShift) - owner->cpu_addr;
    });
    Check(published, "every resident block is published at its current owner");
}

// TextureCache::RefreshImage on a GPU-modified range: ObtainBufferForImage resolves the arena, DetileImage's params
// copy may wait for the GPU (faults processed then), the binding is refreshed, then the dispatch reads it.
void TestTextureUploadSourceAcrossMigration() {
    const u64 boundary = 0x10'1100'0000;
    const u64 image = boundary - 0x80000;
    World w;
    const auto [arena, offset] = w.Obtain(image, 0x40000);
    World::Write(*arena, image, 0x40000, 0x11);
    w.Obtain(boundary + 0x10000, 0x100);
    // The wait: a fault merges the image's arena with its neighbour, newer guest data lands in the new arena.
    const auto [merged, merged_offset] = w.Obtain(boundary - 0x10, 0x20);
    World::Write(*merged, image, 0x40000, 0x22);
    (void)merged_offset;
    const auto [source, source_offset] = w.Refresh(arena, offset);
    Check(source != arena && source == w.owners[image >> World::PageBits] && source->cpu_addr + source_offset == image &&
              World::ByteAt(*source, source->cpu_addr + source_offset) == 0x22,
          "the detile reads the current arena at the same guest offset");
    Check(World::ByteAt(*arena, image) == 0x11, "read through the stale arena, the upload would take the old bytes");
    Check(std::ranges::find(w.retired, source) == w.retired.end() && w.destroyed.empty(),
          "the source is never a retired arena; the stale one lives until the command list ends");

    // Sparse arenas never migrate: nothing is ever replaced, so a binding is left as is.
    FakeArena sparse{0x10'0000'0000, 2 * GiB, 0x5000'0000'0000, {}};
    const std::vector<FakeArena*> none;
    const auto kept = Layout::CurrentBinding<FakeArena>(w.owners, none, World::PageBits, &sparse, 0x1234);
    Check(kept.first == &sparse && kept.second == 0x1234, "a sparse binding is never redirected");
}

// A command buffer as the scheduler records it: binds are per command buffer, a submit starts a new one with none.
struct Commands {
    struct Bind {
        const FakeArena* arena;
        u64 offset;
    };
    std::optional<Bind> vertex, index;
    u32 vertex_binds = 0, index_binds = 0;
    void Submit() {
        vertex.reset();
        index.reset();
    }
};

// DrawIndirectRecord: resolve vertex and index buffers, look up the indirect arguments (which may submit and process
// faults), then emit the binds (the old order: before the lookup) and draw.
bool ModelIndirectDraw(bool emit_before_lookup, bool lookup_submits, bool lookup_migrates) {
    World w;
    Commands cmd;
    const u64 boundary = 0x10'1500'0000;
    const u64 vertices = boundary - 0x100000, indices = boundary - 0x200000;
    w.Obtain(boundary + 0x100, 0x100);
    auto [vb, vb_offset] = w.Obtain(vertices, 0x1000);
    auto [ib, ib_offset] = w.Obtain(indices, 0x100);
    const u64 generation = w.generation;
    const auto emit = [&] {
        const auto [v, vo] = w.Refresh(vb, vb_offset);
        const auto [i, io] = w.Refresh(ib, ib_offset);
        cmd.vertex = Commands::Bind{v, vo};
        cmd.index = Commands::Bind{i, io};
        ++cmd.vertex_binds;
        ++cmd.index_binds;
    };
    if (emit_before_lookup) {
        emit();
    }
    if (lookup_submits) {
        cmd.Submit();
    }
    if (lookup_migrates) {
        w.Obtain(boundary - 0x10, 0x20);
    }
    if (!emit_before_lookup) {
        emit();
    }
    const auto current = [&](u64 addr, const std::optional<Commands::Bind>& bound) {
        return bound && bound->arena == w.owners[addr >> World::PageBits] && bound->arena->cpu_addr + bound->offset == addr;
    };
    return current(vertices, cmd.vertex) && current(indices, cmd.index) && cmd.vertex_binds == 1 &&
           cmd.index_binds == 1 && (lookup_migrates == (w.generation != generation));
}

void TestIndirectDrawBindOrder() {
    bool after = true;
    for (const bool submits : {false, true}) {
        for (const bool migrates : {false, true}) {
            after &= ModelIndirectDraw(false, submits, migrates);
        }
    }
    Check(after, "binds emitted after the argument lookup survive a submit and a migration in it");
    Check(ModelIndirectDraw(true, false, false) && !ModelIndirectDraw(true, true, false) &&
              !ModelIndirectDraw(true, false, true),
          "the old order loses its binds to a submit, or keeps a migrated arena");
}

// ---------------------------------------------------------------------------------------------
// Source contracts: the parts that need a Vulkan device
// ---------------------------------------------------------------------------------------------

std::string Source(const char* path) {
    std::ifstream in(fs::path(SOS_STRING(SOS_SOURCE_DIR)) / path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::erase(text, '\r');
    return text;
}

// Body of the function whose definition starts with `signature`, up to its closing brace at column 0.
std::string Body(const std::string& source, const std::string& signature) {
    const auto start = source.find(signature);
    if (start == std::string::npos) {
        return {};
    }
    const auto end = source.find("\n}\n", start);
    return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// The block opened by `opening`, up to its closing brace at one indentation level.
std::string Block(const std::string& body, const std::string& opening) {
    const auto start = body.find(opening);
    if (start == std::string::npos) {
        return {};
    }
    const auto end = body.find("\n    }\n", start);
    return body.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

bool Ordered(const std::string& text, const std::string& first, const std::string& second) {
    const auto a = text.find(first);
    return a != std::string::npos && text.find(second, a + first.size()) != std::string::npos;
}

void TestBufferCacheSources() {
    const std::string cache = Source("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp");
    const std::string header = Source("gpu/shadps4/video_core/buffer_cache/buffer_cache.h");
    const std::string buffer = Source("gpu/shadps4/video_core/buffer_cache/buffer.cpp");
    const std::string layout = Source("gpu/shadps4/video_core/buffer_cache/arena_layout.h");
    const std::string instance_h = Source("gpu/shadps4/video_core/renderer_vulkan/vk_instance.h");
    Check(!cache.empty() && !header.empty() && !buffer.empty() && !layout.empty() && !instance_h.empty(),
          "the buffer cache sources are readable");

    // The old stop is gone; the mode comes from the production capability check.
    Check(!Has(cache, "cannot run the game: sparse buffers") && !Has(cache, "UNREACHABLE_MSG(\"GPU: this Vulkan device"),
          "missing sparse support no longer stops the game");
    Check(Has(instance_h, "bool IsSparseBufferBindingSupported() const {") &&
              Has(instance_h, "GpuCompat::SparseBuffersSupported(features.sparseBinding,"),
          "the instance's sparse check is the tested GpuCompat one");
    const std::string ctor = Body(cache, "BufferCache::BufferCache(");
    Check(Has(ctor, "ArenaLayout::SelectArenaModeFromEnvironment(instance.IsSparseBufferBindingSupported())") &&
              Has(ctor, "sparse_arenas = !ArenaLayout::IsFullyResident(arena_mode);"),
          "the constructor selects the arena mode from the device, its queue and the variable");
    Check(Count(cache, "std::getenv(") == 0 && Count(layout, "std::getenv(FORCE_NON_SPARSE_ENV)") == 1,
          "the variable is read only through the tested helper");
    Check(Count(cache, "ArenaMode::FullyResidentForced") == 0 && Count(cache, "ArenaMode::FullyResidentUnsupported") == 0 &&
              Count(cache, "arena_mode") == 3,
          "forced and unsupported share one implementation: the reason only picks the startup line");
    Check(Has(Block(ctor, "if (!sparse_arenas) {"), "ArenaLayout::ResidentPageFits(") &&
              Has(Block(ctor, "if (!sparse_arenas) {"), "ArenaLayout::FullyResidentStartupLine(") &&
              Has(Block(ctor, "if (!sparse_arenas) {"), "instance.MissingSparseBufferRequirements()") &&
              Has(Block(ctor, "if (!sparse_arenas) {"), "std::printf(\"%s\\n\""),
          "fully resident mode checks the page fits and prints its startup line");
    Check(Has(ctor, "ArenaLayout::PageBits(sparse_arenas, max_buffer_size, max_allocation_size)") &&
              Has(ctor, "ArenaLayout::BlockSize(sparse_arenas, reqs.alignment)"),
          "page and block sizes come from the tested layout");
    Check(Has(ctor, "BDA page \"\n                \"table %llu MiB") && Has(ctor, "sparse_arenas ? \"sparse\" : \"fully resident\""),
          "the arena log line is kept and names the mode");

    // The sparse path as it was on devices that support it.
    Check(Has(ctor, ".flags = sparse_arenas ? vk::BufferCreateFlagBits::eSparseBinding |\n"
                    "                                     vk::BufferCreateFlagBits::eSparseResidency"),
          "the probe is sparse in sparse mode");
    Check(Has(ctor, "FallbackMemoryTypes(instance.GetMemoryProperties(), reqs.memoryTypeBits,"),
          "arena memory types come from the low VRAM fallback order");
    const std::string get_arena = Body(cache, "const Buffer* BufferCache::GetArena(");
    Check(Ordered(get_arena, "if (!sparse_arenas) {\n        return GetResidentArena(", "address_space[first_page]") &&
              Has(get_arena, "MemoryType::Sparse") && Has(get_arena, "BindsForArena(new_arena)") &&
              Has(get_arena, "ASSERT_MSG(last_page - first_page <= 1,"),
          "sparse GetArena keeps address_space, sparse arenas and their migration binds");
    Check(Count(cache, "GetResidentArena(") == 2, "GetResidentArena: its definition and GetArena's resident branch");
    const std::string ensure = Body(cache, "void BufferCache::EnsureResident(");
    const std::string non_sparse = Block(ensure, "if (!sparse_arenas) {");
    Check(Has(non_sparse, "resident_ranges.Add(") && Has(non_sparse, "PublishArenaBda(arena,") && Has(non_sparse, "return;") &&
              !Has(non_sparse, "allocateMemory") && !Has(non_sparse, "BindsForArena"),
          "fully resident residency is bookkeeping and BDA entries only");
    Check(Ordered(ensure, "if (!sparse_arenas) {", "AllocateWithFallback(backing_memory_types") &&
              Ordered(ensure, "AllocateWithFallback(backing_memory_types", "BindsForArena(arena)") &&
              Has(ensure, "Vulkan::Check(allocation.result);") && Has(ensure, "AllocationCategory::SparseBacking"),
          "sparse residency keeps its backing allocation with the low VRAM fallback and its binds");
    Check(Ordered(Body(cache, "void BufferCache::SubmitPendingArenaBinds("), "if (pending_binds.empty()) {\n        return;",
                  "bindSparse(sparse_info)"),
          "sparse binds are still submitted, and nothing is without pending binds");
    Check(Has(header, "ASSERT(sparse_arenas);"), "sparse binds are never recorded for fully resident arenas");
    Check(Has(header, "std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};"), "sparse mode keeps address_space");
    const std::string dma = Body(cache, "void BufferCache::SynchronizeDmaBuffers(");
    Check(Has(dma, "sparse_arenas ? address_space[page] : resident_owners[page]"), "DMA sync looks arenas up per mode");

    // Fully resident arenas: device-local first with the low VRAM fallback, within the driver limit, migrated before
    // the BDA page table and barriers, retired later.
    const std::string resident = Body(cache, "const Buffer* BufferCache::GetResidentArena(");
    Check(Has(resident, "ArenaLayout::ResidentArenaFor<Buffer>(") && Has(resident, "ArenaLayout::ResidentArenaLimit(") &&
              Has(resident, "ASSERT_MSG(size <= limit") && !Has(resident, "MemoryType::Sparse") &&
              !Has(resident, "BindsForArena") && !Has(resident, "bindSparse"),
          "resident arenas are ordinary buffers within the driver limit");
    Check(Ordered(resident, "ops.FlushCopies(*arena);", "PublishArenaBda(arena,") &&
              Ordered(resident, "PublishArenaBda(arena,", "runtime.FlushBarriers();") && Has(resident, "eMemoryWrite"),
          "a migration copies, then publishes, then flushes barriers");
    Check(Has(resident, "if (retired_arenas.size() != retired) {\n        ++arena_generation;"),
          "a migration bumps the arena generation");
    Check(Count(cache, "++arena_generation;") == 1 && Count(cache, "retired_arenas.push_back(") == 1,
          "only fully resident migrations bump the generation and retire arenas");
    const std::string create = Body(cache, "Buffer* BufferCache::CreateResidentArena(");
    Check(Has(create, "std::span<const u32>{backing_memory_types}") && Has(create, "Vulkan::Check(allocation.result);") &&
              Has(create, "RecordFallback(Telemetry(), AllocationCategory::SparseBacking") &&
              Has(create, "device memory is full") && !Has(create, "MemoryType::Sparse"),
          "a resident arena is allocated device-local first with the low VRAM fallback and its telemetry");
    Check(Has(create, "resident_arena_bytes += size;") && Has(create, "GPU: fully resident buffer arenas hold %llu MiB"),
          "resident arena sizes are logged as they grow");
    const std::string in_type = Body(buffer, "vk::Result UniqueBuffer::CreateInMemoryType(");
    Check(Has(in_type, ".memoryTypeBits = memory_type < 32 ? 1u << memory_type : 0u,") &&
              !Has(in_type, "VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT") && Has(in_type, "return vk::Result{result};"),
          "each attempt allocates in exactly one memory type and returns its result");
    Check(Has(buffer, "allocation = AllocateWithFallback(memory_types, [&](u32 type) {"),
          "the resident arena buffer tries the fallback memory types in order");
    const std::string retire = Body(cache, "void BufferCache::RetireMigratedArenas(");
    Check(Has(retire, "scheduler.DeferOperation(") && Has(retire, "buffer.Destroy()") && !Has(retire, "arenas.erase"),
          "retired arenas are destroyed once the GPU is done, their entries stay");
    Check(Has(header, "std::deque<Buffer> arenas;"), "arenas live in a deque: addresses stay valid");

    // Followed migrations and readback.
    const std::string obtain = Body(cache, "std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(");
    Check(Ordered(obtain, "u64 generation = arena_generation;", "SynchronizeMemory(arena,") &&
              Ordered(obtain, "SynchronizeMemory(arena,", "while (generation != arena_generation)") &&
              Ordered(obtain, "FollowMigration(", "gpu_modified_ranges.Add("),
          "ObtainBuffer follows a migration that happened while it waited");
    Check(Ordered(Block(obtain, "while (generation != arena_generation) {"), "FollowMigration(",
                  "SynchronizeMemoryFromImage(arena, device_addr, size);"),
          "an image written into the stale arena is written again into the current one");
    const std::string read = Body(cache, "void BufferCache::ReadMemory(");
    Check(Ordered(read, "if (!sparse_arenas) {", "DownloadOwnedRange(") && Ordered(read, "DownloadOwnedRange(", "GetArena("),
          "fully resident readback walks the owned arenas instead of creating one");
    Check(Has(Body(cache, "void BufferCache::DownloadOwnedRange("), "DownloadMemory(run.arena"), "readback per owned run");
    Check(Has(Body(cache, "void BufferCache::PublishArenaBda("), "ArenaLayout::BlockDeviceAddress(arena_address, arena->cpu_addr"),
          "BDA entries use the tested address formula");
    Check(Has(Body(cache, "bool BufferCache::SynchronizeMemoryFromImage("), "TexelImageCopiesFor(image.info"),
          "texel image copies come from the shared helper");
}

void TestCallerSources() {
    const std::string r = Source("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp");
    const std::string rh = Source("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h");
    const std::string hle = Source("gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp");
    const std::string tile = Source("gpu/shadps4/video_core/texture_cache/tile_manager.cpp");
    const std::string tex = Source("gpu/shadps4/video_core/texture_cache/texture_cache.cpp");
    Check(!r.empty() && !rh.empty() && !hle.empty() && !tile.empty() && !tex.empty(), "the caller sources are readable");

    const std::string direct = Body(r, "void Rasterizer::DrawRecord(");
    Check(Ordered(direct, "ResolveIndexBuffer(index_offset);", "RefreshArenaBindings();\n    EmitVertexBuffers();") &&
              Count(direct, "RefreshArenaBindings();") == 1 && Count(direct, "EmitVertexBuffers();") == 1 &&
              Count(direct, "EmitIndexBuffer();") == 1,
          "direct draws refresh their bindings once, after the last lookup and before emitting them");
    const std::string indirect = Body(r, "void Rasterizer::DrawIndirectRecord(");
    Check(!Has(indirect, "BindVertexBuffers(") && !Has(indirect, "BindIndexBuffer(") &&
              Count(indirect, "ResolveVertexBuffers(pipeline, nullptr);") == 1 &&
              Count(indirect, "EmitVertexBuffers();") == 1 && Count(indirect, "EmitIndexBuffer();") == 1,
          "indirect draws resolve and emit their vertex and index binds once");
    Check(Ordered(indirect, "ResolveVertexBuffers(pipeline, nullptr);", "ObtainBuffer(indirect.args") &&
              Ordered(indirect, "ObtainBuffer(count_address", "RefreshArenaBindings();") &&
              Ordered(indirect, "RefreshArenaBindings();", "buffer_cache.RefreshBinding(buffer, base);") &&
              Ordered(indirect, "buffer_cache.RefreshBinding(count_buffer, count_offset);", "EmitVertexBuffers();") &&
              Ordered(indirect, "EmitIndexBuffer();", "scheduler.Record("),
          "indirect draws emit after the argument and count lookups, with refreshed argument and count buffers");
    const auto after_binds = indirect.substr(std::min(indirect.size(), indirect.find("EmitIndexBuffer();")));
    bool quiet = true;
    for (const char* call : {"ObtainBuffer(", "scheduler.Flush", "scheduler.Finish", "scheduler.Wait"}) {
        quiet &= !Has(after_binds, call) && !Has(direct.substr(std::min(direct.size(), direct.find("EmitIndexBuffer();"))), call);
    }
    Check(quiet, "nothing between the binds and the draw can submit or migrate");
    Check(!Has(r, "void Rasterizer::BindVertexBuffers(") && !Has(r, "void Rasterizer::BindIndexBuffer(") &&
              !Has(rh, "void BindVertexBuffers(") && !Has(rh, "void BindIndexBuffer("),
          "the bind-before-lookup helpers are gone");
    Check(Ordered(Body(r, "void Rasterizer::DispatchRecord("), "BindResources(pipeline)", "RefreshArenaBindings();"),
          "dispatches refresh after binding");
    const std::string dispatch_indirect = Body(r, "void Rasterizer::DispatchIndirectRecord(");
    Check(Ordered(dispatch_indirect, "ObtainBuffer(address + offset", "RefreshArenaBindings();") &&
              Ordered(dispatch_indirect, "buffer_cache.RefreshBinding(buffer, base);", "dispatchIndirect"),
          "indirect dispatches refresh their bindings and argument buffer");
    Check(Ordered(Body(r, "void Rasterizer::CopyBuffer("), "ObtainBuffer(dst", "RefreshBinding(src_buffer, src_offset)"),
          "a buffer copy refreshes its source after the destination lookup");
    Check(Ordered(hle, "ObtainBuffer(\n            dst_buf_sharp", "RefreshBinding(src_buf, src_buf_offset)"),
          "the HLE copy refreshes its source after the destination lookup");
    const std::string refresh = Body(r, "void Rasterizer::RefreshArenaBindings(");
    Check(Has(refresh, "if (buffer_cache.ArenaGeneration() == bind_generation) {\n        return;") &&
              Has(refresh, "buffer_infos") && Has(refresh, "host_buffers") && Has(refresh, "index_bind.handle") &&
              Has(refresh, "bound_buffers") && Has(refresh, "needs_barrier |="),
          "the refresh covers descriptors, vertex, index and barrier bindings, and costs one compare without a migration");
    Check(Has(Body(r, "bool Rasterizer::BindResources("), "bind_generation = buffer_cache.ArenaGeneration();"),
          "a command's bindings start at the current generation");
    Check(Ordered(Body(r, "void Rasterizer::OnSubmit("), "DrainDrawPipe();", "buffer_cache.RetireMigratedArenas();"),
          "migrated arenas are retired between command lists");

    const std::string detile = Body(tile, "std::pair<const Buffer*, u64> TileManager::DetileImageFrom(");
    Check(Ordered(detile, "stream_buffer.Copy(", "buffer_cache->RefreshBinding(in_buffer, in_offset);") &&
              Ordered(detile, "GetStagingPool().Request(", "buffer_cache->RefreshBinding(in_buffer, in_offset);") &&
              Ordered(detile, "buffer_cache->RefreshBinding(in_buffer, in_offset);", ".buffer = in_buffer->Handle(),"),
          "the detile refreshes its source after the waits and before the dispatch reads it");
    Check(Ordered(Body(tile, "void TileManager::MergeGuestPagesFrom("), "TileImage(image, download_copies",
                  "buffer_cache->RefreshBinding(guest, guest_offset);") &&
              Ordered(Body(tile, "void TileManager::MergeGuestPagesFrom("), "buffer_cache->RefreshBinding(guest, guest_offset);",
                      "runtime.CopyBuffer(guest"),
          "the tiled page merge refreshes its guest source before copying from it");
    Check(Ordered(Body(tex, "void TextureCache::RefreshImage("), "buffer_cache.ObtainBufferForImage(",
                  "tile_manager.DetileImage(buffer_cache, in_buffer, in_offset, image.info)"),
          "texture uploads detile through the buffer cache binding");
    Check(Has(tex, "tile_manager.MergeGuestPages(buffer_cache, image, buffer, offset, ranges)"),
          "tiled page merges pass the buffer cache");
}

void TestNoDebugMarkers() {
    bool clean = true;
    for (const char* file : {"gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp", "gpu/shadps4/video_core/buffer_cache/buffer_cache.h",
                             "gpu/shadps4/video_core/buffer_cache/arena_layout.h",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp"}) {
        const std::string text = Source(file);
        clean &= !text.empty() && !Has(text, "DEBUG_" "SAULO");
    }
    Check(clean, "no debug markers in the arena sources");
}

} // namespace

int main() {
    TestSparseLayoutUnchanged();
    TestResidentLayoutRespectsDriverLimits();
    TestImpossibleResidentLimitsAreRefused();
    TestBdaPageTableSize();
    TestModeSelection();
    TestRequestInsideOneArenaReusesIt();
    TestSpanningTwoArenasMigrates();
    TestHolesAreNotCopied();
    TestNestedMigrationFollowsStaleBinding();
    TestRetiredArenasLiveUntilRetirement();
    TestOwnedRunsForReadback();
    TestTopOfAddressSpace();
    TestRandomBuffersStayConsistent();
    TestTextureUploadSourceAcrossMigration();
    TestIndirectDrawBindOrder();
    TestBufferCacheSources();
    TestCallerSources();
    TestNoDebugMarkers();
    std::printf("buffer arena tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
