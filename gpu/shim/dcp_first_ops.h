// SPDX-License-Identifier: GPL-2.0-or-later
// dcp: the first time each GPU operation behind the game's first frame starts and ends. The renderer's own
// "first present" comes before guest entry; between it and the game's first frame ("Runtime: first present") the guest's
// first shader translation, SPIR-V module, pipeline creations, draw and swapchain acquire happen, and a crash there must
// name the one it was in. Each operation prints at most two lines per process ("Runtime: first <op> started: <detail>",
// "Runtime: first <op> finished: <result>"), so the log never fills; the Bug Report takes "Runtime: first ..." lines as
// the last startup stage. State is lock-free and readable from a fault handler's best-effort report (Describe).
// No Vulkan headers: results are passed as their integer VkResult values, tested headless (tests/first_ops_tests.cpp).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace DcpFirstOps {

enum Op : unsigned {
    ShaderTranslate,
    SpirvEmit,
    GraphicsPipeline,
    ComputePipeline,
    Draw,
    Dispatch,
    Acquire,
    OpCount,
};

inline constexpr std::array<const char*, OpCount> Names{
    "shader translation", "SPIR-V emission",  "graphics pipeline creation", "compute pipeline creation",
    "guest draw",         "guest dispatch",   "swapchain acquire",
};
// Short names for the one-line summary in crash reports.
inline constexpr std::array<const char*, OpCount> Keys{
    "translate", "spirv", "graphics_pipeline", "compute_pipeline", "draw", "dispatch", "acquire",
};

enum class State : int { None, Started, Finished, Failed };

inline constexpr std::size_t DetailChars = 120;

class Tracker {
public:
    using Print = void (*)(const char* line);

    constexpr explicit Tracker(Print print_ = nullptr) : print{print_} {}

    // True for the one caller that owns the first occurrence of `op`; only that caller may call Finish for it. Prints
    // "Runtime: first <op> started: <detail>".
    bool Begin(Op op, const char* detail) noexcept {
        if (op >= OpCount) {
            return false;
        }
        int expected = int(State::None);
        if (states[op].load(std::memory_order_relaxed) != expected ||
            !states[op].compare_exchange_strong(expected, int(State::Started), std::memory_order_acq_rel)) {
            return false;
        }
        CopyText(details[op].data(), details[op].size(), detail);
        detail_ready[op].store(true, std::memory_order_release);
        char line[64 + DetailChars];
        std::snprintf(line, sizeof(line), "Runtime: first %s started: %s", Names[op], details[op].data());
        Emit(line);
        return true;
    }

    // `result` 0 is success (VK_SUCCESS); `result_text` names it ("Success", "ErrorOutOfDeviceMemory", "translated").
    void Finish(Op op, int result, const char* result_text) noexcept {
        if (op >= OpCount) {
            return;
        }
        int expected = int(State::Started);
        if (!states[op].compare_exchange_strong(expected, int(result == 0 ? State::Finished : State::Failed),
                                                std::memory_order_acq_rel)) {
            return;
        }
        char line[160];
        std::snprintf(line, sizeof(line), "Runtime: first %s finished: %s (result %d)", Names[op],
                      result_text && result_text[0] ? result_text : "?", result);
        Emit(line);
    }

    // An operation without a separate end (a draw recorded): one "Runtime: first <op>: <detail>" line.
    void Once(Op op, const char* detail) noexcept {
        if (op >= OpCount) {
            return;
        }
        int expected = int(State::None);
        if (states[op].load(std::memory_order_relaxed) != expected ||
            !states[op].compare_exchange_strong(expected, int(State::Finished), std::memory_order_acq_rel)) {
            return;
        }
        CopyText(details[op].data(), details[op].size(), detail);
        detail_ready[op].store(true, std::memory_order_release);
        char line[64 + DetailChars];
        std::snprintf(line, sizeof(line), "Runtime: first %s: %s", Names[op], details[op].data());
        Emit(line);
    }

    [[nodiscard]] State Get(Op op) const noexcept {
        return op < OpCount ? State(states[op].load(std::memory_order_acquire)) : State::None;
    }

    // "translate=done spirv=done graphics_pipeline=STARTED [vs 0x... ps 0x...] draw=none ...": what was in flight when a
    // fault ended the process. Always NUL-terminated; returns the length.
    std::size_t Describe(char* out, std::size_t size) const noexcept {
        if (!out || !size) {
            return 0;
        }
        out[0] = '\0';
        std::size_t used = 0;
        for (unsigned op = 0; op < OpCount; ++op) {
            const State state = Get(Op(op));
            const char* text = state == State::None       ? "none"
                               : state == State::Started  ? "STARTED"
                               : state == State::Finished ? "done"
                                                          : "FAILED";
            int written;
            if (state == State::Started && detail_ready[op].load(std::memory_order_acquire)) {
                written = std::snprintf(out + used, size - used, "%s%s=%s [%s]", used ? " " : "", Keys[op], text,
                                        details[op].data());
            } else {
                written = std::snprintf(out + used, size - used, "%s%s=%s", used ? " " : "", Keys[op], text);
            }
            if (written < 0) {
                break;
            }
            used = std::min(size - 1, used + std::size_t(written));
            if (used + 1 >= size) {
                break;
            }
        }
        return used;
    }

private:
    static void CopyText(char* out, std::size_t size, const char* text) noexcept {
        std::size_t i = 0;
        for (; text && text[i] && i + 1 < size; ++i) {
            const char c = text[i];
            out[i] = (c == '\n' || c == '\r') ? ' ' : c;
        }
        out[i] = '\0';
    }

    void Emit(const char* line) const noexcept {
        if (print) {
            print(line);
        } else {
            std::printf("%s\n", line);
        }
    }

    Print print;
    std::array<std::atomic<int>, OpCount> states{};
    std::array<std::atomic<bool>, OpCount> detail_ready{};
    std::array<std::array<char, DetailChars>, OpCount> details{};
};

// The process-wide tracker (gpu/shim/dcp_startup_watch.cpp).
Tracker& Global() noexcept;

} // namespace DcpFirstOps
