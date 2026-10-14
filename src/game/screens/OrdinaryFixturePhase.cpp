#include "game/screens/OrdinaryFixturePhase.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "game/screens/PartyHelpAdmission.h"

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
    std::vector<LevelFixtures::NamedSound> sounds;
    bool lesson = false;
    const LevelFixtures::Events events{.help = [&](s32 id, usize index) {
        lesson |= !PartyHelpAdmission::suppressed(id, index, party);
        // A proven suppressed native request returns false. A new request
        // refuses the whole transaction rather than changing its help owner.
        return false;
    }};
    auto result = fixtures.updatePhase(ticks, seconds, party, nullptr, [&](const GateEvent& event) {
        fixtures.dispatchGateEvent(event, party, events, [&](std::string_view name, f32 volume) {
            sounds.push_back({std::string{name}, volume});
        });
        return !lesson;
    });
    if (!result.refusal) {
        result.sounds = std::move(sounds);
    }
    return result;
}
} // namespace gdl::game
