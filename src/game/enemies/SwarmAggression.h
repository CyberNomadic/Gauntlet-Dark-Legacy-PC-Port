#pragma once

#include <algorithm>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/players/MeleeStreak.h"

namespace gdl::game {

/**
 * The swarm's answer to a run of close blows: as the longest run under way grows, the
 * generators breed sooner and keep more out, and the swarm walks and swings quicker and is
 * born tougher, with no end to it: the longer the run, the more. Held at the bravery line,
 * a rage sets in and grows for as long as the run holds: it darkens the swarm to red, puts
 * an aura about it and frenzies the generators near the party, which spew births into the
 * whole pool with no regard to any quota or the level's cap. The heat climbs toward the
 * run so nothing jumps, the rage comes slowly, and the moment the run is over all of it
 * goes: the swarm is back to its own measures at once. The walking, the swinging and the
 * breeding are capped; the health and the brood are not. Native state.
 */
struct SwarmAggression {
    static constexpr s32 kFullRun = MeleeStreak::kBravery; ///< the run the heat is full at
    static constexpr f32 kRise = 0.5f;         ///< heat a second toward a longer run
    static constexpr f32 kCool = 0.2f;         ///< heat a second toward a shorter run
    static constexpr f32 kRageSeconds = 20.0f; ///< at the bravery line, seconds to full rage
    static constexpr f32 kFrenzyReach = 60.0f; ///< how near the party a frenzied hut stands
    static constexpr s32 kFrenzyTicks = 3;     ///< ticks between a frenzied hut's births
    static constexpr f32 kMostMeasure = 1.0e6f; ///< past this a heat or rage is corrupt
    /** What full heat and full rage add to each measure, as shares of the plain one. */
    static constexpr f32 kBreedingHeat = 1.2f;
    static constexpr f32 kBreedingRage = 0.4f;
    static constexpr f32 kBroodHeat = 1.0f;
    static constexpr f32 kBroodRage = 0.5f;
    static constexpr f32 kSpeedHeat = 0.4f;
    static constexpr f32 kSpeedRage = 0.2f;
    static constexpr f32 kAttackHeat = 0.5f;
    static constexpr f32 kAttackRage = 0.3f;
    static constexpr f32 kHealthHeat = 0.8f;
    static constexpr f32 kHealthRage = 0.6f;
    /** The most the paces reach, however long the run. */
    static constexpr f32 kBreedingCap = 2.0f;
    static constexpr f32 kSpeedCap = 1.4f;
    static constexpr f32 kAttackCap = 1.5f;
    static constexpr Color kRageTint = Color::rgba(165, 30, 30); ///< the swarm at full rage

    f32 heat = 0.0f; ///< the longest run as a share of the bravery line, eased, past 1 beyond it
    f32 rage = 0.0f; ///< how long it has been held there, in `kRageSeconds`; 0 the moment it is not

    bool operator==(const SwarmAggression&) const = default;
    template <class Digest> friend void visitNativeState(Digest& digest, const SwarmAggression& value) {
        digest.template fields<2>(value);
    }

    bool valid() const;
    /** Moves on by `seconds` toward `run`, the longest run of close blows under way (0 for
     * none). */
    void step(f32 seconds, s32 run);

    /** How much sooner a generator breeds: its countdown is divided by this. */
    f32 breedingPace() const {
        return std::min(kBreedingCap, 1.0f + kBreedingHeat * heat + kBreedingRage * rage);
    }
    /** How many more a generator keeps out: its quota is multiplied by this. */
    f32 broodScale() const { return 1.0f + kBroodHeat * heat + kBroodRage * rage; }
    /** How much quicker the swarm walks. */
    f32 speedScale() const {
        return std::min(kSpeedCap, 1.0f + kSpeedHeat * heat + kSpeedRage * rage);
    }
    /** How much quicker it swings. */
    f32 attackPace() const {
        return std::min(kAttackCap, 1.0f + kAttackHeat * heat + kAttackRage * rage);
    }
    /** How much tougher it is born. */
    f32 healthScale() const { return 1.0f + kHealthHeat * heat + kHealthRage * rage; }
    /** The swarm's colour, white through to `kRageTint` with the rage. */
    Color tint() const;
    /** How strong the aura about the swarm is, 0 to 1. */
    f32 aura() const { return std::min(rage, 1.0f); }
    /** Whether the generators within `kFrenzyReach` of a player breed past any quota: from
     * the moment the run holds at the bravery line until the rage has gone. */
    bool frenzied() const { return rage > 0.0f; }
};

} // namespace gdl::game
