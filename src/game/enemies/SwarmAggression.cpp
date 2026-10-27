#include "game/enemies/SwarmAggression.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {

bool SwarmAggression::valid() const {
    return std::isfinite(heat) && heat >= 0.0f && heat <= kMostMeasure && std::isfinite(rage) &&
           rage >= 0.0f && rage <= kMostMeasure && run >= 0;
}

void SwarmAggression::step(f32 seconds, s32 longest) {
    if (!(seconds > 0.0f)) {
        return;
    }
    run = std::max(longest, 0);
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

f32 SwarmAggression::elementalChance() const {
    if (run <= 0) {
        return 0.0f;
    }
    const s32 grade = std::max(StreakGrades::gradeOf(run), 0);
    return kElementalChanceAtD + kElementalChanceStep * static_cast<f32>(grade);
}

Color SwarmAggression::tint() const {
    const auto channel = [this](u8 full) {
        const f32 value = 255.0f + (static_cast<f32>(full) - 255.0f) * std::clamp(rage, 0.0f, 1.0f);
        return static_cast<u8>(std::lround(value));
    };
    return Color::rgba(channel(kRageTint.r), channel(kRageTint.g), channel(kRageTint.b));
}

} // namespace gdl::game
