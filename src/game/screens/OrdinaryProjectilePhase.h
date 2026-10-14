#pragma once

#include "game/screens/CombatRollbackCheckpoint.h"

namespace gdl::game {

/** The native player-projectile phase inside an already prepared ordinary-combat
 * transaction. This is not a world checkpoint: the caller must undo the complete
 * tick when advance refuses, including the missile queue it has consumed.
 * Accepted swarm hits retain their native loss/feedback queues for the NEXT
 * opponent phase. Native world-hit effects use trusted prebound resources; sound
 * requests are buffered. No reward, sound, lesson, or external callback is dispatched. */
class OrdinaryProjectilePhase {
public:
    static constexpr usize kMaxFlightSteps = 256;
    static constexpr usize kMaxTotalSteps = 4096;
    static constexpr usize kMaxImpacts = 4096;
    using Cue = PlayerArsenal::ImpactCue;

    struct Result {
        bool advanced = false;
        usize impacts = 0;
        std::vector<Cue> cues;
    };

    /** Pure admission/work check against complete native state and a trusted
     * immutable resource manifest. Plain normal-multiplayer class weapons only;
     * potion, piercing, reflected, and special-weapon gameplay remain excluded.
     * Retained visual banks and particle tails are not silently discarded. */
    static bool supported(const PlayerArsenalCheckpoint::State& state,
                          std::span<const PlayerMissiles::Resource> resources, f32 seconds);
    /** Trusted native ordinary weapon-impact artwork may remain after its shot
     * retires. This does not admit potion, elemental, or world-explosion effects. */
    static bool supportsEffect(const PlayerArsenal& arsenal,
                               const EffectTrees::PoolResource& resource);

    /** Uses the bound arsenal and the exact native target, collision, and impact
     * ordering. All queued and new impacts are checked BEFORE the first impact
     * is dispatched. Surviving shot endpoints use the same scenery contact
     * predicate as native play; distant or already-started scenery is harmless.
     * A refusal after flight changes only owners covered by the
     * caller's prepared OrdinaryCombatCheckpoint undo. The caller also verifies
     * the resulting complete state, including newly queued lessons/rewards. */
    static Result advance(PlayerAttacks& attacks, std::span<PlayerRuntime> players,
                          const PlayerAttacks::Targets& targets, f32 seconds,
                          const CombatRollbackCheckpoint::Resources& resources);

private:
    static bool ordinaryTarget(s32 target);
};

} // namespace gdl::game
