#pragma once

#include <optional>

#include "game/screens/CombatRollbackCheckpoint.h"

namespace gdl::game {

/** A native normal or strong throw inside a caller-owned combat transaction. Physics,
 * artwork and release-cue selection are shared with PlayerArsenal; this adapter
 * supplies only the restricted admission policy and prebound effect resources.
 * It never spends inventory, plays sound, or publishes damage/impact events. */
class OrdinaryWeaponEmission {
public:
    struct Cue {
        s32 owner = -1;
        MissileRelease release;
        PlayerArsenal::WeaponCue sound;
        bool operator==(const Cue&) const = default;
    };
    struct Result {
        bool launched = false;
        std::optional<Cue> cue;
    };

    /** Call only at a native normal/strong release latch, after PartyMotion has
     * updated the figure. Resources are immutable authenticated local bindings.
     * Every fan member and its muzzle are checked before starting any missile.
     * A later pool refusal can still leave private flights/effects: the caller
     * MUST rewind the enclosing combat transaction, and publish no outputs.
     * A successful result contains one cue for the entire fan, not one per shot.
     * Effects/models borrow the catalog until the pool is cleared. */
    static Result advance(PlayerArsenal& arsenal, const PlayerActor& actor, PlayerFigure& figure,
                          const Vec3& direction, std::optional<Vec3> target,
                          const CombatRollbackCheckpoint::Resources& resources);
};

} // namespace gdl::game
