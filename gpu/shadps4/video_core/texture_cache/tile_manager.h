// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <unordered_map>
#include <utility>
#include <boost/container/small_vector.hpp>
#include <boost/container_hash/hash.hpp>

#include "common/types.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/page_ownership.h"

namespace Vulkan {
struct Runtime;
}

namespace VideoCore {

struct ImageInfo;
struct Image;
class BufferCache;

class TileManager {
    struct TilingKey {
        AmdGpu::TileMode tile_mode;
        u32 num_bits;
        u32 num_samples;
        bool alt_tile;
        bool is_tiler;

        bool operator==(const TilingKey&) const = default;

        struct Hash {
            size_t operator()(const TilingKey& key) const {
                size_t hash = 0;
                boost::hash_combine(hash, key.tile_mode);
                boost::hash_combine(hash, key.num_bits);
                boost::hash_combine(hash, key.num_samples);
                boost::hash_combine(hash, key.alt_tile);
                boost::hash_combine(hash, key.is_tiler);
                return hash;
            }
        };
    };

public:
    explicit TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, StreamBuffer& stream_buffer);
    ~TileManager();

    void TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                   const VideoCore::Buffer* out_buffer, u64 out_offset);

    std::pair<const Buffer*, u64> DetileImage(const VideoCore::Buffer* in_buffer, u64 in_offset,
                                              const ImageInfo& info);

    /// dcp: DetileImage of a buffer cache binding: the params copy and the staging request can
    /// wait for the GPU, and faults processed then may migrate a fully resident source arena, so
    /// the binding is refreshed before the dispatch reads it.
    std::pair<const Buffer*, u64> DetileImage(const BufferCache& buffer_cache,
                                              const VideoCore::Buffer* in_buffer, u64 in_offset,
                                              const ImageInfo& info);

    [[nodiscard]] static bool CanMergeGuestPages(const ImageInfo& info);

    [[nodiscard]] static boost::container::small_vector<vk::BufferImageCopy, 14> MipCopies(
        const Image& image);

    // Replaces the given guest byte ranges of a tiled image with the guest memory at `guest`,
    // keeping the GPU contents of every other byte.
    void MergeGuestPages(Image& image, const Buffer* guest, u64 guest_offset,
                         std::span<const ByteRange> ranges);

    /// dcp: MergeGuestPages of a buffer cache binding, refreshed like DetileImage's.
    void MergeGuestPages(const BufferCache& buffer_cache, Image& image, const Buffer* guest,
                         u64 guest_offset, std::span<const ByteRange> ranges);

private:
    std::pair<const Buffer*, u64> DetileImageFrom(const BufferCache* buffer_cache,
                                                  const VideoCore::Buffer* in_buffer,
                                                  u64 in_offset, const ImageInfo& info);

    void MergeGuestPagesFrom(const BufferCache* buffer_cache, Image& image, const Buffer* guest,
                             u64 guest_offset, std::span<const ByteRange> ranges);

    void RecordTilingDispatch(vk::Pipeline pipeline, const vk::DescriptorBufferInfo& tiled,
                              const vk::DescriptorBufferInfo& linear,
                              const vk::DescriptorBufferInfo& params, u32 dim_x);

    vk::Pipeline GetTilingPipeline(const ImageInfo& info, bool is_tiler);

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    StreamBuffer& stream_buffer;
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pl_layout;
    std::unordered_map<TilingKey, vk::UniquePipeline, TilingKey::Hash> tiling_pipelines;
};

} // namespace VideoCore
