// Headless Vulkan proof, on the production TileImage, DetileImage and MergeGuestPages, that tiled images keep GPU-newer
// data and take CPU-newer guest pages exactly. A compute device only: no window, swapchain, launcher, game or audio.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "video_core/amdgpu/tiling.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/page_ownership.h"
#include "video_core/texture_cache/tile_manager.h"

extern "C" FILE* sos_stderr_stream(void) {
    return stdout;
}

extern "C" std::uint64_t runtime_disabled_optimizations = 0;

using namespace VideoCore;
using Vulkan::StagingBufferRef;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_finite_floats = false;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

struct Gpu {
    Vulkan::Instance instance{-1, false};
    Vulkan::Scheduler scheduler{instance, false};
    Vulkan::Runtime runtime{instance, scheduler};
    StreamBuffer stream{instance, scheduler, MemoryType::Stream, u64{64} << 20};
    TileManager tiles{instance, scheduler, runtime, stream};
    Common::SlotVector<ImageView> views;

    StagingBufferRef Host(u64 size, MemoryType type) { return runtime.GetStagingPool().Request(size, type, 16); }
};

struct Shape {
    const char* name;
    AmdGpu::TileMode mode;
    u32 bits, width, height, layers, levels;
    VAddr base;
    u32 depth = 1;
    bool zplane = false, stencil = false;
};

vk::Format FormatFor(u32 bits) {
    switch (bits) {
    case 8: return vk::Format::eR8Uint;
    case 16: return vk::Format::eR16Uint;
    case 32: return vk::Format::eR8G8B8A8Unorm;
    case 64: return vk::Format::eR32G32Uint;
    default: return vk::Format::eR32G32B32A32Uint;
    }
}

ImageInfo MakeInfo(const Shape& shape) {
    ImageInfo info{};
    info.tile_mode = shape.mode;
    info.array_mode = AmdGpu::GetArrayMode(shape.mode);
    info.props.is_tiled = 1;
    info.num_bits = shape.bits;
    info.size = {shape.width, shape.height, shape.depth};
    info.pitch = shape.width;
    info.resources = {shape.levels, shape.layers};
    info.props.is_volume = shape.depth > 1;
    info.type = shape.depth > 1 ? AmdGpu::ImageType::Color3D : shape.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
    info.pixel_format = FormatFor(shape.bits);
    if (shape.zplane) {
        info.props.is_depth = 1;
        info.props.has_stencil = shape.stencil;
        info.pixel_format = shape.bits == 32 ? (shape.stencil ? vk::Format::eD32SfloatS8Uint : vk::Format::eD32Sfloat) : vk::Format::eD16Unorm;
        g_finite_floats = shape.bits == 32;
    } else {
        g_finite_floats = false;
    }
    info.guest_address = shape.base;
    info.UpdateSize();
    return info;
}

struct Texels {
    const ImageInfo& info;
    u32 Bytes() const { return info.num_bits / 8; }
    u32 Width(u32 mip) const { return std::max(info.size.width >> mip, 1u); }
    u32 Height(u32 mip) const { return std::max(info.size.height >> mip, 1u); }
    u32 Slices(u32 mip) const { return info.props.is_volume ? std::max(info.size.depth >> mip, 1u) : info.resources.layers; }
    u64 Offset(u32 mip, u32 layer, u32 x, u32 y) const {
        const auto& layout = info.mips_layout[mip];
        return layout.offset + ((u64{layer} * layout.height + y) * layout.pitch + x) * Bytes();
    }
};

u8 GpuByte(u32 version, u32 mip, u32 layer, u32 x, u32 y, u32 c) {
    if (g_finite_floats && c == 3) return 0x3F;
    return static_cast<u8>(0x10 + (x * 7 + y * 13 + c * 31 + layer * 5 + mip * 3 + version * 11) % 0x40);
}
u8 CpuByte(u64 offset, u32 version) {
    if (g_finite_floats && offset % 4 == 3) return 0x3E;
    return static_cast<u8>(0xC0 + (offset * 13 + version * 17 + 7) % 61);
}

std::vector<u8> GpuLinear(const ImageInfo& info, u32 version) {
    const Texels t{info};
    std::vector<u8> data(info.guest_size, 0);
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        for (u32 layer = 0; layer < t.Slices(mip); ++layer) {
            for (u32 y = 0; y < t.Height(mip); ++y) {
                for (u32 x = 0; x < t.Width(mip); ++x) {
                    for (u32 c = 0; c < t.Bytes(); ++c) data[t.Offset(mip, layer, x, y) + c] = GpuByte(version, mip, layer, x, y, c);
                }
            }
        }
    }
    return data;
}

struct Fixture {
    Gpu& gpu;
    std::unique_ptr<Image> image;
    ImageInfo info;

    Fixture(Gpu& gpu, const Shape& shape) : gpu{gpu}, info{MakeInfo(shape)} {
        image = std::make_unique<Image>(gpu.instance, gpu.runtime, gpu.views, info);
    }

    void Upload(const std::vector<u8>& linear, std::optional<vk::BufferImageCopy> region = {}) {
        const auto staging = gpu.Host(info.guest_size, MemoryType::HostUncached);
        std::memcpy(staging.mapped, linear.data(), linear.size());
        staging.Flush();
        auto copies = TileManager::MipCopies(*image);
        std::vector<vk::BufferImageCopy> used(copies.begin(), copies.end());
        if (region) used = {*region};
        for (auto& copy : used) copy.bufferOffset += staging.offset;
        gpu.runtime.UploadImage(image.get(), staging.buffer, used);
        gpu.scheduler.Finish();
    }

    std::vector<u8> Download() {
        const auto out = gpu.Host(info.guest_size, MemoryType::HostCached);
        auto copies = TileManager::MipCopies(*image);
        for (auto& copy : copies) copy.bufferOffset += out.offset;
        gpu.runtime.DownloadImage(image.get(), out.buffer, copies);
        gpu.scheduler.Finish();
        out.Invalidate();
        return std::vector<u8>(out.mapped, out.mapped + info.guest_size);
    }
};

std::vector<u8> ReadDeviceBuffer(Gpu& gpu, const Buffer* buffer, u64 offset, u64 size) {
    const auto host = gpu.Host(size, MemoryType::HostCached);
    const vk::BufferCopy copy{offset, host.offset, size};
    gpu.runtime.CopyBuffer(buffer, host.buffer, std::span{&copy, 1});
    gpu.scheduler.Finish();
    host.Invalidate();
    return std::vector<u8>(host.mapped, host.mapped + size);
}

bool SameTexels(const ImageInfo& info, const std::vector<u8>& a, const std::vector<u8>& b, std::string* first = nullptr) {
    const Texels t{info};
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        for (u32 layer = 0; layer < t.Slices(mip); ++layer) {
            for (u32 y = 0; y < t.Height(mip); ++y) {
                for (u32 x = 0; x < t.Width(mip); ++x) {
                    const u64 at = t.Offset(mip, layer, x, y);
                    if (std::memcmp(&a[at], &b[at], t.Bytes()) != 0) {
                        if (first) *first = "mip " + std::to_string(mip) + " layer " + std::to_string(layer) + " (" + std::to_string(x) + "," + std::to_string(y) + ")";
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

void RoundTrip(Gpu& gpu, const Shape& shape) {
    Fixture f{gpu, shape};
    const auto pattern = GpuLinear(f.info, 1);
    f.Upload(pattern);

    const auto tiled = gpu.runtime.GetStagingPool().Request(f.info.guest_size, MemoryType::DeviceLocal);
    auto copies = TileManager::MipCopies(*f.image);
    gpu.tiles.TileImage(*f.image, copies, tiled.buffer, tiled.offset);
    const auto tiled_bytes = ReadDeviceBuffer(gpu, tiled.buffer, tiled.offset, f.info.guest_size);

    const auto [linear, offset] = gpu.tiles.DetileImage(tiled.buffer, tiled.offset, f.info);
    const auto back = ReadDeviceBuffer(gpu, linear, offset, f.info.guest_size);
    std::string where;
    const std::string label = std::string(shape.name) + " " + std::to_string(shape.bits) + "bpp " + std::to_string(shape.width) + "x" + std::to_string(shape.height) +
                              (shape.layers > 1 ? " layers " + std::to_string(shape.layers) : "") + (shape.depth > 1 ? " depth " + std::to_string(shape.depth) : "") + (shape.levels > 1 ? " mips " + std::to_string(shape.levels) : "");
    const bool exact = SameTexels(f.info, pattern, back, &where);
    const bool merges = TileManager::CanMergeGuestPages(f.info);
    Check(merges, "the merge accepts: " + label);
    Check(!merges || exact, "TileImage then DetileImage is exact for every shape the merge accepts: " + label + (where.empty() ? "" : " first mismatch " + where));
    Check(!merges || !SameTexels(f.info, pattern, tiled_bytes), "TileImage changes the layout (linear -> guest tiled): " + label);
}

// Guest byte offset of every texel slot of the linear layout, taken from the real detiler (bpp 32: each word of the tiled
// input holds its own index).
std::vector<u64> GuestOffsets(Gpu& gpu, const ImageInfo& info) {
    const auto guest = gpu.Host(info.guest_size, MemoryType::HostUncached);
    auto* words = reinterpret_cast<u32*>(guest.mapped);
    for (u64 i = 0; i < info.guest_size / 4; ++i) words[i] = static_cast<u32>(i);
    guest.Flush();
    const auto [linear, offset] = gpu.tiles.DetileImage(guest.buffer, guest.offset, info);
    const auto bytes = ReadDeviceBuffer(gpu, linear, offset, info.guest_size);
    std::vector<u64> map(info.guest_size / 4);
    for (size_t i = 0; i < map.size(); ++i) {
        u32 word;
        std::memcpy(&word, &bytes[i * 4], 4);
        map[i] = u64{word} * 4;
    }
    return map;
}

struct Ledger {
    std::map<u64, int> count;
    bool negative = false;
    static void Apply(void* context, VAddr begin, u64 bytes, bool track) {
        auto& ledger = *static_cast<Ledger*>(context);
        for (u64 page = begin >> 12; page < ((begin + bytes + 4095) >> 12); ++page) {
            int& value = ledger.count[page];
            value += track ? 1 : -1;
            if (value < 0) ledger.negative = true;
        }
    }
    bool Watched(u64 page) const {
        const auto it = count.find(page);
        return it != count.end() && it->second > 0;
    }
    int Total() const {
        int total = 0;
        for (const auto& entry : count) total += entry.second;
        return total;
    }
};

constexpr u64 kPage = 4096;

// A tiled image the GPU wrote, a guest memory the CPU writes, and per-texel truth (every mip and layer) of the newest writer.
struct World {
    Gpu& gpu;
    Fixture f;
    Ledger ledger;
    PageOwnership owner;
    std::vector<u64> map;
    std::vector<bool> valid;
    std::vector<u8> guest;
    std::vector<u32> truth;
    VAddr begin, end;
    u32 pitch, width, height, bytes;
    u32 version = 0;
    StagingBufferRef guest_buffer;

    World(Gpu& gpu, const Shape& shape)
        : gpu{gpu}, f{gpu, shape}, owner{shape.base, shape.base + f.info.guest_size, PageWatcher{&ledger, &Ledger::Apply}},
          begin{shape.base}, end{shape.base + f.info.guest_size}, pitch{f.info.mips_layout[0].pitch}, width{shape.width},
          height{shape.height}, bytes{shape.bits / 8} {
        map = GuestOffsets(gpu, f.info);
        owner.Watch(begin, end, true);
        guest.resize(f.info.guest_size);
        for (u64 o = 0; o < guest.size(); ++o) guest[o] = CpuByte(o, 0);
        guest_buffer = gpu.Host(f.info.guest_size, MemoryType::HostUncached);

        const Texels t{f.info};
        valid.assign(f.info.guest_size / 4, false);
        truth.assign(f.info.guest_size / 4, 0);
        const auto initial = GpuLinear(f.info, 1);
        f.Upload(initial);
        for (u32 mip = 0; mip < f.info.resources.levels; ++mip) {
            for (u32 layer = 0; layer < t.Slices(mip); ++layer) {
                for (u32 y = 0; y < t.Height(mip); ++y) {
                    for (u32 x = 0; x < t.Width(mip); ++x) {
                        const u64 index = t.Offset(mip, layer, x, y) / 4;
                        valid[index] = true;
                        std::memcpy(&truth[index], &initial[index * 4], 4);
                    }
                }
            }
        }
    }

    u32 GpuWord(u32 v, u32 x, u32 y) const {
        u32 word = 0;
        for (u32 c = 0; c < 4; ++c) word |= u32{GpuByte(v, 0, 0, x, y, c)} << (8 * c);
        return word;
    }
    u32 GuestWord(u64 offset) const {
        u32 word;
        std::memcpy(&word, &guest[offset], 4);
        return word;
    }

    void Refresh() {
        const auto ranges = owner.TakeDirty();
        if (ranges.empty()) return;
        std::memcpy(guest_buffer.mapped, guest.data(), guest.size());
        guest_buffer.Flush();
        gpu.tiles.MergeGuestPages(*f.image, guest_buffer.buffer, guest_buffer.offset, ranges);
        gpu.scheduler.Finish();
    }

    // Rewrites a rectangle of mip 0 / layer 0.
    void GpuWriteRect(u32 x0, u32 y0, u32 w, u32 h) {
        Refresh();
        const u32 v = ++version;
        f.Upload(GpuLinear(f.info, v), vk::BufferImageCopy{
                                           .bufferOffset = (u64{y0} * pitch + x0) * 4,
                                           .bufferRowLength = pitch,
                                           .bufferImageHeight = f.info.mips_layout[0].height,
                                           .imageSubresource{.aspectMask = f.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
                                                             .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                                           .imageOffset = {static_cast<s32>(x0), static_cast<s32>(y0), 0},
                                           .imageExtent = {w, h, 1},
                                       });
        for (u32 y = y0; y < y0 + h; ++y) {
            for (u32 x = x0; x < x0 + w; ++x) truth[u64{y} * pitch + x] = GpuWord(v, x, y);
        }
        owner.ClearDirty();
    }
    void GpuWriteAll() { GpuWriteRect(0, 0, width, height); }

    // One CPU write of the whole of a page (clipped to the image), faulting if the page is watched.
    void CpuPage(u64 index) {
        const VAddr page = (begin & ~(kPage - 1)) + index * kPage;
        const VAddr from = std::max(page, begin);
        const VAddr to = std::min(page + kPage, end);
        if (from >= to) return;
        if (ledger.Watched(page / kPage)) owner.MarkCpuWrite(from, 1);
        ++version;
        for (VAddr a = from; a < to; ++a) guest[a - begin] = CpuByte(a - begin, version);
        for (u64 i = 0; i < valid.size(); ++i) {
            const u64 offset = map[i];
            if (valid[i] && begin + offset >= from && begin + offset + 4 <= to) truth[i] = GuestWord(offset);
        }
    }

    // A CPU write before the image in its first page: it faults, releases that page, and changes no image byte.
    void CpuNeighbor() {
        const VAddr page = begin & ~(kPage - 1);
        if (page == begin || !ledger.Watched(page / kPage)) return;
        if (!owner.MarkCpuWrite(page, 1)) owner.Watch(begin, std::min<VAddr>(page + kPage, end), false);
    }
    u64 Pages() const { return ((end + kPage - 1) / kPage) - (begin / kPage); }

    bool Verify(std::string* where = nullptr) {
        Refresh();
        const auto data = f.Download();
        for (u64 i = 0; i < valid.size(); ++i) {
            if (!valid[i]) continue;
            u32 word;
            std::memcpy(&word, &data[i * 4], 4);
            if (word != truth[i]) {
                if (where) *where = "word " + std::to_string(i) + " got " + std::to_string(word) + " want " + std::to_string(truth[i]);
                return false;
            }
        }
        return true;
    }
    bool Balanced() const { return !ledger.negative && ledger.Total() == static_cast<int>(owner.HeldWatches()); }
};

constexpr VAddr kBase = 0x30000300;

void Ownership(Gpu& gpu, const Shape& shape) {
    const std::string tag = std::string(shape.name) + ": ";
    const auto run = [&](const char* what, auto&& body) {
        World w(gpu, shape);
        body(w);
        std::string where;
        Check(w.Verify(&where) && w.Balanced(), tag + what + (where.empty() ? "" : " - " + where));
    };

    run("GPU only", [](World&) {});
    run("single dirty page keeps CPU data, the rest GPU", [](World& w) { w.CpuPage(3); });
    run("multiple non-contiguous dirty pages", [](World& w) { w.CpuPage(1); w.CpuPage(4); w.CpuPage(6); });
    run("first partial page dirty", [](World& w) { w.CpuPage(0); });
    run("last page dirty", [](World& w) { w.CpuPage(w.Pages() - 1); });
    run("all pages dirty", [](World& w) { for (u64 p = 0; p < w.Pages(); ++p) w.CpuPage(p); });
    run("CPU then GPU: the GPU wins", [](World& w) { w.CpuPage(2); w.GpuWriteAll(); });
    run("GPU then CPU: the CPU wins only its page", [](World& w) { w.GpuWriteAll(); w.CpuPage(2); });
    run("GPU, CPU, GPU: the latest GPU write wins again", [](World& w) { w.GpuWriteAll(); w.CpuPage(2); w.GpuWriteAll(); });
    run("CPU page A, GPU, CPU page B", [](World& w) { w.CpuPage(2); w.GpuWriteAll(); w.CpuPage(5); });
    run("CPU between two partial GPU writes", [](World& w) {
        w.GpuWriteRect(0, 0, w.width, w.height / 2);
        w.CpuPage(3);
        w.GpuWriteRect(0, w.height / 2, w.width, w.height - w.height / 2);
    });
    run("GPU between two CPU writes", [](World& w) { w.CpuPage(1); w.GpuWriteRect(0, 0, w.width / 2, w.height); w.CpuPage(4); });
    run("repeated refresh", [](World& w) { w.CpuPage(2); w.Verify(); w.Verify(); w.CpuPage(4); });
    run("write outside the image in the shared first page", [](World& w) { w.CpuNeighbor(); });
}

void Reuse(Gpu& gpu, const Shape& first, const Shape& second) {
    {
        World w(gpu, first);
        w.CpuPage(2);
        w.Verify();
        w.owner.Watch(w.begin, w.end, false);
        w.owner.TakeDirty();
        Check(w.ledger.Total() == 0 && !w.ledger.negative, "recreating at the same address: the old image releases every watch");
    }
    World second_world(gpu, second);
    second_world.CpuPage(1);
    std::string where;
    Check(second_world.Verify(&where) && second_world.Balanced(), std::string("same address, different layout: no state survives") + (where.empty() ? "" : " - " + where));
}

void Fuzz(Gpu& gpu, const Shape& shape, int runs) {
    uint32_t state = 777;
    const auto next = [&](uint32_t limit) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % limit;
    };
    int failures = 0;
    std::string first;
    for (int run = 0; run < runs; ++run) {
        World w(gpu, shape);
        std::string log;
        const int steps = 3 + next(7);
        for (int step = 0; step < steps; ++step) {
            switch (next(4)) {
            case 0:
            case 1: {
                const u64 page = next(static_cast<uint32_t>(w.Pages()));
                w.CpuPage(page);
                log += "C" + std::to_string(page) + " ";
                break;
            }
            case 2: {
                const u32 y0 = next(w.height - 1);
                const u32 h = 1 + next(w.height - y0);
                w.GpuWriteRect(0, y0, w.width, h);
                log += "G" + std::to_string(y0) + "+" + std::to_string(h) + " ";
                break;
            }
            default:
                w.Verify();
                log += "V ";
                break;
            }
        }
        std::string where;
        const bool ok = w.Verify(&where) && w.Balanced();
        if (!ok && ++failures == 1) first = log + where;
    }
    Check(failures == 0, std::string("randomized CPU/GPU sequences on tiled ") + shape.name + " (" + std::to_string(runs) + " runs, first failure: " + first + ")");
}


// A depth merge reads and writes the depth aspect only: the stencil plane of the same image keeps its GPU contents.
void StencilIsKept(Gpu& gpu) {
    const Shape shape{"depth+stencil", AmdGpu::TileMode::Depth2DThin64, 32, 128, 128, 1, 1, kBase, 1, true, true};
    World w(gpu, shape);
    const u32 pitch = w.pitch;
    std::vector<u8> stencil(u64{pitch} * w.height);
    for (size_t i = 0; i < stencil.size(); ++i) stencil[i] = static_cast<u8>(0x11 + i % 0xE0);

    const auto staging = gpu.Host(stencil.size(), MemoryType::HostUncached);
    std::memcpy(staging.mapped, stencil.data(), stencil.size());
    staging.Flush();
    const vk::BufferImageCopy upload{.bufferOffset = staging.offset,
                                     .bufferRowLength = pitch,
                                     .bufferImageHeight = w.f.info.mips_layout[0].height,
                                     .imageSubresource{.aspectMask = vk::ImageAspectFlagBits::eStencil, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                                     .imageOffset = {0, 0, 0},
                                     .imageExtent = {w.width, w.height, 1}};
    gpu.runtime.UploadImage(w.f.image.get(), staging.buffer, std::span{&upload, 1});
    gpu.scheduler.Finish();

    w.CpuPage(2);
    w.CpuPage(5);
    std::string where;
    Check(w.Verify(&where), "depth merge keeps the depth data exact on a depth+stencil image" + (where.empty() ? "" : " - " + where));

    const auto out = gpu.Host(stencil.size(), MemoryType::HostCached);
    const vk::BufferImageCopy download{.bufferOffset = out.offset,
                                       .bufferRowLength = pitch,
                                       .bufferImageHeight = w.f.info.mips_layout[0].height,
                                       .imageSubresource{.aspectMask = vk::ImageAspectFlagBits::eStencil, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                                       .imageOffset = {0, 0, 0},
                                       .imageExtent = {w.width, w.height, 1}};
    gpu.runtime.DownloadImage(w.f.image.get(), out.buffer, std::span{&download, 1});
    gpu.scheduler.Finish();
    out.Invalidate();
    bool same = true;
    for (u32 y = 0; y < w.height && same; ++y) same = std::memcmp(out.mapped + u64{y} * pitch, stencil.data() + u64{y} * pitch, w.width) == 0;
    Check(same, "the stencil plane is untouched by a depth merge");
}

} // namespace

int main() {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    Gpu gpu;
    std::printf("  device: %s\n", std::string(gpu.instance.GetModelName()).c_str());
    using AmdGpu::TileMode;

    for (const Shape& shape : {Shape{"micro", TileMode::Display1DThin, 32, 40, 24, 1, 1, kBase}, Shape{"micro", TileMode::Thin1DThin, 32, 100, 75, 1, 1, kBase},
                               Shape{"micro", TileMode::Thin1DThin, 32, 256, 256, 1, 1, kBase}, Shape{"macro", TileMode::Thin2DThin, 32, 40, 24, 1, 1, kBase},
                               Shape{"macro", TileMode::Thin2DThin, 32, 300, 130, 1, 1, kBase}, Shape{"macro", TileMode::Thin2DThin, 32, 512, 384, 1, 1, kBase},
                               Shape{"macro", TileMode::Display2DThin, 32, 256, 256, 1, 1, kBase}, Shape{"micro", TileMode::Thin1DThin, 8, 70, 33, 1, 1, kBase},
                               Shape{"micro", TileMode::Thin1DThin, 16, 70, 33, 1, 1, kBase}, Shape{"macro", TileMode::Thin2DThin, 16, 130, 66, 1, 1, kBase},
                               Shape{"micro", TileMode::Thin1DThin, 64, 70, 33, 1, 1, kBase}, Shape{"macro", TileMode::Thin2DThin, 64, 130, 66, 1, 1, kBase},
                               Shape{"micro", TileMode::Thin1DThin, 128, 70, 33, 1, 1, kBase}, Shape{"macro", TileMode::Thin2DThin, 128, 130, 66, 1, 1, kBase},
                               Shape{"micro", TileMode::Thin1DThin, 32, 64, 64, 3, 1, kBase}, Shape{"micro", TileMode::Thin1DThin, 32, 128, 64, 1, 4, kBase},
                               Shape{"macro", TileMode::Thin2DThin, 32, 128, 128, 2, 3, kBase}, Shape{"macro", TileMode::Thin2DThin, 32, 128, 128, 1, 8, kBase}, Shape{"macro", TileMode::Thin2DThin, 32, 128, 128, 2, 1, kBase},
                               Shape{"macro", TileMode::Thin2DThin, 32, 256, 256, 1, 9, kBase}, Shape{"macro", TileMode::Thin2DThin, 32, 512, 512, 1, 10, kBase}, Shape{"macro", TileMode::Display2DThin, 32, 256, 256, 1, 9, kBase},
                               Shape{"macro", TileMode::Thin2DThin, 8, 256, 256, 1, 9, kBase}, Shape{"macro", TileMode::Thin2DThin, 16, 256, 256, 1, 9, kBase}, Shape{"macro", TileMode::Thin2DThin, 64, 256, 256, 1, 9, kBase},
                               Shape{"macro", TileMode::Thin2DThin, 128, 128, 128, 1, 8, kBase}, Shape{"macro", TileMode::Thin2DThin, 32, 300, 130, 1, 6, kBase}, Shape{"micro", TileMode::Thin1DThin, 32, 256, 256, 1, 9, kBase},
                               Shape{"vol Thin1D", TileMode::Thin1DThin, 32, 32, 32, 1, 1, kBase, 8}, Shape{"vol Thin2D", TileMode::Thin2DThin, 32, 64, 64, 1, 1, kBase, 8}, Shape{"vol Thin3D", TileMode::Thin3DThin, 32, 64, 64, 1, 1, kBase, 8},
                               Shape{"vol Thick1D", TileMode::Thick1DThick, 32, 32, 32, 1, 1, kBase, 8}, Shape{"vol Thick2D", TileMode::Thick2DThick, 32, 64, 64, 1, 1, kBase, 8}, Shape{"vol Thick3D", TileMode::Thick3DThick, 32, 64, 64, 1, 1, kBase, 8},
                               Shape{"vol Thin1D", TileMode::Thin1DThin, 32, 32, 32, 1, 4, kBase, 8}, Shape{"vol Thin3D", TileMode::Thin3DThin, 32, 128, 128, 1, 4, kBase, 16}, Shape{"vol Thick3D", TileMode::Thick3DThick, 32, 128, 128, 1, 4, kBase, 16},
                               Shape{"vol Thin2D", TileMode::Thin2DThin, 32, 128, 128, 1, 4, kBase, 16}, Shape{"vol Thin3D", TileMode::Thin3DThin, 8, 64, 64, 1, 1, kBase, 8}, Shape{"vol Thin3D", TileMode::Thin3DThin, 16, 64, 64, 1, 1, kBase, 8},
                               Shape{"vol Thin3D", TileMode::Thin3DThin, 64, 64, 64, 1, 1, kBase, 8}, Shape{"vol Thin3D", TileMode::Thin3DThin, 128, 64, 64, 1, 1, kBase, 8},
                               Shape{"depth macro", TileMode::Depth2DThin64, 32, 128, 128, 1, 1, kBase, 1, true}, Shape{"depth macro", TileMode::Depth2DThin128, 32, 300, 130, 1, 1, kBase, 1, true},
                               Shape{"depth micro", TileMode::Depth1DThin, 32, 100, 75, 1, 1, kBase, 1, true}, Shape{"depth macro mips", TileMode::Depth2DThin64, 32, 256, 256, 1, 9, kBase, 1, true},
                               Shape{"depth+stencil macro", TileMode::Depth2DThin64, 32, 128, 128, 1, 1, kBase, 1, true, true}, Shape{"depth16 macro", TileMode::Depth2DThin64, 16, 128, 128, 1, 1, kBase, 1, true},
                               Shape{"depth16 micro", TileMode::Depth1DThin, 16, 100, 75, 1, 1, kBase, 1, true}, Shape{"depth layers", TileMode::Depth2DThin64, 32, 128, 128, 3, 1, kBase, 1, true}}) {
        RoundTrip(gpu, shape);
    }
    const auto round_trip_done = Clock::now();

    for (const Shape& shape : {Shape{"micro 1D", TileMode::Thin1DThin, 32, 100, 75, 1, 1, kBase}, Shape{"macro 2D", TileMode::Thin2DThin, 32, 300, 130, 1, 1, kBase},
                               Shape{"display macro", TileMode::Display2DThin, 32, 256, 256, 1, 1, kBase}, Shape{"macro 2D mip chain", TileMode::Thin2DThin, 32, 256, 256, 1, 9, kBase},
                               Shape{"micro 1D mip chain", TileMode::Thin1DThin, 32, 128, 128, 1, 8, kBase}, Shape{"macro 2D layers+mips", TileMode::Thin2DThin, 32, 128, 128, 2, 4, kBase},
                               Shape{"volume 3D thin", TileMode::Thin3DThin, 32, 64, 64, 1, 1, kBase, 8}, Shape{"volume 3D thick mips", TileMode::Thick3DThick, 32, 64, 64, 1, 3, kBase, 8},
                               Shape{"volume 1D thin", TileMode::Thin1DThin, 32, 32, 32, 1, 1, kBase, 8}, Shape{"depth macro", TileMode::Depth2DThin64, 32, 128, 128, 1, 1, kBase, 1, true},
                               Shape{"depth micro", TileMode::Depth1DThin, 32, 100, 75, 1, 1, kBase, 1, true}, Shape{"depth macro mips", TileMode::Depth2DThin64, 32, 256, 256, 1, 9, kBase, 1, true}}) {
        Ownership(gpu, shape);
    }
    Reuse(gpu, Shape{"micro", TileMode::Thin1DThin, 32, 100, 75, 1, 1, kBase}, Shape{"macro", TileMode::Thin2DThin, 32, 300, 130, 1, 1, kBase});
    Fuzz(gpu, Shape{"micro", TileMode::Thin1DThin, 32, 100, 75, 1, 1, kBase}, 40);
    Fuzz(gpu, Shape{"macro", TileMode::Thin2DThin, 32, 300, 130, 1, 1, kBase}, 40);
    Fuzz(gpu, Shape{"macro mip chain", TileMode::Thin2DThin, 32, 256, 256, 1, 9, kBase}, 30);
    Fuzz(gpu, Shape{"volume", TileMode::Thin3DThin, 32, 64, 64, 1, 1, kBase, 8}, 30);
    Fuzz(gpu, Shape{"depth", TileMode::Depth2DThin64, 32, 128, 128, 1, 1, kBase, 1, true}, 30);
    StencilIsKept(gpu);

    const auto end = Clock::now();
    const auto ms = [](auto d) { return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()); };
    std::printf("  tiled ownership test: round trips %lld ms, ownership %lld ms, total %lld ms\n", ms(round_trip_done - start), ms(end - round_trip_done), ms(end - start));
    if (g_failures) {
        std::printf("%d of %d tiled ownership checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("all %d tiled ownership checks passed\n", g_checks);
    return 0;
}
