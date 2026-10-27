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
    Potion,    ///< a potion's burst, cast or thrown
    Turbo,     ///< a turbo move's strike, the thunder hammer among them
    Charge,    ///< a charge or combo body blow
    Breath,    ///< a breath worn as a powerup
    Shield,    ///< a fire or lightning shield's touch
};

/**
 * The one place the base game asks about the mechanics laid over it. Each hook puts a named
 * question here instead of knowing any mechanic, so another additive mechanic answers the
 * same questions without another hook. Settings only, never simulation state: a mechanic's
 * own state lives with the system that steps it.
 *
 * Aggressor Mode: the style meter over a run of close blows, every landed blow building it
 * but those struck under time stop, invulnerability or invisibility; the swarm aggravated by
 * the run; and the generators harmed by swings and shots alone.
 */
class MechanicsFacade {
public:
    struct Settings {
        bool aggressorMode = true;
        bool operator==(const Settings&) const = default;
    };

    MechanicsFacade() = default;
    explicit MechanicsFacade(const Settings& settings) : m_settings(settings) {}

    /** The answers of the standard settings, for a hook handed no facade. */
    static const MechanicsFacade& standard();

    void configure(const Settings& settings) { m_settings = settings; }
    const Settings& settings() const { return m_settings; }
    bool aggressorMode() const { return m_settings.aggressorMode; }

    /** Whether a blow of `source` landed by `striker` builds their run of close blows: in
     * Aggressor Mode any blow, unless struck under time stop, invulnerability or
     * invisibility; out of it the original's rule stands, swings alone. */
    bool blowBuildsRun(BlowSource source, const PlayerRuntime& striker,
                       std::span<const PlayerRuntime> players) const;
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
