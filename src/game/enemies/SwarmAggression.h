#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/players/MeleeStreak.h"

namespace gdl::game {

/**
 * The swarm's answer to a run of close blows: as the longest run under way grows, the
 * generators breed sooner and keep more out, and the swarm walks and swings quicker and is
 * born tougher; held at the bravery line, a rage sets in that darkens the swarm to red and
 * puts an aura about it. The heat climbs toward the run and cools once it is over, so
 * nothing jumps; the rage comes slowly and goes faster than it came. Native state.
 */
struct SwarmAggression {
    static constexpr s32 kFullRun = MeleeStreak::kBravery; ///< the run the heat is full at
    static constexpr f32 kRise = 0.5f;         ///< heat a second toward a longer run
    static constexpr f32 kCool = 0.2f;         ///< heat a second once the run is over
    static constexpr f32 kRageSeconds = 20.0f; ///< at the bravery line, seconds to full rage
    static constexpr f32 kCalmSeconds = 8.0f;  ///< below it, seconds from full rage to none
    /** What full heat and full rage add to each measure, as shares of the plain one. */
    static constexpr f32 kBreedingHeat = 0.6f;
    static constexpr f32 kBreedingRage = 0.2f;
    static constexpr f32 kBroodHeat = 0.5f;
    static constexpr f32 kBroodRage = 0.25f;
    static constexpr f32 kSpeedHeat = 0.2f;
    static constexpr f32 kSpeedRage = 0.1f;
    static constexpr f32 kAttackHeat = 0.25f;
    static constexpr f32 kAttackRage = 0.15f;
    static constexpr f32 kHealthHeat = 0.4f;
    static constexpr f32 kHealthRage = 0.3f;
    static constexpr Color kRageTint = Color::rgba(165, 30, 30); ///< the swarm at full rage

    f32 heat = 0.0f; ///< how far the longest run has come toward the bravery line, eased
    f32 rage = 0.0f; ///< how long it has been held there, 0 to 1

    bool operator==(const SwarmAggression&) const = default;
    template <class Digest> friend void visitNativeState(Digest& digest, const SwarmAggression& value) {
        digest.template fields<2>(value);
    }

    bool valid() const;
    /** Moves on by `seconds` toward `run`, the longest run of close blows under way (0 for
     * none). */
    void step(f32 seconds, s32 run);

    /** How much sooner a generator breeds: its countdown is divided by this. */
    f32 breedingPace() const { return 1.0f + kBreedingHeat * heat + kBreedingRage * rage; }
    /** How many more a generator keeps out: its quota is multiplied by this. */
    f32 broodScale() const { return 1.0f + kBroodHeat * heat + kBroodRage * rage; }
    /** How much quicker the swarm walks. */
    f32 speedScale() const { return 1.0f + kSpeedHeat * heat + kSpeedRage * rage; }
    /** How much quicker it swings. */
    f32 attackPace() const { return 1.0f + kAttackHeat * heat + kAttackRage * rage; }
    /** How much tougher it is born. */
    f32 healthScale() const { return 1.0f + kHealthHeat * heat + kHealthRage * rage; }
    /** The swarm's colour, white through to `kRageTint` with the rage. */
    Color tint() const;
    /** How strong the aura about the swarm is, 0 to 1. */
    f32 aura() const { return rage; }
};

} // namespace gdl::game
