// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded pass window for frame capture.
#pragma once

#include <cstddef>

namespace DcpCaptureWindow {

enum class Phase { Idle, Armed, Recording };
enum class Step { Skip, Begin, Record, Finish };

constexpr Step PassStep(Phase phase, std::size_t recorded, std::size_t max_passes) {
    switch (phase) {
    case Phase::Armed:
        return Step::Begin;
    case Phase::Recording:
        return recorded >= max_passes ? Step::Finish : Step::Record;
    default:
        return Step::Skip;
    }
}

} // namespace DcpCaptureWindow
