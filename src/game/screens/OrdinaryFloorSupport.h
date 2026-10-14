#pragma once

#include "engine/world/WorldCollision.h"

#include "game/screens/PlayerRuntime.h"

namespace gdl::game {

/** Grounded-support admission for the complete ordinary combat transaction.
 * This does not move the live party or advance the world. The caller still
 * runs native PartyMotion, including ordered party contact, carry and land.
 * Moving support is allowed only with the jointly restored stage and party. */
class OrdinaryFloorSupport {
public:
    enum class Phase {
        /** Before PartyMotion, or after the later trigger/rotator phase. A
         * rider's recorded floor placement may lag the current world. */
        BeforeCarry,
        /** Immediately after PartyMotion/fixture contact, before later world
         * motion. Moving links must agree; a post-fixture static seam may
         * retain the preceding static floor until native landing runs again. */
        AfterLanding
    };

    /** BeforeCarry checks native carry on a temporary actor and floor only;
     * no figure, inventory, resource owner or event callback is copied. The
     * resulting support must remain grounded and above native rescue depth.
     * allowMoving must be false without a complete, trusted stage binding.
     * Kept-apart and floor-exit locks are native landing rules, not reasons
     * to reject a valid rider when the complete party/stage is restored.
     * The enclosing checkpoint validates every claimed collision owner against
     * its trusted catalog, including historical static support links. */
    static bool accepts(const PlayerRuntime& player, const WorldCollision& collision, Phase phase,
                        bool allowMoving);
};

} // namespace gdl::game
