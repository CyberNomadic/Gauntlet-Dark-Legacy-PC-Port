#pragma once

#include <memory>

#include "game/netplay/CombatReplica.h"
#include "game/netplay/SnapshotPlayback.h"

namespace gdl::game {
class StageParticlePresentation;

/** Local-only provenance for a sampled enemy pose. Never encoded in a packet:
 * a renderer may use trusted floor artwork to keep grounded interpolation above
 * a ramp, without changing the snapshot or acquiring gameplay authority. */
struct EnemyPresentation {
    struct PlayerPose {
        u32 grant = 0;
        u32 continuity = 0;
        ReplicaPlayerLife life = ReplicaPlayerLife::Standing;
        CombatAnimation animation;
        TreePose::Checkpoint pose;
    };
    struct Interval {
        EnemyCombatState from;
        EnemyCombatState to;
        /** Native swarm steps at 30 Hz even when the party steps at 60 Hz. */
        std::optional<f32> nativeBlend;
        /** Exact native hierarchy pose, rebound to the replica's trusted tree. */
        std::optional<TreePose::Checkpoint> nativePose;
        std::optional<f32> nativeTextureFrame;
    };
    u64 epoch = 0;
    u64 tick = 0;
    f32 blend = 0;
    std::vector<Interval> enemies;
    std::optional<SceneGeometry> fromGeometry;
    std::optional<SceneGeometry> toGeometry;
    std::optional<SceneGeometry> shownGeometry;
    /** Local native particle picture; never serialized or advanced by playback. */
    std::shared_ptr<const StageParticlePresentation> particles;
    u64 particleTick = 0;
    f32 particleBlend = 0;
    /** Native player endpoint interpolation; never a received simulation seed. */
    std::array<std::optional<PlayerPose>, InputCommand::kSeats> players;
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
    /** Shared display interpolation for complete private/host cohorts. Neither
     * result nor provenance carries a simulation seed. Heap ownership avoids
     * stacking several large snapshots during native replay presentation. */
    static std::unique_ptr<CombatSnapshot> samplePair(const CombatSnapshot& lower,
                                                      const CombatSnapshot& upper, f32 blend,
                                                      EnemyPresentation* presentation = nullptr);
    const CombatSnapshot* latest() const { return m_receiver.latest(); }
    usize size() const { return m_history.size(); }

private:
    CombatReplica m_receiver;
    SnapshotPlayback m_motion;
    std::deque<CombatSnapshot> m_history;
};
} // namespace gdl::game
