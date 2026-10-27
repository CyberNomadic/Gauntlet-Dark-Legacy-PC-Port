#include "game/mechanics/MechanicsFacade.h"

#include "game/party/PlayerPowerups.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

const MechanicsFacade& MechanicsFacade::standard() {
    static const MechanicsFacade kStandard;
    return kStandard;
}

bool MechanicsFacade::blowBuildsRun(BlowSource source, const PlayerRuntime& striker,
                                    std::span<const PlayerRuntime> players) const {
    if (!m_settings.aggressorMode) {
        return source == BlowSource::Swing;
    }
    // Nothing struck while the swarm stands still or cannot strike back is earned.
    const PowerupEffects worn = PowerupEffects::of(striker.actor.save().progress().inventory);
    return !PlayerPowerups::timeStopped(players) && !worn.invulnerable() && !worn.invisible();
}

bool MechanicsFacade::generatorTakes(BlowSource source) const {
    return !m_settings.aggressorMode || source == BlowSource::Swing || source == BlowSource::Shot;
}

} // namespace gdl::game
