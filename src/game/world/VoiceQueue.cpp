#include "game/world/VoiceQueue.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>

#include "engine/core/Log.h"

namespace gdl::game {

bool VoiceQueue::room(f32 maxWait) const {
    if (m_held) {
        return false;
    }
    const f64 waiting = backlog();
    if (waiting > 0.0 && m_lines.size() >= kMost) {
        return false;
    }
    return maxWait < 0.0f || waiting <= static_cast<f64>(maxWait);
}

f64 VoiceQueue::backlog() const {
    f64 remaining = 0;
    for (const auto& line : m_lines) {
        if (line.looping) {
            return std::numeric_limits<f64>::infinity();
        }
        remaining += line.remainingSeconds;
    }
    return remaining;
}

f64 VoiceQueue::lengthOf(const SoundSequence& sequence) {
    return SpeechTiming::of(sequence).remainingSeconds;
}

SoundHandle VoiceQueue::queue(const SoundSequence& sequence, f32 volume) {
    if (m_held || m_lines.size() >= kMost) {
        return kNoSound;
    }
    const auto timing = SpeechTiming::of(sequence);
    if (!timing.playing()) {
        return kNoSound;
    }
    m_lines.push_back(timing);
    SoundHandle handle = kNoSound;
    if (m_output != nullptr) {
        try {
            handle = m_output->playAfter(m_tail, sequence, volume, SoundCategory::Effects);
        } catch (const std::exception& error) {
            // The admitted line still occupies its native simulation-time slot.
            // Optional output failure must not abort the rest of a spoken phrase.
            log::warn("Queued voice playback: {}", error.what());
        }
    }
    if (handle != kNoSound) {
        m_tail = handle;
    }
    return handle;
}

void VoiceQueue::update(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    f64 remaining = seconds;
    usize finished = 0;
    for (auto& line : m_lines) {
        if (line.looping) {
            break;
        }
        const f64 used = std::min(remaining, line.remainingSeconds);
        line.advance(used);
        remaining -= used;
        if (line.playing()) {
            break;
        }
        ++finished;
    }
    m_lines.erase(m_lines.begin(), m_lines.begin() + static_cast<std::ptrdiff_t>(finished));
}

void VoiceQueue::clear() {
    m_lines.clear();
    m_tail = kNoSound;
    m_held = false;
}

} // namespace gdl::game
