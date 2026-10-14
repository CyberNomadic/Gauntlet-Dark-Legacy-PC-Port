#pragma once

#include "game/enemies/Enemies.h"
#include "game/netplay/CombatPlayback.h"

namespace gdl::game {
struct PlayerRuntime;
/** Native current-owner drawing policy, distinct from delayed packet playback.
 * Births, removal and morphs belong to the CURRENT tick at every draw fraction;
 * only retained compatible owners interpolate their continuous state. */
class StageSnapshotSampling {
public:
    struct Swarm {
        std::vector<Enemies::Presentation> enemies;
    };
    struct Player {
        ReplicaPlayerLife life = ReplicaPlayerLife::Standing;
        CombatAnimation animation;
        TreePose previous;
        TreePose current;
    };
    struct Party {
        std::array<std::optional<Player>, InputCommand::kSeats> players;
    };
    /** Read-only, current native roster; immutable archive trees must outlive
     * these anchors. The stage particle sidecar supplies their lifetime fence. */
    static Swarm captureSwarm(const Enemies& enemies);
    /** Native completed endpoint poses, including interrupted stance blends and
     * generation cuts. Like swarm anchors, these borrow immutable action trees. */
    static std::optional<Party> captureParty(std::span<const PlayerRuntime> players);
    static std::unique_ptr<CombatSnapshot> sample(const CombatSnapshot& previous,
                                                  const CombatSnapshot& current, f32 blend,
                                                  EnemyPresentation* presentation = nullptr,
                                                  const Swarm* swarm = nullptr,
                                                  const Party* party = nullptr);
};
} // namespace gdl::game
