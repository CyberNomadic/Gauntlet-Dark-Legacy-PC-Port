#pragma once

#include "game/netplay/MatchSession.h"

namespace gdl::game {
/** Client presentation clock, separate from fixed-step input and host simulation.
 * Advance once per rendered frame with real elapsed time. A short snapshot buffer
 * permits fractional poses; missing state holds at the latest complete checkpoint.
 * Never extrapolates actors, changes authoritative input ticks or runs gameplay. */
class ClientClock {
public:
    static constexpr u64 kDelay = 3; // one 20 Hz checkpoint: 50 ms, not an extra 100 ms
    static constexpr u64 kCatchup =
        6; // rejoin after a gap, rather than keeping stale input visible
    bool begin(const MatchSession& match);
    void clear();
    std::optional<CombatSnapshot> sample(const MatchSession& match, f64 seconds,
                                         EnemyPresentation* presentation = nullptr);
    u64 tick() const { return m_tick; }
    f64 fraction() const { return m_fraction; }

private:
    MatchContext m_context;
    u64 m_tick = 0;
    f64 m_fraction = 0;
    bool m_started = false;
};
} // namespace gdl::game
