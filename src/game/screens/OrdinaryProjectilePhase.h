#pragma once

#include "game/screens/CombatRollbackCheckpoint.h"

namespace gdl::game {

/** The native player-projectile phase inside an already prepared ordinary-combat
 * transaction. This is not a world checkpoint: the caller must undo the complete
 * tick when advance refuses, including the missile queue it has consumed.
 * Accepted swarm hits retain their native loss/feedback queues for the NEXT
 * opponent phase. No reward, sound, lesson, or external callback is dispatched. */
class OrdinaryProjectilePhase {
public:
    static constexpr usize kMaxFlightSteps = 256;
    static constexpr usize kMaxTotalSteps = 4096;
    static constexpr usize kMaxImpacts = 4096;

    struct Result {
        bool advanced = false;
        usize impacts = 0;
    };

    /** Pure admission/work check against complete native state and a trusted
     * immutable resource manifest. Plain normal-multiplayer class weapons only;
     * potion, piercing, reflected, and special-weapon gameplay remain excluded.
     * Retained visual banks and particle tails are not silently discarded. */
    static bool supported(const PlayerArsenalCheckpoint::State& state,
                          std::span<const PlayerMissiles::Resource> resources, f32 seconds);

    /** Uses the bound arsenal and the exact native target, collision, and impact
     * ordering. All queued and new impacts are checked BEFORE the first impact
     * is dispatched. A refusal after flight changes only owners covered by the
     * caller's prepared OrdinaryCombatCheckpoint undo. The caller also verifies
     * the resulting complete state, including newly queued lessons/rewards. */
    static Result advance(PlayerAttacks& attacks, std::span<PlayerRuntime> players,
                          const PlayerAttacks::Targets& targets, f32 seconds,
                          const CombatRollbackCheckpoint::Resources& resources);
};

} // namespace gdl::game
