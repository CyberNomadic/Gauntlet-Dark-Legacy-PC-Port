#include "game/enemies/SwarmAggression.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {

bool SwarmAggression::valid() const {
    return std::isfinite(heat) && heat >= 0.0f && heat <= kMostMeasure && std::isfinite(rage) &&
           rage >= 0.0f && rage <= kMostMeasure;
}

void SwarmAggression::step(f32 seconds, s32 run) {
    if (!(seconds > 0.0f)) {
        return;
    }
    // The run over, the swarm is itself again at once.
    if (run <= 0) {
        heat = 0.0f;
        rage = 0.0f;
        return;
    }
    const f32 target = static_cast<f32>(run) / static_cast<f32>(kFullRun);
    heat = target > heat ? std::min(target, heat + kRise * seconds)
                         : std::max(target, heat - kCool * seconds);
    rage = run >= kFullRun ? std::min(kMostMeasure, rage + seconds / kRageSeconds) : 0.0f;
    heat = std::min(heat, kMostMeasure);
}

Color SwarmAggression::tint() const {
    const auto channel = [this](u8 full) {
        const f32 value = 255.0f + (static_cast<f32>(full) - 255.0f) * std::clamp(rage, 0.0f, 1.0f);
        return static_cast<u8>(std::lround(value));
    };
    return Color::rgba(channel(kRageTint.r), channel(kRageTint.g), channel(kRageTint.b));
}

} // namespace gdl::game
