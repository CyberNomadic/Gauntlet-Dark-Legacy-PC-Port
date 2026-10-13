#pragma once

#include <optional>
#include <span>
#include <vector>

#include "game/netplay/PlayerMeleeCheckpoint.h"

namespace gdl::game {
/** Frozen, ordered native contact observations for a short local movement prefix.
 * Replaying these never moves an opponent, shoves a partner or opens an item.
 * New authority replaces the entire observation, including disappearing bodies. */
struct PlayerLocomotionState {
    static constexpr u32 kMaxTicks = 12;
    static constexpr f32 kMaxRadius = 8;
    static constexpr usize kMaxBodies = 128;
    static constexpr usize kMaxGenerators = 64;
    static constexpr usize kMaxFixtures = 128;
    static constexpr usize kMaxParty = 3;
    struct Body {
        Vec3 base{0};
        f32 radius = 0;
        f32 height = 0;
        bool operator==(const Body&) const = default;
        MissileTarget target() const { return {-1, base, radius, height}; }
    };
    Vec3 origin{0};
    f32 radius = 0;
    u32 ticks = 0;
    std::vector<Body> bodies;
    std::vector<Obstacle> generators;
    std::vector<Obstacle> fixtures;
    std::vector<Body> party;
    bool automatic = false;
    /** Present only when the native acquisition roster is completely represented.
     * With automatic=true, absent geometry means a forward auto-melee query is
     * unknown, not that no enemy is present. */
    std::optional<PlayerMeleeState> automaticGeometry;
    bool valid() const;
};

class PlayerLocomotionCheckpoint {
public:
    static constexpr usize kHeaderBytes = 44;
    static constexpr usize kBodyBytes = 20;
    static constexpr usize kObstacleBytes = 40;
    static constexpr usize kMaxBytes =
        kHeaderBytes +
        (PlayerLocomotionState::kMaxBodies + PlayerLocomotionState::kMaxParty) * kBodyBytes +
        (PlayerLocomotionState::kMaxGenerators + PlayerLocomotionState::kMaxFixtures) *
            kObstacleBytes +
        PlayerMeleeCheckpoint::kMaxBytes;
    static std::optional<std::vector<u8>> encode(const PlayerLocomotionState& state);
    static std::optional<PlayerLocomotionState> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
