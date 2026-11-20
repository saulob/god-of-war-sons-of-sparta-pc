// Headless proof of the guest-memory coherency rules of the texture cache: an image cached at an address is not reused
// for a different layout at the same address, depth never aliases a color image, and the write-back, boundary-page,
// depth-retirement and fence-ordering mechanisms are wired as the audit of the PS5 reference commit found them.
// Layout rules run on the real ImageInfo and aliasing code; wiring is checked in the sources. No window, GPU or game.
#include <algorithm>
#include <array>
#include <map>
#include <vector>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/aliasing.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/page_ownership.h"

extern "C" FILE* sos_stderr_stream(void) {
    return stdout;
}

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

namespace fs = std::filesystem;
using VideoCore::ByteRange;
using VideoCore::ImageInfo;
using VideoCore::LinearRects;
using VideoCore::PageOwnership;
using VideoCore::PageWatcher;
using VideoCore::TexelRect;

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
    text.erase(std::remove(text.begin(), text.end(), char(13)), text.end());
    return text;
}

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
    const auto begin = text.find(from);
    if (begin == std::string::npos) return {};
    const auto end = text.find(to, begin + from.size());
    return text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

constexpr VAddr kAddress = 0x4000000;

ImageInfo Color(AmdGpu::TileMode mode, vk::Format format, u32 width, u32 height, u32 pitch) {
    ImageInfo info{};
    info.guest_address = kAddress;
    info.tile_mode = mode;
    info.pixel_format = format;
    info.size = {width, height, 1};
    info.pitch = pitch;
    info.num_bits = 32;
    info.type = AmdGpu::ImageType::Color2D;
    info.props.is_tiled = mode != AmdGpu::TileMode::DisplayLinearAligned;
    info.guest_size = pitch * height * 4;
    return info;
}

void AddressReuseLayouts() {
    using AmdGpu::TileMode;
    const auto linear = Color(TileMode::DisplayLinearAligned, vk::Format::eR8G8B8A8Unorm, 1280, 720, 1280);
    const auto tiled = Color(TileMode::Display1DThin, vk::Format::eR8G8B8A8Unorm, 1280, 720, 1280);
    const auto otherTiled = Color(TileMode::Thin1DThin, vk::Format::eR8G8B8A8Unorm, 1280, 720, 1280);

    Check(VideoCore::IsLayoutReuse(tiled, linear), "tiled image cached, linear requested at the same address is a reuse");
    Check(VideoCore::IsLayoutReuse(linear, tiled), "linear image cached, tiled requested at the same address is a reuse");
    Check(!VideoCore::IsLayoutReuse(linear, linear), "same linear layout is not a reuse");
    Check(!VideoCore::IsLayoutReuse(tiled, otherTiled), "two tiled modes keep their existing view resolution");

    Check(VideoCore::GetAliasCopyExtent(linear, linear).has_value(), "compatible linear resource is copyable");
    Check(!VideoCore::GetAliasCopyExtent(linear, tiled), "linear and tiled resources at one address never alias");
    Check(!VideoCore::GetAliasCopyExtent(tiled, otherTiled), "different tile modes never alias");

    auto otherFormat = linear;
    otherFormat.pixel_format = vk::Format::eR32Sfloat;
    otherFormat.size = {640, 720, 1};
    Check(!VideoCore::GetAliasCopyExtent(linear, otherFormat), "different format and extent do not copy");

    auto otherPitch = linear;
    otherPitch.pitch = 1536;
    Check(!VideoCore::GetAliasCopyExtent(linear, otherPitch), "different pitch does not alias");

    auto otherBits = linear;
    otherBits.num_bits = 64;
    Check(!VideoCore::GetAliasCopyExtent(linear, otherBits), "different element size does not alias");

    auto depth = Color(TileMode::Depth2DThin64, vk::Format::eD32Sfloat, 1280, 720, 1280);
    depth.props.is_depth = 1;
    auto asColor = Color(TileMode::Depth2DThin64, vk::Format::eR32Sfloat, 1280, 720, 1280);
    Check(!VideoCore::GetAliasCopyExtent(depth, asColor) && !VideoCore::GetAliasCopyExtent(asColor, depth),
          "a depth image never aliases a color image");
}

void CacheWiring() {
    const auto cache = Source("gpu/shadps4/video_core/texture_cache/texture_cache.cpp");

    const auto find = Between(cache, "ImageId TextureCache::FindImage(", "ImageId TextureCache::FindImageFromRange");
    Check(Has(find, "IsLayoutReuse(cache_image.info, info)"), "FindImage perfect match rejects a layout reuse");
    Check(Has(find, "cached.tiled == info.IsTiled()") && Has(find, ".tiled = info.IsTiled()"),
          "FindImage result cache is keyed by the layout class");
    Check(Has(find, "image.info.IsTiled() == info.IsTiled()"), "FindImage still-exact shortcut checks the layout class");

    const auto overlap = Between(cache, "TextureCache::ResolveOverlap(", "ImageId TextureCache::ExpandImage");
    const auto depthCall = overlap.find("ResolveDepthOverlap(image_info, binding, cache_image_id)");
    const auto reuseCall = overlap.find("IsLayoutReuse(cache_image.info, image_info)");
    Check(depthCall != std::string::npos && reuseCall != std::string::npos && depthCall < reuseCall,
          "ResolveOverlap rejects a layout reuse after the depth resolution");

    const auto depth = Between(cache, "ImageId TextureCache::ResolveDepthOverlap(", "std::tuple<ImageId, int, int> TextureCache::ResolveOverlap");
    Check(Has(depth, "case BindingType::RenderTarget:") && Has(depth, "recreate |= cache_image.info.props.is_depth;"),
          "a color target over a cached depth image recreates it");
    Check(Has(depth, "FreeImage(cache_image_id);") && Has(depth, "htile_clear_mask = 0;"),
          "the replaced depth image is freed and the new one carries no clear mask");
    Check(Has(depth, "case BindingType::DepthTarget:") && Has(depth, "recreate |= !cache_image.info.props.is_depth;"),
          "a depth target over a cached color image recreates it");

    const auto stencil = Between(cache, "void TextureCache::AssociateStencil(", "void TextureCache::RefreshImage");
    Check(Has(stencil, "AssociateDepth(depth_id, slot_images[depth_id].image_uid)"), "stencil memory is tied to its depth image by uid");

    const auto edges = Between(cache, "void TextureCache::MarkDirtyIfEdgesModified", "void TextureCache::TrackImageHead");
    Check(Has(edges, "head_hash") && Has(edges, "tail_hash") && Has(edges, "NoteEdgeWrite"),
          "writes to a shared boundary page dirty the image only when its own bytes changed");
    const auto invalidate = Between(cache, "void TextureCache::InvalidateMemory(", "void TextureCache::InvalidateMemoryFromGPU");
    Check(Has(invalidate, "UntrackImageHead(image_id)") && Has(invalidate, "UntrackImageTail(image_id)") && Has(invalidate, "MarkAsMaybeDirty"),
          "first, last and single shared pages are handled separately from pages the image owns");

    const auto storage = Between(cache, "ImageView& TextureCache::FindTexture(", "ImageView& TextureCache::FindRenderTarget");
    Check(Has(storage, "BindingType::Storage") && Has(storage, "readback_linear_images") && Has(storage, "download_images.emplace(image_id)"),
          "a linear storage image is queued for write-back when it is bound");
    Check(!Has(storage, "Finish()") && !Has(storage, "Wait("), "binding a storage image does not wait for the GPU");
    const auto target = Between(cache, "ImageView& TextureCache::FindRenderTarget(", "ImageView& TextureCache::FindDepthTarget");
    Check(Has(target, "download_images.emplace(image_id)"), "a linear render target is queued for write-back");

    const auto process = Between(cache, "void TextureCache::ProcessDownloadImages()", "std::optional<TextureCache::PendingDownload> TextureCache::RecordImageDownload");
    Check(Has(process, "scheduler.Finish();") && !Has(process, "WaitIdle"), "queued images are written back with one wait per batch");
    const auto record = Between(cache, "std::optional<TextureCache::PendingDownload> TextureCache::RecordImageDownload", "void TextureCache::DownloadImageMemory");
    Check(Has(record, "ASSERT(download_size <= image.info.guest_size)") && Has(record, "guest_address, download_size"),
          "write-back is limited to the image's own bytes");
}

void FenceOrdering() {
    const auto pool = Source("gpu/shim/core/emulator_settings.h");
    Check(Has(pool, "IsReadbackLinearImagesEnabled() { return true; }"), "linear image write-back is on");

    const auto live = Source("gpu/shadps4/video_core/amdgpu/liverpool.cpp");
    const auto eop = Between(live, "void RunEventWriteEop(", "void RunEventWriteEos");
    Check(Has(eop, "ProcessDownloadImages()") && eop.find("ProcessDownloadImages()") < eop.find("SignalEop"),
          "write-back precedes the end-of-pipe fence the guest sees");
    const auto eos = Between(live, "void RunEventWriteEos(", "PM4ItOpcode");
    Check(Has(eos, "ProcessDownloadImages()") && eos.find("ProcessDownloadImages()") < eos.find("SignalFence"),
          "write-back precedes the end-of-shader fence");
    const auto release = Between(live, "case PM4ItOpcode::ReleaseMem:", "case PM4ItOpcode::EventWrite:");
    Check(Has(release, "ProcessDownloadImages()") && release.find("ProcessDownloadImages()") < release.find("SignalFence"),
          "write-back precedes the compute release-mem fence");

    const auto rasterizer = Source("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp");
    const auto onSubmit = Between(rasterizer, "void Rasterizer::OnSubmit()", "const PreparedStage* Rasterizer::FindPreparedStage");
    Check(Has(onSubmit, "texture_cache.ProcessDownloadImages()"), "write-back also runs at submit");
    const auto dispatch = Between(rasterizer, "void Rasterizer::DispatchDirect()", "void Rasterizer::DispatchIndirect(");
    Check(!dispatch.empty() && !Has(dispatch, "ProcessDownloadImages") && !Has(dispatch, "scheduler.Finish"),
          "a compute dispatch does not wait for write-back");

    const auto read = Between(rasterizer, "bool Rasterizer::ReadMemory(", "void Rasterizer::ProcessDownloadImages()");
    Check(Has(read, "ResolveReadbacks"), "guest CPU reads of a pending copy are synchronized");
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
    int Total() const {
        int total = 0;
        for (const auto& entry : count) total += entry.second;
        return total;
    }
    bool Watched(u64 page) const {
        const auto it = count.find(page);
        return it != count.end() && it->second > 0;
    }
};

constexpr VAddr kPage = 4096;
constexpr u32 kTexel = 4;

// A linear GPU-written image with a CPU view (guest), a GPU view (gpu) and the newest writer of every byte (truth).
struct World {
    VAddr begin, end, region_begin, region_end;
    u32 width, height, pitch;
    std::vector<u8> guest, gpu, truth;
    Ledger& ledger;
    PageOwnership owner;
    u32 gpu_serial = 0;
    bool head_untracked = false, tail_untracked = false;
    std::vector<u8> head_snapshot, tail_snapshot;
    u32 head_serial = 0, tail_serial = 0;
    u8 value = 0;

    World(Ledger& ledger, VAddr base, u32 width, u32 height, u32 pitch, bool adopt = false)
        : begin{base}, end{base + u64{pitch} * height * kTexel}, region_begin{base & ~(kPage - 1)},
          region_end{(end + kPage - 1) & ~(kPage - 1)}, width{width}, height{height}, pitch{pitch},
          ledger{ledger}, owner{begin, end, PageWatcher{&ledger, &Ledger::Apply}} {
        guest.resize(region_end - region_begin);
        for (size_t i = 0; i < guest.size(); ++i) guest[i] = static_cast<u8>(0xA0 + i % 7);
        gpu = guest;
        truth = guest;
        if (adopt) {
            Ledger::Apply(&ledger, begin, end - begin, true);
            owner.AdoptWatched(begin, end);
        } else {
            owner.Watch(begin, end, true);
        }
    }

    u8 Fresh() { return value = static_cast<u8>(value % 200 + 1); }
    size_t At(VAddr address) const { return address - region_begin; }
    bool InImage(VAddr address) const { return address >= begin && address < end; }
    bool IsTexel(VAddr address) const { return InImage(address) && (address - begin) % (u64{pitch} * kTexel) < u64{width} * kTexel; }
    VAddr PageStart(u64 index) const { return region_begin + index * kPage; }
    u64 Pages() const { return (region_end - region_begin) / kPage; }
    VAddr HeadEnd() const { return std::min<VAddr>(region_begin + kPage, end); }
    VAddr TailBegin() const { return std::max<VAddr>(region_end - kPage, begin); }

    void Fault(VAddr address) {
        if (owner.MarkCpuWrite(address, 1)) return;
        if (address < begin) {
            owner.Watch(begin, HeadEnd(), false);
            head_snapshot.assign(guest.begin() + At(begin), guest.begin() + At(HeadEnd()));
            head_serial = gpu_serial;
            head_untracked = true;
        } else {
            owner.Watch(TailBegin(), end, false);
            tail_snapshot.assign(guest.begin() + At(TailBegin()), guest.begin() + At(end));
            tail_serial = gpu_serial;
            tail_untracked = true;
        }
    }

    void CpuStore(VAddr from, VAddr to, u8 byte) {
        for (VAddr page = from & ~(kPage - 1); page < to; page += kPage) {
            const VAddr first = std::max(from, page);
            if (ledger.Watched(page / kPage)) Fault(first);
            for (VAddr a = first; a < std::min(to, page + kPage); ++a) {
                guest[At(a)] = byte;
                truth[At(a)] = byte;
            }
        }
    }

    void CheckEdges(bool retrack) {
        if (head_untracked) {
            const bool changed = !std::equal(head_snapshot.begin(), head_snapshot.end(), guest.begin() + At(begin));
            if (changed && gpu_serial == head_serial) owner.MarkCpuWrite(begin, 1);
            if (retrack) {
                owner.Watch(begin, HeadEnd(), true);
                head_untracked = false;
            }
        }
        if (tail_untracked) {
            const bool changed = !std::equal(tail_snapshot.begin(), tail_snapshot.end(), guest.begin() + At(TailBegin()));
            if (changed && gpu_serial == tail_serial) owner.MarkCpuWrite(end - 1, 1);
            if (retrack) {
                owner.Watch(TailBegin(), end, true);
                tail_untracked = false;
            }
        }
    }

    void Refresh() {
        for (const ByteRange& range : owner.TakeDirty()) {
            for (const TexelRect& rect : LinearRects(range, begin, pitch, width, height, kTexel)) {
                for (u32 row = 0; row < rect.height; ++row) {
                    for (u32 b = 0; b < rect.width * kTexel; ++b) {
                        const VAddr a = begin + rect.buffer_offset + u64{row} * pitch * kTexel + b;
                        gpu[At(a)] = guest[At(a)];
                    }
                }
            }
        }
    }

    void GpuWrite(VAddr from, VAddr to, u8 byte) {
        CheckEdges(true);
        Refresh();
        for (VAddr a = std::max(from, begin); a < std::min(to, end); ++a) {
            if (IsTexel(a)) gpu[At(a)] = truth[At(a)] = byte;
        }
        ++gpu_serial;
        owner.ClearDirty();
    }

    void GpuWriteAll(u8 byte) { GpuWrite(begin, end, byte); }

    void FlagOnlyGpuWrite(u8 byte) {
        for (VAddr a = begin; a < end; ++a) {
            if (IsTexel(a)) gpu[At(a)] = truth[At(a)] = byte;
        }
        ++gpu_serial;
        owner.ClearDirty();
    }

    void Download() {
        CheckEdges(false);
        owner.ForEachCleanRun(begin, end, [&](const ByteRange& run) {
            for (VAddr a = run.begin; a < run.end; ++a) {
                if (IsTexel(a)) guest[At(a)] = gpu[At(a)];
            }
        });
    }

    void Settle() {
        CheckEdges(true);
        Refresh();
        Download();
    }

    void Untrack() { owner.Watch(begin, end, false); }

    bool Balanced() const { return !ledger.negative && ledger.Total() == static_cast<int>(owner.HeldWatches()); }

    mutable std::string mismatch;

    bool Consistent() const {
        for (VAddr a = region_begin; a < region_end; ++a) {
            const bool outside = !InImage(a);
            if ((outside && guest[At(a)] != truth[At(a)]) ||
                (!outside && IsTexel(a) && (guest[At(a)] != truth[At(a)] || gpu[At(a)] != truth[At(a)]))) {
                mismatch = std::string(outside ? "outside" : "image") + " page " + std::to_string((a - region_begin) / kPage) + " guest=" +
                           std::to_string(guest[At(a)]) + " gpu=" + std::to_string(gpu[At(a)]) + " truth=" + std::to_string(truth[At(a)]);
                return false;
            }
        }
        return true;
    }

    bool PageIs(u64 index, u8 byte, bool gpu_side) const {
        for (VAddr a = std::max(PageStart(index), begin); a < std::min(PageStart(index + 1), end); ++a) {
            if (IsTexel(a) && (gpu_side ? gpu : guest)[At(a)] != byte) return false;
        }
        return true;
    }

    void CpuPage(u64 index, bool inside_only, u8 byte) {
        VAddr from = PageStart(index), to = PageStart(index + 1);
        if (inside_only) {
            from = std::max(from, begin);
            to = std::min(to, end);
        }
        if (from < to) CpuStore(from, to, byte);
    }
    void GpuPages(u64 first, u64 last, u8 byte) { GpuWrite(PageStart(first), PageStart(last + 1), byte); }
};

constexpr VAddr kBase = 0x40000300;

void LinearOwnershipCases() {
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        const u8 gpu1 = w.Fresh();
        w.GpuWriteAll(gpu1);
        w.Download();
        Check(w.PageIs(2, gpu1, false), "GPU only: the guest receives the GPU data");
        const u8 cpu = w.Fresh();
        w.CpuPage(2, true, cpu);
        w.Download();
        Check(w.PageIs(2, cpu, false) && w.PageIs(1, gpu1, false) && w.PageIs(3, gpu1, false),
              "GPU then CPU page: the dirty page keeps CPU data, other pages keep GPU data");
        w.Download();
        w.Download();
        Check(w.PageIs(2, cpu, false), "repeated downloads keep the CPU page");
        w.Settle();
        Check(w.PageIs(2, cpu, true) && w.PageIs(1, gpu1, true) && w.Consistent() && w.Balanced(),
              "the next bind uploads only the CPU page");
    }
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        const u8 cpu = w.Fresh();
        w.CpuPage(2, true, cpu);
        const u8 gpu1 = w.Fresh();
        w.GpuWriteAll(gpu1);
        w.Download();
        w.Settle();
        Check(w.PageIs(2, gpu1, false) && w.PageIs(2, gpu1, true) && w.Consistent(), "CPU then GPU: the GPU wins");
    }
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        w.GpuWriteAll(w.Fresh());
        w.Download();
        const u8 cpu = w.Fresh();
        w.CpuPage(2, true, cpu);
        const u8 gpu2 = w.Fresh();
        w.GpuWriteAll(gpu2);
        w.Download();
        w.Settle();
        Check(w.PageIs(2, gpu2, false) && w.Consistent() && w.Balanced(), "GPU then CPU then GPU: the latest GPU write wins again");
    }
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        const u8 gpu1 = w.Fresh();
        w.GpuWriteAll(gpu1);
        w.Download();
        const u8 a = w.Fresh();
        w.CpuPage(1, true, a);
        const u8 b = w.Fresh();
        w.CpuPage(3, true, b);
        const u8 c = w.Fresh();
        w.CpuPage(5, true, c);
        w.Download();
        Check(w.PageIs(1, a, false) && w.PageIs(3, b, false) && w.PageIs(5, c, false) && w.PageIs(2, gpu1, false) &&
                  w.PageIs(4, gpu1, false),
              "several CPU pages: only those pages are CPU data");
        w.Settle();
        Check(w.Consistent() && w.Balanced(), "several CPU pages reach the GPU image");
    }
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        w.GpuWriteAll(w.Fresh());
        w.Download();
        const u8 a = w.Fresh();
        w.CpuPage(2, true, a);
        const u8 gpu2 = w.Fresh();
        w.GpuWriteAll(gpu2);
        const u8 b = w.Fresh();
        w.CpuPage(4, true, b);
        w.Download();
        Check(w.PageIs(2, gpu2, false) && w.PageIs(4, b, false), "CPU page A, GPU, CPU page B: A is GPU, B is CPU");
        w.Settle();
        Check(w.Consistent() && w.Balanced(), "CPU, GPU, CPU settles consistently");
    }
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        const u8 gpu1 = w.Fresh();
        w.GpuWriteAll(gpu1);
        const u8 cpu = w.Fresh();
        w.CpuPage(2, true, cpu);
        const u8 gpu2 = w.Fresh();
        w.GpuPages(4, 5, gpu2);
        w.Download();
        Check(w.PageIs(2, cpu, false) && w.PageIs(2, cpu, true) && w.PageIs(4, gpu2, false) && w.PageIs(1, gpu1, false),
              "CPU between two GPU writes: the CPU page is uploaded before the second GPU write");
    }
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        w.GpuWriteAll(w.Fresh());
        w.Download();
        for (u64 page = 0; page < w.Pages(); ++page) w.CpuPage(page, true, w.Fresh());
        Check(w.owner.AllDirty(), "every page written by the CPU is reported as dirty");
        w.Download();
        w.Settle();
        Check(w.Consistent() && w.Balanced() && !w.owner.AllDirty(), "all pages dirty: every page keeps CPU data");
    }
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        const u8 gpu1 = w.Fresh();
        w.GpuWriteAll(gpu1);
        w.Download();
        const u8 cpu = w.Fresh();
        w.CpuStore(w.begin, w.begin + 64, cpu);
        w.Download();
        Check(w.guest[w.At(w.begin)] == cpu && w.PageIs(1, gpu1, false), "dirty first partial page keeps CPU data and nothing else");
        const u8 tail = w.Fresh();
        w.CpuStore(w.end - 64, w.end, tail);
        w.Download();
        Check(w.guest[w.At(w.end - 1)] == tail && w.PageIs(w.Pages() - 2, gpu1, false), "dirty last partial page keeps CPU data and nothing else");
        w.Settle();
        Check(w.Consistent() && w.Balanced(), "partial boundary pages settle consistently");
    }
}

void SharedPageCases() {
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        const u8 gpu1 = w.Fresh();
        w.GpuWriteAll(gpu1);
        const u8 before = w.Fresh();
        w.CpuStore(w.region_begin, w.begin, before);
        w.Download();
        Check(w.guest[w.At(w.region_begin)] == before && w.guest[w.At(w.begin)] == gpu1,
              "a write before the image in its first page does not dirty the image");
        const u8 after = w.Fresh();
        w.CpuStore(w.end, w.region_end, after);
        w.Download();
        Check(w.guest[w.At(w.end)] == after && w.guest[w.At(w.end - 1)] == gpu1,
              "a write after the image in its last page does not dirty the image");
        w.Settle();
        Check(w.Consistent() && w.Balanced(), "neighbour bytes are never changed by downloads");
    }
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        w.GpuWriteAll(w.Fresh());
        w.Download();
        const u8 cpu = w.Fresh();
        w.CpuStore(w.region_begin, w.region_begin + kPage, cpu);
        w.Download();
        Check(w.guest[w.At(w.begin)] == cpu, "a write that reaches the image through the shared page is detected before the download");
        w.Settle();
        Check(w.Consistent() && w.Balanced(), "late boundary detection settles consistently");
    }
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        w.GpuWriteAll(w.Fresh());
        w.Download();
        const u8 cpu = w.Fresh();
        w.CpuStore(w.region_begin, w.region_begin + kPage, cpu);
        const u8 blit = w.Fresh();
        w.FlagOnlyGpuWrite(blit);
        w.Settle();
        Check(w.PageIs(0, blit, false) && w.PageIs(0, blit, true), "an older CPU write seen late does not replace a newer GPU write");
    }
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        const u8 blit0 = w.Fresh();
        w.FlagOnlyGpuWrite(blit0);
        w.CpuStore(w.region_begin, w.begin, w.Fresh());
        const u8 blit = w.Fresh();
        w.FlagOnlyGpuWrite(blit);
        w.CpuStore(w.begin, w.begin + 16, w.Fresh());
        w.Settle();
        Check(w.guest[w.At(w.begin)] == blit, "an ambiguous late boundary write is resolved for the GPU");
    }
}

void WatcherBalanceCases() {
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        const int pages = static_cast<int>(w.Pages());
        Check(ledger.Total() == pages, "every page of the image is watched once");
        w.CpuPage(2, true, w.Fresh());
        Check(ledger.Total() == pages - 1 && !ledger.Watched(w.PageStart(2) / kPage) && ledger.Watched(w.PageStart(3) / kPage),
              "a fault releases only its page");
        w.CpuPage(4, true, w.Fresh());
        w.CpuPage(5, true, w.Fresh());
        Check(ledger.Total() == pages - 3 && w.Balanced(), "several faults release several pages");
        w.CpuPage(3, true, w.Fresh());
        Check(!ledger.Watched(w.PageStart(3) / kPage), "a later write to another page still faults");
        w.Untrack();
        Check(ledger.Total() == 0 && !ledger.negative, "untracking releases only the pages still watched");
        Check(w.owner.AnyDirty(), "dirty pages survive an untrack until they are consumed");
        w.owner.TakeDirty();
        Check(ledger.Total() == 0 && !ledger.negative, "consuming dirty pages of an untracked image watches nothing");
    }
    {
        Ledger ledger;
        {
            World w(ledger, kBase, 1024, 20, 1024, true);
            w.CpuPage(2, true, w.Fresh());
            w.CpuPage(3, true, w.Fresh());
            w.Untrack();
        }
        Check(ledger.Total() == 0 && !ledger.negative, "an image that adopted existing watches releases them exactly");
        World again(ledger, kBase, 1024, 20, 1024);
        Check(ledger.Total() == static_cast<int>(again.Pages()) && again.owner.HeldWatches() == again.Pages(),
              "an image recreated at the same address starts with no stale watch or dirty page");
        Check(!again.owner.AnyDirty(), "recreated image has no dirty pages");
        again.Untrack();
        Check(ledger.Total() == 0, "recreated image untracks cleanly");
    }
    {
        Ledger ledger;
        World w(ledger, kBase, 1024, 20, 1024);
        w.CpuStore(w.region_begin, w.begin, w.Fresh());
        Check(w.head_untracked && ledger.Total() == static_cast<int>(w.Pages()) - 1, "a head fault releases only the head page");
        w.CpuStore(w.begin + kPage, w.begin + kPage + 8, w.Fresh());
        w.Untrack();
        w.owner.TakeDirty();
        Check(ledger.Total() == 0 && !ledger.negative, "head, dirty and tail pages are released exactly once");
    }
}

void LinearRectCases() {
    for (const auto& shape : {std::array<u32, 3>{1024, 20, 1024}, {1000, 17, 1024}, {300, 40, 320}, {7, 100, 9}}) {
        const u32 width = shape[0], height = shape[1], pitch = shape[2];
        const VAddr base = 0x70000000 + 0x44;
        const u64 bytes = u64{pitch} * height * kTexel;
        bool exact = true;
        for (u64 from = 0; from < bytes && exact; from += 996) {
            for (u64 length : {4ull, 100ull, 4096ull, 20000ull}) {
                const ByteRange run{base + from, base + std::min(bytes, from + length)};
                std::vector<int> covered(bytes, 0);
                for (const TexelRect& rect : LinearRects(run, base, pitch, width, height, kTexel)) {
                    for (u32 row = 0; row < rect.height; ++row) {
                        for (u32 b = 0; b < rect.width * kTexel; ++b) ++covered[rect.buffer_offset + u64{row} * pitch * kTexel + b];
                    }
                }
                for (u64 b = 0; b < bytes; ++b) {
                    const bool texel = b % (u64{pitch} * kTexel) < u64{width} * kTexel;
                    const bool expected = texel && base + b >= run.begin && base + b < run.end;
                    if (covered[b] != (expected ? 1 : 0)) exact = false;
                }
            }
        }
        Check(exact, "linear runs map to exactly their texels (" + std::to_string(width) + "x" + std::to_string(height) + " pitch " + std::to_string(pitch) + ")");
    }
}

void OwnershipFuzz() {
    uint32_t state = 12345;
    const auto next = [&](uint32_t limit) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % limit;
    };
    const std::array<std::array<u32, 3>, 3> shapes{{{1024, 20, 1024}, {1000, 17, 1024}, {300, 40, 320}}};
    const VAddr bases[] = {0x40000000, 0x40000300, 0x40000FF0, 0x40000004};
    int failures = 0, runs = 0;
    std::string first;
    for (int run = 0; run < 3000; ++run) {
        const auto& shape = shapes[next(3)];
        Ledger ledger;
        World w(ledger, bases[next(4)], shape[0], shape[1], shape[2], next(2) == 0);
        std::string log;
        bool ok = true;
        const int steps = 4 + next(24);
        for (int step = 0; step < steps && ok; ++step) {
            const u64 page = next(static_cast<uint32_t>(w.Pages()));
            switch (next(7)) {
            case 0:
            case 1:
                w.CpuPage(page, next(2) == 0, w.Fresh());
                log += "C" + std::to_string(page) + " ";
                break;
            case 2:
                if (w.begin & (kPage - 1)) w.CpuStore(w.region_begin, w.begin, w.Fresh());
                log += "Nh ";
                break;
            case 3:
                if (w.end & (kPage - 1)) w.CpuStore(w.end, w.region_end, w.Fresh());
                log += "Nt ";
                break;
            case 4: {
                const u64 last = std::min<u64>(w.Pages() - 1, page + next(4));
                w.GpuPages(page, last, w.Fresh());
                log += "G" + std::to_string(page) + "-" + std::to_string(last) + " ";
                break;
            }
            case 5:
                w.Download();
                log += "D ";
                break;
            default:
                w.GpuWriteAll(w.Fresh());
                log += "GA ";
                break;
            }
            ok = w.Balanced();
        }
        w.Settle();
        const bool balanced_before = ok;
        const bool consistent = w.Consistent();
        const bool balanced = w.Balanced();
        w.Untrack();
        w.owner.TakeDirty();
        const bool released = ledger.Total() == 0 && !ledger.negative;
        ok = balanced_before && consistent && balanced && released;
        ++runs;
        if (!ok && ++failures == 1) {
            first = log + "[balanced-during=" + std::to_string(balanced_before) + " consistent=" + std::to_string(consistent) +
                    " balanced-after=" + std::to_string(balanced) + " released=" + std::to_string(released) + " base=" + std::to_string(w.begin & 0xFFFF) + " pages=" + std::to_string(w.Pages()) + " " + w.mismatch + "]";
        }
    }
    Check(failures == 0, "randomized CPU/GPU/boundary sequences: newest writer wins, watches balanced (" + std::to_string(runs) +
                             " runs, first failure: " + first + ")");
}

void OwnershipWiring() {
    const auto cache = Source("gpu/shadps4/video_core/texture_cache/texture_cache.cpp");

    const auto fault = Between(cache, "void TextureCache::InvalidateMemory(", "void TextureCache::InvalidateMemoryFromGPU");
    Check(Has(fault, "RecordCpuWrite(image, addr, written)") && Has(fault, "first_byte_only") && Has(fault, "UntrackImage(image_id);"),
          "a fault on a page-owned image records its page and leaves the other pages watched");

    const auto eligible = Between(cache, "bool TextureCache::UsesPageOwnership(", "bool TextureCache::RecordCpuWrite");
    Check(Has(eligible, "!info.props.is_tiled") && Has(eligible, "!info.props.is_depth") && Has(eligible, "!info.props.is_block") &&
              Has(eligible, "num_samples == 1") && Has(eligible, "GpuModified"),
          "page ownership applies only to single-sample linear color images the GPU wrote");

    const auto track = Between(cache, "void TextureCache::TrackImage(", "static void ApplyPageWatch");
    Check(!Has(track, "tracker.UpdatePageWatchers") && Has(track, "WatchImage(image, image_begin, image.info.guest_size, true)"),
          "tracking goes through the per-image watch ledger");
    const auto untrack = Between(cache, "void TextureCache::UntrackImage(", "void TextureCache::GarbageCollectImages");
    Check(!Has(untrack, "tracker.UpdatePageWatchers") && Has(untrack, "WatchImage(image, addr, size, false)"),
          "untracking goes through the per-image watch ledger");

    const auto refresh = Between(cache, "bool TextureCache::RefreshDirtyPages(", "void TextureCache::PublishDownload");
    Check(Has(refresh, "AllDirty()"), "a fully CPU-written image takes the whole-image upload");
    Check(Has(refresh, "TakeDirty()") && Has(refresh, "LinearRects(") && Has(refresh, "UploadImage(&image, buffer, copies)"),
          "a refresh uploads only the CPU-written pages");
    const auto refresh_whole = Between(cache, "void TextureCache::RefreshImage(", "vk::Sampler TextureCache::GetSampler");
    Check(Has(refresh_whole, "RefreshDirtyPages(image)") && Has(refresh_whole, "page_owner->ClearDirty()"),
          "a whole-image upload consumes the dirty pages");

    const auto publish = Between(cache, "void TextureCache::PublishDownload(", "void TextureCache::MarkDirtyIfEdgesModified");
    Check(Has(publish, "MarkDirtyIfEdgesModified(image)") && Has(publish, "ForEachCleanRun("),
          "a download writes only pages the CPU did not write, after the boundary check");

    const auto edge = Between(cache, "void TextureCache::NoteEdgeWrite(", "bool TextureCache::RefreshDirtyPages");
    Check(Has(edge, "gpu_write_serial != serial_at_untrack"), "a boundary write seen after a GPU write is not applied");

    const auto texture = Between(cache, "ImageView& TextureCache::FindTexture(", "ImageView& TextureCache::FindRenderTarget");
    const auto target = Between(cache, "ImageView& TextureCache::FindRenderTarget(", "ImageView& TextureCache::FindDepthTarget");
    Check(Has(texture, "NoteGpuWrite()") && Has(target, "NoteGpuWrite()"), "storage and render target binds supersede older CPU pages");
    const auto runtime = Source("gpu/shadps4/video_core/renderer_vulkan/vk_runtime.cpp");
    size_t notes = 0;
    for (auto at = runtime.find("dst->NoteGpuWrite();"); at != std::string::npos; at = runtime.find("dst->NoteGpuWrite();", at + 1)) ++notes;
    Check(notes == 6, "every copy, resolve and clear supersedes older CPU pages");
    Check(Has(cache, "dst.NoteGpuWrite();"), "alias copies supersede older CPU pages");

    const auto download = Between(cache, "void TextureCache::ProcessDownloadImages()", "std::optional<TextureCache::PendingDownload> TextureCache::RecordImageDownload");
    Check(Has(download, "PublishDownload(download)") && !Has(download, "scoped_lock lock{mutex}") && !Has(download, "std::scoped_lock{mutex}"),
          "download publication takes no texture cache mutex");

    const auto tiled_refresh = Between(cache, "bool TextureCache::RefreshDirtyPages(", "void TextureCache::PublishDownload");
    Check(Has(tiled_refresh, "tile_manager.MergeGuestPages(image, buffer, offset, ranges)") && Has(tiled_refresh, "TakeDirty()") &&
              tiled_refresh.find("TakeDirty()") < tiled_refresh.find("MergeGuestPages"),
          "a tiled refresh takes its dirty pages first and merges them without holding the ownership lock");
    Check(Has(eligible, "TileManager::CanMergeGuestPages(info)"), "tiled page ownership applies only to shapes the merge supports");

    const auto tiles = Source("gpu/shadps4/video_core/texture_cache/tile_manager.cpp");
    const auto merge = Between(tiles, "void TileManager::MergeGuestPages(", "void TileManager::TileImage(");
    const auto tile_at = merge.find("TileImage(image, download_copies");
    const auto overlay_at = merge.find("runtime.CopyBuffer(guest");
    const auto detile_at = merge.find("DetileImage(guest_layout.buffer");
    const auto upload_at = merge.find("runtime.UploadImage(&image");
    Check(tile_at != std::string::npos && tile_at < overlay_at && overlay_at < detile_at && detile_at < upload_at,
          "the merge tiles the GPU image, overlays the CPU pages in guest layout, detiles and uploads");
    const auto supported = Between(tiles, "bool TileManager::CanMergeGuestPages(", "boost::container::small_vector<vk::BufferImageCopy, 14> TileManager::MipCopies");
    const auto depth_plane = Between(tiles, "static bool DepthPlaneIsBitExact(", "bool TileManager::CanMergeGuestPages(");
    Check(Has(supported, "!info.props.is_block") && Has(supported, "num_samples == 1") && Has(supported, "DepthPlaneIsBitExact(info)"),
          "block-compressed and multisampled images never use the merge, and depth only as a bit-exact plane");
    Check(Has(depth_plane, "eD32Sfloat") && Has(depth_plane, "eD16Unorm") && !Has(depth_plane, "eD24"),
          "only the depth formats whose depth aspect equals the guest plane use the merge");
    Check(Has(Between(tiles, "boost::container::small_vector<vk::BufferImageCopy, 14> TileManager::MipCopies", "void TileManager::MergeGuestPages("), "eDepth") &&
              Has(Between(tiles, "boost::container::small_vector<vk::BufferImageCopy, 14> TileManager::MipCopies", "void TileManager::MergeGuestPages("), "is_volume"),
          "merge copies address the depth aspect and every volume slice");
    Check(Has(eligible, "image_ci.format == info.pixel_format"), "a depth image whose host format differs from the guest plane is not page-owned");

    const auto shader = Source("gpu/shadps4/video_core/host_shaders/tiling.comp");
    Check(Has(shader, "micro_mips") && Has(shader, "ComputeSurfaceAddrFromCoordMicroTiled(x, y, slice, pitch, height, 0)") && Has(shader, "ComputeSurfaceAddrFromCoordMacroTiled(x, y, slice, pitch, height, 0)"),
          "mips below one macro tile use micro addressing in both directions");
    const auto layout = Source("gpu/shadps4/video_core/texture_cache/tile.h");
    Check(Has(layout, "DowngradesToMicro(pitch, height, tile_mode, bpp, num_samples, mip_n, alt)"),
          "the size rule and the addressing rule share one downgrade predicate");
    const auto info_source = Source("gpu/shadps4/video_core/texture_cache/image_info.cpp");
    Check(Has(info_source, "micro_mips |= 1u << mip"), "image info records which mips are micro tiled");
    Check(Has(Between(tiles, "std::pair<const Buffer*, u64> TileManager::DetileImage(", "void TileManager::TileImage(") , "params.micro_mips = info.micro_mips") &&
              Has(tiles.substr(tiles.find("void TileManager::TileImage(")), "params.micro_mips = info.micro_mips"),
          "tiler and detiler both pass the micro mip mask");
}

} // namespace

int main() {
    AddressReuseLayouts();
    CacheWiring();
    FenceOrdering();
    LinearOwnershipCases();
    SharedPageCases();
    WatcherBalanceCases();
    LinearRectCases();
    OwnershipFuzz();
    OwnershipWiring();
    if (g_failures) {
        std::printf("%d of %d image reuse checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("all %d image reuse checks passed\n", g_checks);
    return 0;
}
