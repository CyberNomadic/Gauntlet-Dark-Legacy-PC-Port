#pragma once

#include "game/netplay/CombatReplica.h"
#include "game/netplay/SnapshotPlayback.h"

namespace gdl::game {

/** Local-only provenance for a sampled enemy pose. Never encoded in a packet:
 * a renderer may use trusted floor artwork to keep grounded interpolation above
 * a ramp, without changing the snapshot or acquiring gameplay authority. */
struct EnemyPresentation {
    struct Interval {
        EnemyCombatState from;
        EnemyCombatState to;
    };
    u64 epoch = 0;
    u64 tick = 0;
    f32 blend = 0;
    std::vector<Interval> enemies;
    std::optional<SceneGeometry> fromGeometry;
    std::optional<SceneGeometry> toGeometry;
    std::optional<SceneGeometry> shownGeometry;
    bool matches(const CombatSnapshot& snapshot) const;
};

/** Delayed client display, not a simulation. Motion, camera, health and actor
 * presence share one snapshot clock. Only complete host checkpoints enter the
 * history; missing updates hold the last state without inventing hits or deaths. */
class CombatPlayback {
public:
    bool begin(PacketTransport::Connection host, u64 epoch);
    void clear();
    CombatReplica::Admission receive(PacketTransport::Connection sender, std::span<const u8> bytes);
    /** Display-only: complete opponent rollback state is available exclusively
     * through latest(), never through an interpolated or clamped sample. */
    std::optional<CombatSnapshot> sample(u64 tick, f32 fraction = 0,
                                         EnemyPresentation* presentation = nullptr) const;
    const CombatSnapshot* latest() const { return m_receiver.latest(); }
    usize size() const { return m_history.size(); }

private:
    CombatReplica m_receiver;
    SnapshotPlayback m_motion;
    std::deque<CombatSnapshot> m_history;
};
} // namespace gdl::game
