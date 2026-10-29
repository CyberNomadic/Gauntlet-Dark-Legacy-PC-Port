#pragma once

#include <span>

#include "engine/core/Types.h"

namespace gdl::game {

struct PlayerRuntime;

/** What landed a blow, as the base game knows it. */
enum class BlowSource : u8 {
    Swing,     ///< a melee swing
    Shot,      ///< a thrown or fired weapon
    SuperShot, ///< the super shot
    Potion,    ///< a potion's burst, cast or thrown, or its shield's ring
    Turbo,     ///< a turbo move's strike, the thunder hammer among them
    Charge,    ///< a charge or combo body blow
    Breath,    ///< a breath worn as a powerup
    Shield,    ///< a fire or lightning shield's touch
};

/** What a blow did to what it landed on. */
enum class BlowOutcome : u8 {
    Hurt,      ///< it stands, or was not standing to take it
    Killed,    ///< the blow felled it
    Destroyed, ///< a generator, to rubble
};

/** What a blow does for the striker's run of close blows (`MeleeStreak`). */
enum class RunEffect : u8 {
    Nothing,
    Count,           ///< counts one
    CountAsOriginal, ///< counts as the original's swing did: one, or three when telling
};

/**
 * The one place the base game asks about the mechanics laid over it. Each hook puts a named
 * question here instead of knowing any mechanic, so another additive mechanic answers the
 * same questions without another hook. Settings only, never simulation state: a mechanic's
 * own state lives with the system that steps it.
 *
 * Aggressor Mode: the style meter over a run of close blows, counted in kills and generators
 * destroyed alone, none of them under time stop, invulnerability or invisibility, the run
 * surviving less quiet the higher its grade; the swarm aggravated by the run; and the
 * generators harmed by swings and shots alone.
 */
class MechanicsFacade {
public:
    struct Settings {
        bool aggressorMode = false;
        bool operator==(const Settings&) const = default;
    };

    MechanicsFacade() = default;
    explicit MechanicsFacade(const Settings& settings) : m_settings(settings) {}

    /** The answers of the standard settings (the mode off), for a hook handed no facade. */
    static const MechanicsFacade& standard();

    void configure(const Settings& settings) { m_settings = settings; }
    const Settings& settings() const { return m_settings; }
    bool aggressorMode() const { return m_settings.aggressorMode; }

    /** What a blow of `source` with `outcome`, landed by `striker`, does for their run. In
     * Aggressor Mode a kill or a generator destroyed counts one and nothing else does, none
     * of it under time stop, invulnerability or invisibility; out of the mode the original's
     * rule stands, swings alone, as the original counted them. */
    RunEffect runEffect(BlowSource source, BlowOutcome outcome, const PlayerRuntime& striker,
                        std::span<const PlayerRuntime> players) const;
    /** The quiet a run of `count` survives, in seconds: in Aggressor Mode a step shorter each
     * grade, three seconds at D down to one at S; out of it the original's two. */
    f32 quietWindow(s32 count) const;
    /** Whether a blow of `source` harms a generator at all: in Aggressor Mode swings and
     * shots alone. */
    bool generatorTakes(BlowSource source) const;
    /** Whether the swarm answers the party's run of close blows. */
    bool swarmAggravated() const { return m_settings.aggressorMode; }
    /** Whether the run's meter is drawn. */
    bool meterShown() const { return m_settings.aggressorMode; }

private:
    Settings m_settings;
};

} // namespace gdl::game
