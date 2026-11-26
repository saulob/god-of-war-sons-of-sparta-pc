// SPDX-License-Identifier: GPL-2.0-or-later
// dcp: the attachments of the newest vkCmdBeginRendering and whether the driver is still inside it, for crash reports.
// Lock-free, readable by the fault handler on the crashing thread. No Vulkan headers (tests/intel_uhd630_tests.cpp).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace DcpRendering {

struct Shape {
    std::uint32_t colors{};         // colorAttachmentCount
    std::uint32_t unbound_colors{}; // bit i: pColorAttachments[i].imageView is VK_NULL_HANDLE
    bool depth{};
    bool stencil{};
    bool null_depth_stencil_view{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t layers{};
};

constexpr std::uint64_t Pack(const Shape& s) noexcept {
    return std::uint64_t(s.colors & 0xf) | std::uint64_t(s.unbound_colors & 0xff) << 4 | std::uint64_t(s.depth) << 12 |
           std::uint64_t(s.stencil) << 13 | std::uint64_t(s.null_depth_stencil_view) << 14 |
           std::uint64_t(s.width & 0xffff) << 16 | std::uint64_t(s.height & 0xffff) << 32 |
           std::uint64_t(s.layers & 0xffff) << 48;
}

constexpr Shape Unpack(std::uint64_t v) noexcept {
    return {std::uint32_t(v & 0xf),           std::uint32_t(v >> 4 & 0xff),      bool(v >> 12 & 1),
            bool(v >> 13 & 1),                bool(v >> 14 & 1),                 std::uint32_t(v >> 16 & 0xffff),
            std::uint32_t(v >> 32 & 0xffff), std::uint32_t(v >> 48 & 0xffff)};
}

inline int FormatShape(char* out, std::size_t size, const Shape& s) noexcept {
    return std::snprintf(out, size, "colors=%u unbound_colors=%#x depth=%s stencil=%s null_depth_stencil_view=%s area=%ux%u layers=%u",
                         s.colors, s.unbound_colors, s.depth ? "yes" : "no", s.stencil ? "yes" : "no",
                         s.null_depth_stencil_view ? "yes" : "no", s.width, s.height, s.layers);
}

class Tracker {
public:
    using Print = void (*)(const char* line);

    constexpr explicit Tracker(Print print_ = nullptr) : print{print_} {}

    // Just before vkCmdBeginRendering. The first pass with an unbound color attachment is logged once, so a log shows
    // whether the game reaches that shape even when nothing crashes.
    void Begin(const Shape& shape, std::uint32_t thread) noexcept {
        packed.store(Pack(shape), std::memory_order_relaxed);
        owner.store(thread, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        in_driver.store(true, std::memory_order_release);
        if (shape.unbound_colors && !logged_unbound.exchange(true, std::memory_order_relaxed)) {
            char line[200];
            int used = std::snprintf(line, sizeof(line), "Runtime: first rendering pass with an unbound color attachment: ");
            if (used > 0 && std::size_t(used) < sizeof(line)) {
                FormatShape(line + used, sizeof(line) - std::size_t(used), shape);
            }
            Emit(line);
        }
    }

    // After vkCmdBeginRendering returned.
    void End() noexcept {
        in_driver.store(false, std::memory_order_release);
    }

    // "vkCmdBeginRendering #12 IN PROGRESS on thread 8912: colors=2 unbound_colors=0x1 ...", always NUL-terminated.
    std::size_t Describe(char* out, std::size_t size) const noexcept {
        if (!out || !size) {
            return 0;
        }
        out[0] = '\0';
        const bool inside = in_driver.load(std::memory_order_acquire);
        const std::uint64_t number = count.load(std::memory_order_relaxed);
        if (!number) {
            std::snprintf(out, size, "no vkCmdBeginRendering recorded");
        } else {
            int used = std::snprintf(out, size, "vkCmdBeginRendering #%llu %s thread %u: ",
                                     static_cast<unsigned long long>(number),
                                     inside ? "IN PROGRESS on" : "returned on", owner.load(std::memory_order_relaxed));
            if (used > 0 && std::size_t(used) < size) {
                FormatShape(out + used, size - std::size_t(used), Unpack(packed.load(std::memory_order_relaxed)));
            }
        }
        std::size_t length = 0;
        while (length + 1 < size && out[length]) {
            ++length;
        }
        return length;
    }

    [[nodiscard]] bool InDriver() const noexcept {
        return in_driver.load(std::memory_order_acquire);
    }

private:
    void Emit(const char* line) const noexcept {
        if (print) {
            print(line);
        } else {
            std::printf("%s\n", line);
            std::fflush(stdout);
        }
    }

    Print print;
    std::atomic<std::uint64_t> packed{};
    std::atomic<std::uint64_t> count{};
    std::atomic<std::uint32_t> owner{};
    std::atomic<bool> in_driver{};
    std::atomic<bool> logged_unbound{};
};

// The process-wide tracker (gpu/shim/dcp_startup_watch.cpp).
Tracker& Global() noexcept;

} // namespace DcpRendering
