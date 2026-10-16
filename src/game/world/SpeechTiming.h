#pragma once

#include <algorithm>
#include <cmath>

#include "engine/audio/SoundClip.h"
#include "engine/core/Types.h"

namespace gdl::game {

/** Gameplay speech duration, advanced only by simulation time. Device playback,
 * volume and asynchronous completion never change this value. An authored loop
 * has no completion until its owning ceremony explicitly clears it. */
struct SpeechTiming {
    f64 remainingSeconds = 0;
    bool looping = false;
    template <class Visitor> friend void visitNativeState(Visitor& visitor, const SpeechTiming& state) {
        visitor.template fields<2>(state);
    }

    static SpeechTiming of(const SoundSequence& sequence) {
        SpeechTiming result;
        for (const auto& step : sequence.steps) {
            if (step.clip != nullptr) {
                result.remainingSeconds += step.clip->seconds();
            }
        }
        result.looping = result.remainingSeconds > 0 && sequence.loops();
        return result;
    }
    bool playing() const { return looping || remainingSeconds > 0; }
    void advance(f64 seconds) {
        if (!looping && std::isfinite(seconds) && seconds > 0) {
            remainingSeconds = std::max(remainingSeconds - seconds, 0.0);
        }
    }
    void append(const SpeechTiming& next) {
        remainingSeconds += next.remainingSeconds;
        looping |= next.looping;
    }
    bool operator==(const SpeechTiming&) const = default;
};

} // namespace gdl::game
