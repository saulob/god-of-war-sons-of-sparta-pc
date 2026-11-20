// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <mutex>
#include <vector>

#include "common/types.h"

namespace VideoCore {

struct ByteRange {
    VAddr begin;
    VAddr end;
    bool operator==(const ByteRange&) const = default;
};

struct TexelRect {
    u32 x, y, width, height;
    u64 buffer_offset;
    bool operator==(const TexelRect&) const = default;
};

struct PageWatcher {
    void* context = nullptr;
    void (*apply)(void* context, VAddr begin, u64 bytes, bool track) = nullptr;
    void operator()(VAddr begin, u64 bytes, bool track) const {
        apply(context, begin, bytes, track);
    }
};

// Per-image 4 KiB page ownership. A page is watched by the image exactly while it is wanted
// and not CPU-dirty, so watcher counts stay balanced however faults and track changes interleave.
class PageOwnership {
public:
    static constexpr u64 PageBits = 12;
    static constexpr u64 PageSize = u64{1} << PageBits;

    PageOwnership(VAddr begin, VAddr end, PageWatcher watcher)
        : image_begin{begin}, image_end{end}, first_page{begin >> PageBits},
          page_count{((end + PageSize - 1) >> PageBits) - (begin >> PageBits)}, watcher{watcher},
          wanted((page_count + 63) / 64), dirty((page_count + 63) / 64) {}

    // Marks pages as already watched by the image, without touching the watcher.
    void AdoptWatched(VAddr begin, VAddr end) {
        std::scoped_lock lock{mutex};
        ForPages(begin, end, [&](u64 page) { Set(wanted, page); });
    }

    void Watch(VAddr begin, VAddr end, bool track) {
        std::scoped_lock lock{mutex};
        RunBuilder run{*this, track};
        ForPagesAll(begin, end, [&](u64 page, bool inside) {
            if (!inside) return run.AddAt(page, true, true);
            const bool was_wanted = Get(wanted, page);
            if (track) Set(wanted, page);
            else Clear(wanted, page);
            run.AddAt(page, was_wanted != track && !Get(dirty, page), false);
        });
        run.Flush();
    }

    // Records a CPU write; returns whether it touched image bytes.
    bool MarkCpuWrite(VAddr address, u64 size) {
        const VAddr begin = std::max(address, image_begin);
        const VAddr end = std::min(address + size, image_end);
        if (begin >= end) return false;
        std::scoped_lock lock{mutex};
        RunBuilder run{*this, false};
        ForPages(begin, end, [&](u64 page) {
            const bool release = Get(wanted, page) && !Get(dirty, page);
            Set(dirty, page);
            run.Add(page, release);
        });
        run.Flush();
        return true;
    }

    std::vector<ByteRange> TakeDirty() {
        std::scoped_lock lock{mutex};
        std::vector<ByteRange> ranges;
        RunBuilder run{*this, true};
        u64 start = page_count;
        const auto close = [&](u64 stop) {
            if (start == page_count) return;
            ranges.push_back({std::max(PageAddress(start), image_begin), std::min(PageAddress(stop), image_end)});
            start = page_count;
        };
        for (u64 page = 0; page < page_count; ++page) {
            const bool is_dirty = Get(dirty, page);
            if (is_dirty) {
                if (start == page_count) start = page;
                Clear(dirty, page);
            } else {
                close(page);
            }
            run.Add(page, is_dirty && Get(wanted, page));
        }
        close(page_count);
        run.Flush();
        return ranges;
    }

    void ClearDirty() {
        if (AnyDirty()) TakeDirty();
    }

    bool AllDirty() const {
        std::scoped_lock lock{mutex};
        for (u64 page = 0; page < page_count; ++page) {
            if (!Get(dirty, page)) return false;
        }
        return true;
    }

    bool AnyDirty() const {
        std::scoped_lock lock{mutex};
        return std::any_of(dirty.begin(), dirty.end(), [](u64 word) { return word != 0; });
    }

    template <typename Callback>
    void ForEachCleanRun(VAddr begin, VAddr end, Callback&& callback) const {
        begin = std::max(begin, image_begin);
        end = std::min(end, image_end);
        if (begin >= end) return;
        std::scoped_lock lock{mutex};
        VAddr start = 0;
        bool open = false;
        ForPages(begin, end, [&](u64 page) {
            const bool clean = !Get(dirty, page);
            const VAddr page_begin = std::max(PageAddress(page), begin);
            if (clean && !open) {
                start = page_begin;
                open = true;
            } else if (!clean && open) {
                callback(ByteRange{start, page_begin});
                open = false;
            }
        });
        if (open) callback(ByteRange{start, end});
    }

    u64 HeldWatches() const {
        std::scoped_lock lock{mutex};
        u64 held = 0;
        for (u64 page = 0; page < page_count; ++page) held += Get(wanted, page) && !Get(dirty, page);
        return held;
    }

private:
    using Bits = std::vector<u64>;

    static bool Get(const Bits& bits, u64 page) { return (bits[page / 64] >> (page % 64)) & 1; }
    static void Set(Bits& bits, u64 page) { bits[page / 64] |= u64{1} << (page % 64); }
    static void Clear(Bits& bits, u64 page) { bits[page / 64] &= ~(u64{1} << (page % 64)); }

    VAddr PageAddress(u64 page) const { return (first_page + page) << PageBits; }

    template <typename F>
    void ForPages(VAddr begin, VAddr end, F&& f) const {
        const u64 lo = std::max(begin >> PageBits, first_page) - first_page;
        const u64 hi = std::min<u64>(((end + PageSize - 1) >> PageBits) - first_page, page_count);
        for (u64 page = lo; page < hi; ++page) f(page);
    }

    // Visits every page of the range; pages outside the image bitmap report inside == false.
    template <typename F>
    void ForPagesAll(VAddr begin, VAddr end, F&& f) const {
        const u64 lo = begin >> PageBits;
        const u64 hi = (end + PageSize - 1) >> PageBits;
        for (u64 abs = lo; abs < hi; ++abs) {
            const bool inside = abs >= first_page && abs - first_page < page_count;
            f(inside ? abs - first_page : abs, inside);
        }
    }

    // Coalesces consecutive pages that need the same watch change into one watcher call.
    struct RunBuilder {
        PageOwnership& owner;
        bool track;
        bool open = false;
        u64 first = 0, next = 0;
        bool absolute_run = false;

        void Add(u64 page, bool needed) { AddAt(page, needed, false); }
        void AddAt(u64 page, bool needed, bool absolute) {
            if (needed && open && page == next && absolute == absolute_run) {
                ++next;
                return;
            }
            Flush();
            if (needed) {
                open = true;
                first = page;
                next = page + 1;
                absolute_run = absolute;
            }
        }
        void Flush() {
            if (!open) return;
            const u64 base = absolute_run ? first : owner.first_page + first;
            owner.watcher(base << PageBits, (next - first) << PageBits, track);
            open = false;
        }
    };

    VAddr image_begin, image_end;
    u64 first_page, page_count;
    PageWatcher watcher;
    Bits wanted, dirty;
    mutable std::mutex mutex;
};

// Maps a byte run of a linear image to texel rectangles; padding bytes beyond the row width have none.
inline std::vector<TexelRect> LinearRects(ByteRange run, VAddr base, u32 pitch, u32 width, u32 height,
                                          u32 bytes_per_texel) {
    std::vector<TexelRect> rects;
    const u64 row_bytes = u64{pitch} * bytes_per_texel;
    const u64 width_bytes = u64{width} * bytes_per_texel;
    const u64 begin = run.begin - base;
    const u64 end = std::min<u64>(run.end - base, row_bytes * height);
    if (row_bytes == 0 || begin >= end) return rects;
    for (u64 row = begin / row_bytes; row <= (end - 1) / row_bytes; ++row) {
        const u64 row_start = row * row_bytes;
        const u64 from = std::max(begin, row_start) - row_start;
        const u64 to = std::min({end - row_start, row_bytes, width_bytes});
        if (from >= to) continue;
        TexelRect rect{static_cast<u32>(from / bytes_per_texel), static_cast<u32>(row),
                       static_cast<u32>((to - from) / bytes_per_texel), 1, row_start + from};
        if (!rects.empty()) {
            TexelRect& last = rects.back();
            if (last.x == rect.x && last.width == rect.width && last.y + last.height == rect.y) {
                ++last.height;
                continue;
            }
        }
        rects.push_back(rect);
    }
    return rects;
}

} // namespace VideoCore
