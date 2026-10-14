#pragma once

#include "game/screens/OpponentCombatCheckpoint.h"
#include "game/screens/PartyDamage.h"

namespace gdl::game {
/** Native live great-creature phases under the caller's complete combat undo.
 * Refusal may follow CPU mutations, but never audio, resource loading or a GPU
 * call. The caller must rewind the entire tick and discard all buffered outputs. */
class OrdinaryCritterPhase {
public:
    enum class Refusal { None, State, Output, Resources, Damage };
    struct Sound {
        std::string name;
        Vec3 position{0};
        f32 volume = 0;
        bool operator==(const Sound&) const = default;
    };
    struct Result {
        Refusal refusal = Refusal::None;
        std::vector<Sound> sounds;
        std::vector<PartyDamage::Output> damage;
        bool advanced() const { return refusal == Refusal::None; }
    };
    /** Admission only, not a substitute for incoming State::valid/resource
     * preparation. Dormant slots and live single-part ordinary families are
     * supported; deaths, projectiles, grabs, arena work and drops are not. */
    static bool supported(const OpponentCombatCheckpoint::State&);
    /** After swarm/missiles and before cues/damage. Uses the PRE-critter player
     * views and native fixture classifier, then applies native physical pushes. */
    static Result advance(LevelOpponents&, s32 ticks, f32 seconds, std::span<const EnemyView>,
                          std::span<const CombatantObstacle>, std::span<PlayerRuntime>);
    /** After the empty boss/projectile phase, before native level observation
     * and swarm blows. Uses exact native cue attachment and damage-gap routing.
     * Blocks are the native selective breath/area cover queries, not all walls. */
    static Result finish(LevelOpponents&, std::span<PlayerRuntime>, PlayerHealth&,
                         const ClassDataSet&, const PartyDamage::Context&,
                         std::span<const EffectTrees::PoolResource>,
                         const std::function<bool(const Vec3&, const Vec3&)>& blocksBreath = {},
                         const std::function<bool(const Vec3&, const Vec3&)>& blocksArea = {});

private:
    static bool outputs(const Critters&);
};
} // namespace gdl::game
