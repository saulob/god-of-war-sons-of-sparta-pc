// SPDX-License-Identifier: GPL-2.0-or-later
// dcp: the Vulkan-independent rules of the persistent pipeline cache: blob integrity, cache
// identity (one store per game build), the warm-up mode, the build gate that keeps the game's
// own pipelines ahead of the warm-up's, and what a failed pipeline creation leads to. How the
// files are written is cache_files.h.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace PipelineCachePolicy {

/// Largest blob the cache writes or reads; bounds every allocation made from a stored size.
constexpr std::size_t MaxBlobBytes = 64u * 1024 * 1024;

inline std::uint64_t Fnv1a(std::span<const std::uint8_t> bytes,
                           std::uint64_t hash = 0xcbf29ce484222325ull) {
    for (const auto byte : bytes) {
        hash = (hash ^ byte) * 0x100000001b3ull;
    }
    return hash;
}

namespace Detail {
template <typename T>
void Append(std::vector<std::uint8_t>& bytes, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(value));
}
template <typename T>
T At(std::span<const std::uint8_t> bytes, std::size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
} // namespace Detail

constexpr std::uint32_t SealMagic = 0x42504344; // "DCPB"
constexpr std::size_t SealSize = 16;

/// Appends magic, payload size and checksum: a truncated, zero-filled or foreign file never
/// passes Unseal.
inline void Seal(std::vector<std::uint8_t>& bytes) {
    const auto size = std::uint32_t(bytes.size());
    const auto hash = Fnv1a(bytes);
    Detail::Append(bytes, SealMagic);
    Detail::Append(bytes, size);
    Detail::Append(bytes, hash);
}

/// Strips the seal; false (bytes untouched) when the blob is not intact.
inline bool Unseal(std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < SealSize || bytes.size() - SealSize > MaxBlobBytes) {
        return false;
    }
    const std::size_t payload = bytes.size() - SealSize;
    const std::span<const std::uint8_t> all{bytes};
    if (Detail::At<std::uint32_t>(all, payload) != SealMagic ||
        Detail::At<std::uint32_t>(all, payload + 4) != payload ||
        Detail::At<std::uint64_t>(all, payload + 8) != Fnv1a(all.first(payload))) {
        return false;
    }
    bytes.resize(payload);
    return true;
}

/// One store folder per game build: 1.00 and 1.01 of a title never share one. An unknown build
/// (empty version) keeps the serial alone, the folder every build used before.
inline std::string CacheFolderName(std::string_view serial, std::string_view version) {
    std::string name{serial};
    if (!version.empty()) {
        name += '-';
        name += version;
    }
    for (auto& c : name) {
        const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.';
        if (!plain) {
            c = '_';
        }
    }
    return name;
}

/// The game build in the store identity: serial and version, kept apart by a separator.
inline std::uint64_t GameIdentity(std::string_view serial, std::string_view version) {
    const auto bytes = [](std::string_view text) {
        return std::span{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
    };
    const std::uint8_t separator = 0;
    return Fnv1a(bytes(version), Fnv1a({&separator, 1}, Fnv1a(bytes(serial))));
}

/// The GPU and driver the cache was written on.
struct GpuIdentity {
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t driver_id{};
    std::uint32_t driver_version{};
    std::uint32_t api_version{};
    bool operator==(const GpuIdentity&) const = default;
};

/// The GPU and the serialization layouts the store depends on. The store holds SPIR-V and
/// pipeline state, not driver binaries, so a driver update alone keeps it. This is exactly what
/// the single-folder store (before one store per build) recorded as its identity.
struct StoreFormat {
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t driver_id{};
    std::uint32_t shader_binary_version{};
    std::uint32_t shader_meta_version{};
    std::uint32_t pipeline_key_version{};
    bool operator==(const StoreFormat&) const = default;
};

/// What a store is a function of: the game build (GameIdentity) and its format.
struct StoreIdentity {
    std::uint64_t game{};
    StoreFormat format{};
    bool operator==(const StoreIdentity&) const = default;
};
static_assert(sizeof(StoreFormat) == 24 && sizeof(StoreIdentity) == 32);

constexpr std::uint32_t StoreIdentityMagic = 0x49504344; // "DCPI"
constexpr std::uint32_t WarmupPolicyMagic = 0x57504344;  // "DCPW"
constexpr std::uint32_t RecordVersion = 1;

namespace Detail {
template <typename T>
std::vector<std::uint8_t> EncodeRecord(std::uint32_t magic, const T& value) {
    static_assert(std::has_unique_object_representations_v<T>);
    std::vector<std::uint8_t> bytes;
    Append(bytes, magic);
    Append(bytes, RecordVersion);
    Append(bytes, std::uint32_t(sizeof(T)));
    Append(bytes, value);
    return bytes;
}
template <typename T>
std::optional<T> DecodeRecord(std::uint32_t magic, std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 12 + sizeof(T) || At<std::uint32_t>(bytes, 0) != magic ||
        At<std::uint32_t>(bytes, 4) != RecordVersion || At<std::uint32_t>(bytes, 8) != sizeof(T)) {
        return std::nullopt;
    }
    return At<T>(bytes, 12);
}
} // namespace Detail

inline std::vector<std::uint8_t> EncodeStoreIdentity(const StoreIdentity& identity) {
    return Detail::EncodeRecord(StoreIdentityMagic, identity);
}
inline std::optional<StoreIdentity> DecodeStoreIdentity(std::span<const std::uint8_t> bytes) {
    return Detail::DecodeRecord<StoreIdentity>(StoreIdentityMagic, bytes);
}
/// The record of the single-folder store: its format only, no game build.
inline std::vector<std::uint8_t> EncodeLegacyStoreIdentity(const StoreFormat& format) {
    return Detail::EncodeRecord(StoreIdentityMagic, format);
}
inline std::optional<StoreFormat> DecodeLegacyStoreIdentity(std::span<const std::uint8_t> bytes) {
    return Detail::DecodeRecord<StoreFormat>(StoreIdentityMagic, bytes);
}

inline StoreIdentity CurrentStoreIdentity(std::string_view serial, std::string_view version,
                                          const StoreFormat& format) {
    return {.game = GameIdentity(serial, version), .format = format};
}

/// Legacy: the single-folder store's record, on this GPU and format, so the only thing it does not
/// prove is the game build. Its entries are keyed by shader and pipeline hashes, so an entry of
/// the other build is never used by mistake: at most it is warmed up and not asked for.
enum class StoreVerdict { Reuse, Missing, Corrupt, Incompatible, Legacy };

inline StoreVerdict CheckStore(std::span<const std::uint8_t> stored, const StoreIdentity& current) {
    if (stored.empty()) {
        return StoreVerdict::Missing;
    }
    if (const auto identity = DecodeStoreIdentity(stored)) {
        return *identity == current ? StoreVerdict::Reuse : StoreVerdict::Incompatible;
    }
    if (const auto format = DecodeLegacyStoreIdentity(stored)) {
        return *format == current.format ? StoreVerdict::Legacy : StoreVerdict::Incompatible;
    }
    return StoreVerdict::Corrupt;
}

inline std::string_view ToString(StoreVerdict verdict) {
    switch (verdict) {
    case StoreVerdict::Reuse:
        return "compatible";
    case StoreVerdict::Missing:
        return "no identity record";
    case StoreVerdict::Corrupt:
        return "corrupt identity record";
    case StoreVerdict::Incompatible:
        return "written for another game build, GPU or cache format";
    case StoreVerdict::Legacy:
        return "written before caches were kept per game build";
    }
    return "unknown";
}

/// Reuse: as it is. Adopt: kept, with this build's identity written over the legacy one. Create:
/// an empty store is only filled. Rebuild: cleared, because it is not provably this build's.
enum class StoreAction { Reuse, Adopt, Create, Rebuild };

constexpr StoreAction DecideStoreAction(StoreVerdict verdict, bool profile_present,
                                        bool profile_match, bool store_empty) {
    if (profile_match && verdict == StoreVerdict::Reuse) {
        return StoreAction::Reuse;
    }
    if (profile_match && verdict == StoreVerdict::Legacy) {
        return StoreAction::Adopt;
    }
    return verdict == StoreVerdict::Missing && !profile_present && store_empty
               ? StoreAction::Create
               : StoreAction::Rebuild;
}

/// Whether this build's store takes over the files of the single-folder store: only when it has
/// its own folder, the old one holds a store and its own holds none yet. The first build started
/// after the update claims it; the other build starts its own.
constexpr bool AdoptLegacyFolder(bool separate_folder, bool legacy_has_store,
                                 bool own_has_store) {
    return separate_folder && legacy_has_store && !own_has_store;
}

/// Driver-dependent: recorded after a pipeline failed while the warm-up built in parallel.
struct WarmupPolicy {
    GpuIdentity gpu{};
    std::uint32_t serial_builds{};
};

inline std::vector<std::uint8_t> EncodeWarmupPolicy(const WarmupPolicy& policy) {
    return Detail::EncodeRecord(WarmupPolicyMagic, policy);
}
inline std::optional<WarmupPolicy> DecodeWarmupPolicy(std::span<const std::uint8_t> bytes) {
    return Detail::DecodeRecord<WarmupPolicy>(WarmupPolicyMagic, bytes);
}

enum class WarmupMode { Parallel, Serial };

/// Serial only on the exact GPU and driver that failed; a driver update gets parallel back.
inline WarmupMode ChooseWarmupMode(std::span<const std::uint8_t> stored, const GpuIdentity& gpu) {
    const auto policy = DecodeWarmupPolicy(stored);
    return policy && policy->serial_builds && policy->gpu == gpu ? WarmupMode::Serial
                                                                 : WarmupMode::Parallel;
}

/// Pipelines build in parallel until EnableSerial, then one at a time; in serial mode a waiting
/// game build goes ahead of every waiting warm-up build.
class BuildGate {
public:
    enum class Kind { Warmup, Game };

private:
    struct Ticket {
        bool parallel;
        bool peers;
        std::uint64_t entries;
    };

public:
    class Slot {
    public:
        Slot(BuildGate& gate_, Kind kind) : gate{gate_}, ticket{gate_.Enter(kind)} {}
        ~Slot() {
            gate.Leave(ticket.parallel);
        }
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

        bool Parallel() const {
            return ticket.parallel;
        }
        /// Another build ran at the same time as this one.
        bool Overlapped() const {
            return ticket.parallel && (ticket.peers || gate.entries.load() != ticket.entries);
        }

    private:
        BuildGate& gate;
        const Ticket ticket;
    };

    /// In serial mode: whether a build of `kind` may start now. Nothing else runs, and a warm-up
    /// build also lets every waiting game build go first.
    static constexpr bool MayStartSerial(Kind kind, unsigned parallel_builds, bool serial_build,
                                         unsigned games_waiting) {
        return parallel_builds == 0 && !serial_build && (kind == Kind::Game || games_waiting == 0);
    }

    /// True for the call that switched to serial builds.
    bool EnableSerial() {
        std::scoped_lock lock{mutex};
        return !std::exchange(serial, true);
    }
    bool Serial() const {
        std::scoped_lock lock{mutex};
        return serial;
    }
    /// Game builds waiting for a serial slot (each goes ahead of every waiting warm-up build).
    unsigned GamesWaiting() const {
        std::scoped_lock lock{mutex};
        return games_waiting;
    }
    /// False when builds still run in parallel after `limit`.
    bool WaitForParallelBuilds(std::chrono::milliseconds limit) {
        std::unique_lock lock{mutex};
        return cv.wait_for(lock, limit, [this] { return parallel == 0; });
    }

private:
    Ticket Enter(Kind kind) {
        std::unique_lock lock{mutex};
        if (!serial) {
            const bool peers = parallel > 0;
            ++parallel;
            return Ticket{true, peers, entries.fetch_add(1) + 1};
        }
        if (kind == Kind::Game) {
            ++games_waiting;
        }
        cv.wait(lock,
                [&] { return MayStartSerial(kind, parallel, serial_running, games_waiting); });
        if (kind == Kind::Game) {
            --games_waiting;
        }
        serial_running = true;
        return Ticket{false, false, entries.load()};
    }
    void Leave(bool was_parallel) {
        {
            std::scoped_lock lock{mutex};
            if (was_parallel) {
                --parallel;
            } else {
                serial_running = false;
            }
        }
        cv.notify_all();
    }

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::atomic<std::uint64_t> entries{0};
    unsigned parallel{};
    unsigned games_waiting{};
    bool serial{};
    bool serial_running{};
};

/// The driver refused a pipeline even without the shared pipeline cache.
struct BuildFailed : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class PipelineOrigin { Preload, Game };
enum class FailureAction { DropCachedEntry, RetryWithWarmUpPaused, Fatal };

/// After a creation already retried without the driver pipeline cache failed again. A preloaded
/// pipeline is only an optimisation; the game's own gets one more try with the warm-up paused.
constexpr FailureAction OnPipelineFailure(PipelineOrigin origin, bool game_retried) {
    if (origin == PipelineOrigin::Preload) {
        return FailureAction::DropCachedEntry;
    }
    return game_retried ? FailureAction::Fatal : FailureAction::RetryWithWarmUpPaused;
}

/// What a failed warm-up build leads to: never fatal. The cached entry is dropped and left to the
/// game; the build mode turns serial only when the failed build was not alone.
struct WarmupFailure {
    bool drop_entry;
    bool record_serial;
};

constexpr WarmupFailure OnWarmupBuildFailure(bool build_overlapped) {
    return {true, build_overlapped};
}

/// Builds a pipeline the game asked for, holding a game slot of `gate`: one more try after
/// `pause_warmup`, and `fatal` (which does not return in the game) when that fails too.
template <typename Make, typename Pause, typename Fatal>
auto BuildGamePipeline(BuildGate& gate, Make&& make, Pause&& pause_warmup, Fatal&& fatal) {
    for (bool retried = false;; retried = true) {
        try {
            const BuildGate::Slot slot{gate, BuildGate::Kind::Game};
            return make();
        } catch (const BuildFailed& error) {
            if (OnPipelineFailure(PipelineOrigin::Game, retried) == FailureAction::Fatal) {
                fatal(error);
                throw;
            }
            pause_warmup(error);
        }
    }
}

/// SOS_PIPELINE_ON_DEMAND: any non-empty value but "0" skips the background warm-up.
inline bool PipelineOnDemandRequested(const char* env_value) {
    return env_value && *env_value && std::strcmp(env_value, "0") != 0;
}

} // namespace PipelineCachePolicy
