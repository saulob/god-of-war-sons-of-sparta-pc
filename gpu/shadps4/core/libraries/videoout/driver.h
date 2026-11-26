// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <vector>

#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "core/libraries/videoout/video_out.h"
#include "dcp_present.h"

#include <condition_variable>
#include <mutex>
#include <queue>

namespace VideoCore {
struct PresentFrame;
}

namespace Libraries::VideoOut {

struct VideoOutPort {
    SceVideoOutResolutionStatus resolution;
    std::array<VideoOutBuffer, MaxDisplayBuffers> buffer_slots;
    std::array<u64, MaxDisplayBuffers> buffer_labels; // should be contiguous in memory
    static_assert(sizeof(buffer_labels[0]) == 8u);
    std::array<BufferAttributeGroup, MaxDisplayBufferGroups> groups;
    FlipStatus flip_status;
    SceVideoOutVblankStatus vblank_status;
    std::vector<Kernel::OrbisKernelEqueue> flip_events;
    std::vector<Kernel::OrbisKernelEqueue> vblank_events;
    std::mutex vo_mutex;
    std::mutex port_mutex;
    std::condition_variable vo_cv;
    std::condition_variable vblank_cv;
    int flip_rate = 0;
    int prev_index = -1;
    bool is_open = false;
    bool is_hdr = false;

    s32 FindFreeGroup() const {
        s32 index = 0;
        while (index < groups.size() && groups[index].is_occupied) {
            index++;
        }
        return index;
    }

    bool IsVoLabel(const u64* address) const {
        const u64* start = &buffer_labels[0];
        const u64* end = &buffer_labels[MaxDisplayBuffers - 1];
        return address >= start && address <= end;
    }

    void WaitVoLabel(auto&& pred) {
        std::unique_lock lk{vo_mutex};
        vo_cv.wait(lk, pred);
    }

    void SignalVoLabel() {
        std::scoped_lock lk{vo_mutex};
        vo_cv.notify_one();
    }

    void AddFlipEvent(Kernel::OrbisKernelEqueue eq) {
        std::scoped_lock lock{port_mutex};
        flip_events.push_back(eq);
    }

    void RemoveFlipEvent(Kernel::OrbisKernelEqueue eq) {
        std::scoped_lock lock{port_mutex};
        if (const auto it = std::ranges::find(flip_events, eq); it != flip_events.end()) {
            flip_events.erase(it);
        }
    }

    void TriggerFlipEventsLocked(s64 flip_arg) {
        std::erase_if(flip_events, [flip_arg](Kernel::OrbisKernelEqueue eq) {
            const auto equeue = Kernel::AcquireEqueue(eq);
            if (!equeue) {
                return true;
            }
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (flip_arg << 16)));
            return equeue->IsClosed();
        });
    }

    void AddVblankEvent(Kernel::OrbisKernelEqueue eq) {
        std::scoped_lock lock{vo_mutex};
        vblank_events.push_back(eq);
    }

    void RemoveVblankEvent(Kernel::OrbisKernelEqueue eq) {
        std::scoped_lock lock{vo_mutex};
        if (const auto it = std::ranges::find(vblank_events, eq); it != vblank_events.end()) {
            vblank_events.erase(it);
        }
    }

    void TriggerVblankEventsLocked(u64 vblank_count) {
        std::erase_if(vblank_events, [vblank_count](Kernel::OrbisKernelEqueue eq) {
            const auto equeue = Kernel::AcquireEqueue(eq);
            if (!equeue) {
                return true;
            }
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                                        (vblank_count << 16)));
            return equeue->IsClosed();
        });
    }

    [[nodiscard]] int NumRegisteredBuffers() const {
        return std::count_if(buffer_slots.cbegin(), buffer_slots.cend(),
                             [](auto& buffer) { return buffer.group_index != -1; });
    }
};

struct ServiceThreadParams {
    u32 unknown;
    bool set_priority;
    u32 priority;
    bool set_affinity;
    u64 affinity;
};

class VideoOutDriver {
public:
    VideoOutDriver(u32 width, u32 height);
    ~VideoOutDriver();

    int Open(const ServiceThreadParams* params);
    void Close(s32 handle);

    VideoOutPort* GetPort(s32 handle);

    int RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses, s32 bufferNum,
                        const BufferAttribute* attribute);
    int UnregisterBuffers(VideoOutPort* port, s32 attributeIndex);
    int ChangeBufferAttribute(VideoOutPort* port, s32 bufferIndex,
                              const BufferAttribute* attribute);

    bool SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop = false);

private:
    struct Request {
        VideoCore::PresentFrame* frame;
        VideoOutPort* port;
        s64 flip_arg;
        s32 index;
        bool eop;
        std::chrono::steady_clock::time_point submitted;

        operator bool() const noexcept {
            return frame != nullptr;
        }
    };

    void Flip(const Request& req);
    void DrawBlankFrame(); // Video port out not open
    void DrawLastFrame();  // Used when there is no flip request
    void SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop = false);
    void PresentThread(std::stop_token token);
    /// Runs presenter work (acquire, blit, vkQueuePresent) off the vblank thread, in order:
    /// acquire and present block while the GPU is the bottleneck, which delayed the guest's
    /// vblank events on the vblank thread.
    void SwapThread(std::stop_token token);
    /// A redraw is dropped while anything is queued or running; a flip always queues.
    void RunPresenter(std::function<void()> work, bool redraw = false);
    bool FlipWaitsForPresenter();

    std::mutex swap_mutex;
    std::condition_variable_any swap_cv;
    DcpPresent::SwapQueue<std::function<void()>> swap_queue;
    std::jthread swap_thread; ///< before present_thread: destroyed after it

    std::mutex mutex;
    VideoOutPort main_port{};
    std::jthread present_thread;
    std::queue<Request> requests;
    u64 flips_consumed = 0; ///< present thread only
};

} // namespace Libraries::VideoOut
