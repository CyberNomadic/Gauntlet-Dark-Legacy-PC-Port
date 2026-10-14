#include "game/screens/OrdinaryFixturePhase.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {
LevelFixtures::Phase OrdinaryFixturePhase::advance(LevelFixtures& fixtures, s32 ticks, f32 seconds,
                                                   std::span<PlayerRuntime> party) {
    const auto valid = [](const PlayerRuntime& runtime) {
        const auto& actor = runtime.actor;
        const auto position = actor.position();
        return std::isfinite(position.x) && std::isfinite(position.y) &&
               std::isfinite(position.z) && std::isfinite(actor.radius()) && actor.radius() >= 0 &&
               std::isfinite(actor.height()) && actor.height() >= 0 &&
               std::isfinite(actor.reach()) && actor.reach() >= 0;
    };
    if (ticks < 1 || ticks > 2 || !std::isfinite(seconds) ||
        std::abs(seconds * 60 - static_cast<f32>(ticks)) > 0.0001f || party.empty() ||
        party.size() > 4 || !std::ranges::all_of(party, valid)) {
        return {LevelFixtures::Boundary::InvalidStep};
    }
    return fixtures.updatePhase(ticks, seconds, party, nullptr);
}
} // namespace gdl::game
