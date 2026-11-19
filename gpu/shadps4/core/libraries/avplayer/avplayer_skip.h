// SPDX-License-Identifier: GPL-2.0-or-later
// Skip Intro: the first source the game opens is its startup video. It still plays through the normal player, but only
// its first moments are demuxed, so the end of the file, the stop event and every state transition the game waits for
// happen exactly as after a natural end. Later sources (tutorial videos) are never affected.

#pragma once

#include <atomic>
#include <cstdint>

namespace Libraries::AvPlayer::SkipIntro {

// Long enough for the game's start-up seek to see frames with different timestamps.
inline constexpr std::uint64_t kPlayableMillis = 200;

class Policy {
public:
    void Configure(bool enabled) {
        m_enabled = enabled;
        m_sources_opened = 0;
    }

    bool Enabled() const {
        return m_enabled;
    }

    // Called once for every source the game opens, in order; true only for the first one.
    bool ClaimSource() {
        return m_sources_opened.fetch_add(1) == 0 && m_enabled;
    }

    static bool PastPlayableWindow(std::uint64_t packet_millis) {
        return packet_millis > kPlayableMillis;
    }

private:
    std::atomic<bool> m_enabled{false};
    std::atomic<unsigned> m_sources_opened{0};
};

inline Policy& Global() {
    static Policy policy;
    return policy;
}

} // namespace Libraries::AvPlayer::SkipIntro
