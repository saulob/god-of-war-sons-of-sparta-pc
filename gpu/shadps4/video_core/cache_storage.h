// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"
#include "video_core/cache_request_queue.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

/// dcp: the driver pipeline binaries of the running game. One folder for every build of the title:
/// a binary is keyed by the driver's own pipeline key, so builds cannot mix them up.
std::filesystem::path PipelineBinaryDirectory();

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    /// dcp: the store of the running game build (CacheFolderName); the first build started after
    /// the single-folder layout takes that folder's store over (pipeline_cache_policy.h).
    void Open();
    /// dcp: finishes the queued writes for a bounded time, then drops the rest (they are
    /// optional) and prints one line of what the IO queue did.
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// dcp: removes every cached blob (an incompatible cache is rebuilt, not ignored).
    void Clear();
    /// dcp: true when the store folder holds no finished file.
    [[nodiscard]] bool IsEmpty() const;

    /// dcp: blobs are sealed (size and checksum) when saved; Load and ForEachBlob hand out only
    /// blobs that pass, and discard the others. False when the write was not queued (store
    /// closed, too large, or the IO queue full).
    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);
    /// dcp: queued behind the pending saves, so it removes what they wrote too.
    void Remove(BlobType type, const std::string& name);

    /// dcp: `stop` returning true ends the walk early (background warm-up at exit).
    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func,
                     const std::function<bool()>& stop = {});
    /// dcp: blobs are sealed files written beside and renamed into place, so other threads
    /// may read them while the IO thread saves (the zip archive cannot).
    [[nodiscard]] bool ConcurrentReads() const;
    /// dcp: number of blobs ForEachBlob visits (the loading screen's total).
    size_t CountBlobs(BlobType type);

    /// dcp: a stored entry that cannot be used. One warning, the rest counted.
    void ReportCorrupt(std::string_view what);

private:
    template <typename T>
    void LoadBlob(BlobType type, const std::string& name, std::vector<T>& data);
    template <typename T>
    bool SaveBlob(BlobType type, const std::string& name, std::vector<T>&& data);
    void DiscardIfCorrupt(const std::filesystem::path& path);
    void AdoptLegacyStore(const std::filesystem::path& root, std::string_view serial,
                          const std::string& folder);

    std::jthread io_worker{};
    std::shared_ptr<RequestQueue> queue{};
    std::filesystem::path cache_path{};
    std::atomic<u32> corrupt_entries{0};
    bool opened{};
};

} // namespace Storage
