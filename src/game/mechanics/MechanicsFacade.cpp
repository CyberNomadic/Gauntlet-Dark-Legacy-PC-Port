#include "game/mechanics/MechanicsFacade.h"

#include "game/party/PlayerPowerups.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

const MechanicsFacade& MechanicsFacade::standard() {
    static const MechanicsFacade kStandard;
    return kStandard;
}

RunEffect MechanicsFacade::runEffect(BlowSource source, BlowOutcome outcome,
                                     const PlayerRuntime& striker,
                                     std::span<const PlayerRuntime> players) const {
    if (!m_settings.aggressorMode) {
        // The original counted its swings on the swarm, the great ones and the bosses alone.
        return source == BlowSource::Swing && outcome != BlowOutcome::Destroyed
                   ? RunEffect::CountAsOriginal
                   : RunEffect::Nothing;
    }
    // Nothing struck while the swarm stands still or cannot strike back is earned.
    const PowerupEffects worn = PowerupEffects::of(striker.actor.save().progress().inventory);
    if (PlayerPowerups::timeStopped(players) || worn.invulnerable() || worn.invisible()) {
        return RunEffect::Nothing;
    }
    return outcome == BlowOutcome::Hurt ? RunEffect::Refresh : RunEffect::Count;
}

bool MechanicsFacade::generatorTakes(BlowSource source) const {
    return !m_settings.aggressorMode || source == BlowSource::Swing || source == BlowSource::Shot;
}

} // namespace gdl::game
