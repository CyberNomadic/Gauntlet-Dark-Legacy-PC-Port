#include "game/world/VoiceQueue.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>

#include "engine/core/Log.h"

namespace gdl::game {

bool VoiceQueue::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("VoiceQueue.native.1");
    digest.value(m_sim);
    return digest.good();
}
bool VoiceQueue::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    digest.string("VoiceQueue.native.1");
    digest.value(saved.m_values);
    return digest.good();
}

void VoiceQueue::saveLocal(LocalSnapshot& saved) const {
    saved.m_values = m_sim;
    saved.m_owner = m_lifetime;
    saved.m_epoch = m_localEpoch;
    saved.m_soundEpoch = m_events != nullptr ? m_events->epoch() : 0;
}
bool VoiceQueue::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner.lock() == m_lifetime && saved.m_epoch == m_localEpoch &&
           saved.m_soundEpoch == (m_events != nullptr ? m_events->epoch() : 0);
}
bool VoiceQueue::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_values;
    return true;
}
bool VoiceQueue::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_values == right.m_values;
}
void VoiceQueue::bind(SoundPlayer* output) {
    ++m_localEpoch;
    m_output = output;
}
void VoiceQueue::recordTo(SceneSoundEvents* events) {
    const bool changedEpoch =
        events != nullptr && m_sim.tail && m_sim.tail.event.epoch != events->epoch();
    if (events == m_events && !changedEpoch) {
        return;
    }
    if (m_events != nullptr && m_events->recording()) {
        m_events->unsupported("changing a voice queue recorder during a native tick");
    }
    if (events != nullptr) {
        if (events->epoch() == 0 || events->recording()) {
            events->unsupported("voice queue attachment requires an open idle sound epoch");
        }
        if (backlog() > 0) {
            events->unsupported("voice queue must drain before attaching another sound epoch");
        }
    }
    // A predecessor belongs to its output lifetime, not just to this queue.
    // Keep admission clocks and holds intact, but never chain a new epoch to a
    // discarded logical cue or to an earlier offline mixer handle.
    ++m_localEpoch;
    m_events = events;
    m_sim.tail = {};
    m_tail = kNoSound;
}

bool VoiceQueue::room(f32 maxWait) const {
    if (m_sim.held) {
        return false;
    }
    const f64 waiting = backlog();
    if (waiting > 0.0 && m_sim.lines.size() >= kMost) {
        return false;
    }
    return maxWait < 0.0f || waiting <= static_cast<f64>(maxWait);
}

f64 VoiceQueue::backlog() const {
    f64 remaining = 0;
    for (const auto& line : m_sim.lines) {
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

SoundHandle VoiceQueue::queue(const SoundSequence& sequence, f32 volume, f32 pan) {
    if (m_sim.held || m_sim.lines.size() >= kMost) {
        return kNoSound;
    }
    const auto timing = SpeechTiming::of(sequence);
    if (!timing.playing()) {
        return kNoSound;
    }
    if (m_events != nullptr) {
        const auto cue = m_events->emit(sequence, volume, pan, m_sim.tail);
        m_sim.lines.push_back(timing);
        m_sim.tail = cue;
        return kNoSound; // A logical token is never masqueraded as a mixer handle.
    }
    m_sim.lines.push_back(timing);
    SoundHandle handle = kNoSound;
    if (m_output != nullptr) {
        try {
            handle = m_output->playAfter(m_tail, sequence, volume, SoundCategory::Effects);
            if (handle != kNoSound && pan != 0) {
                m_output->setPan(handle, pan);
            }
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
    for (auto& line : m_sim.lines) {
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
    m_sim.lines.erase(m_sim.lines.begin(),
                      m_sim.lines.begin() + static_cast<std::ptrdiff_t>(finished));
}

void VoiceQueue::clear() {
    ++m_localEpoch;
    m_sim.lines.clear();
    m_sim.tail = {};
    m_tail = kNoSound;
    m_sim.held = false;
}

} // namespace gdl::game
