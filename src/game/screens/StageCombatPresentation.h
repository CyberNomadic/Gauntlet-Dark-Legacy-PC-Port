#pragma once

#include <memory>

#include "game/netplay/CombatPlayback.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/ReplicaFighters.h"
#include "game/screens/ReplicaFixtures.h"
#include "game/screens/ReplicaPickups.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/screens/StageSnapshotSampling.h"

namespace gdl::game {
/** Drawing boundary for the supported private stage visual cohorts. This does not admit
 * gameplay, restore owners, infer release identities, or publish sound/HUD events.
 * All sources must belong to the same accepted post-step simulation boundary. */
class StageCombatPresentation {
public:
    struct Sources {
        std::span<const PlayerRuntime> party;
        const LevelWorld& world;
        const Enemies& enemies;
        const PlayerMissiles& playerMissiles;
        const EnemyMissiles& enemyMissiles;
        const EffectTrees& effects;
        FixtureCapture::Sources fixtures;
        const Critters& critters;
        const Bosses& bosses;
    };
    struct Resources {
        const ProjectileResources& projectiles;
        const PickupResources& pickups;
        const FixtureResources& fixtures;
        const FighterResources& fighters;
    };

    /** Registries are preloaded against these private owners, not the replica's
     * different archive/model pointers. Missing visible resources refuse the
     * entire picture. The caller supplies every native fixture owner, including
     * portals when present; absent coverage never borrows delayed portal artwork.
     * Arrival/tower presentations and live unrepresented effect/pickup particles
     * refuse this boundary. A native particle sidecar covers those particle pools
     * only when it belongs to this exact epoch and post-step tick.
     * The result is heap-owned to keep large native supplements off the stack. */
    static std::unique_ptr<CombatSnapshot>
    capture(const CombatSnapshot& authority, const MotionSnapshot& motion, const Sources& sources,
            const Resources& resources, const StageParticlePresentation* particles = nullptr);

    /** Atomically replace the complete visual cohort at one shared fraction.
     * Both pictures must come from capture() on consecutive accepted ticks.
     * Confirmed HUD/music/health remain unchanged. Success owns all projectile
     * namespaces in the pictures: independent local visual grafts must stand down.
     * Failure preserves both output arguments, including their provenance. */
    static bool present(CombatSnapshot& shown, EnemyPresentation& presentation,
                        const CombatSnapshot& previous, const CombatSnapshot& current, f32 blend,
                        const StageSnapshotSampling::Swarm* swarm = nullptr,
                        const StageSnapshotSampling::Party* party = nullptr);
};
} // namespace gdl::game
