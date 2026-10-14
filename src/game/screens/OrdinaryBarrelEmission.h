#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "game/world/PlayerArsenal.h"

namespace gdl::game {

class LevelFixtures;
class LevelWorldCheckpoint;
struct BreakableStrike;
struct PlayerRuntime;

/** The post-strike part of a native barrel interaction, inside the caller's
 * complete Stage transaction. Breakables has already applied damage, selected
 * contents using its own RNG and started the breaking animation. This helper
 * never repeats that work or commits a nested restore. */
class OrdinaryBarrelEmission {
public:
    enum class Refusal { None, Binding, Kind, Record, Birth, Lesson };
    struct Result {
        bool accepted = false;
        Refusal refusal = Refusal::None;
        std::optional<PlayerArsenal::ImpactCue> cue;
        std::optional<usize> pickup; ///< actual appended native pickup index
    };

    /** Plain and Holding outcomes use the exact native contents/help dispatch.
     * Available powerup records are born from the persistent world's trusted
     * pickup catalog, without loading/uploading art. A new help message, Death,
     * gas, explosion or an unavailable birth refuses the whole caller's tick.
     * The sound is buffered, never played. Native collection happens later and
     * requires its own admission: accepting a birth does not permit collecting
     * it, nor does this helper add a delay to avoid the collection phase.
     *
     * On refusal, local owners may already have changed. All returned outputs
     * are discarded and the caller must rewind its complete Stage interval. */
    static Result advance(const BreakableStrike& strike, std::string_view sound, s32 byPlayer,
                          std::span<PlayerRuntime> players, LevelFixtures& fixtures,
                          LevelWorldCheckpoint& world);
};

} // namespace gdl::game
