#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/combat/MeleeGeometry.h"
#include "game/players/MeleeSelection.h"
#include "game/world/ItemFigure.h"

namespace gdl::game {
/** Host-filtered acquisition at fixed feet. Replaying a swing never grants hit
 * authority. The entire context expires/replaces with its enclosing host tick. */
struct PlayerMeleeState {
    static constexpr usize kMaxTargets = 64;
    static constexpr usize kMaxObstacles = 64;
    static constexpr u32 kMaxTicks = 12;
    static constexpr f32 kMaxRadius = 8;
    struct Moving {
        Vec3 origin{0};
        f32 radius = 0;
        u32 ticks = 0;
        std::vector<MeleeGeometry::Target> targets;
        std::vector<Obstacle> obstacles;
    };
    MeleeSelection::Snapshot selection;
    f32 captureYaw = 0;
    /** Native stationary auto-facing result, verified stable after that turn.
     * Cursor aiming bypasses it; other headings require a new host context. */
    std::optional<Vec3> assistedAim;
    /** Re-queryable geometry within a bounded host-certified movement region.
     * No obstacle or target changes are committed by local replay. */
    std::optional<Moving> moving;
    bool valid() const;
};

/** Bounded explicit encoding of stationary and optional moving melee context. */
class PlayerMeleeCheckpoint {
public:
    static constexpr usize kTargetBytes = 36;
    static constexpr usize kHeaderBytes = 88;
    static constexpr usize kMovingHeaderBytes = 28;
    static constexpr usize kGeometryBytes = 64;
    static constexpr usize kObstacleBytes = 40;
    static constexpr usize kMaxBytes = kHeaderBytes + PlayerMeleeState::kMaxTargets * kTargetBytes +
                                       kMovingHeaderBytes +
                                       PlayerMeleeState::kMaxTargets * kGeometryBytes +
                                       PlayerMeleeState::kMaxObstacles * kObstacleBytes;
    static std::optional<std::vector<u8>> encode(const PlayerMeleeState& state);
    static std::optional<PlayerMeleeState> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
