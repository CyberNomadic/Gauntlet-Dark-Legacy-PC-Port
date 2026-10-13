#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/core/Types.h"

#include "game/netplay/PredictedShotLedger.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/ReplicaProjectiles.h"

namespace gdl::game {
/** Replays ordinary weapon flight in a private, callback-free native missile pool.
 * Host damage and effects are never executed here. Reconciliation substitutes
 * one current local shot for its delayed visual components, including streaks. */
class LocalProjectilePrediction {
public:
    struct SoundRelease {
        ShotIdentity origin;
        u8 shots = 1;
        u32 weaponFlags = 0;
    };
    void begin(const MatchContext& context, u8 localPeer, const WorldCollision& collision,
               const WorldScene* scene = nullptr, const WorldLayout* layout = nullptr);
    void clear();
    void update(const CombatSnapshot* authority, const LocalMovementPrediction& movement,
                const ProjectileResources& resources, std::span<const InputCommand> sent = {});
    void present(CombatSnapshot& displayed, f32 blend);
    usize count() const { return m_shots.size(); }
    /** Canonical successful fan launches from this update, including immediate
     * impacts. Replay may repeat an origin; consumers deduplicate by identity. */
    std::span<const SoundRelease> soundReleases() const { return m_soundReleases; }

private:
    struct Shot {
        ProjectileState current;
        ProjectileState previous;
    };
    void remember(ProjectileState state);
    MatchContext m_context;
    u8 m_local = 0;
    u64 m_clock = 0;
    u64 m_receivedAt = 0;
    std::optional<CombatSnapshot> m_authority;
    std::array<u64, InputCommand::kSeats> m_inputTicks{};
    WorldCollision m_collision;
    std::unique_ptr<WorldScene> m_geometry;
    const WorldLayout* m_layout = nullptr;
    PredictedShotLedger m_ledger;
    std::vector<Shot> m_shots;
    std::vector<SoundRelease> m_soundReleases;
    std::vector<Shot> m_previous;
    std::map<std::pair<ProjectileSource, ShotIdentity>, usize> m_previousByOrigin;
};
} // namespace gdl::game
