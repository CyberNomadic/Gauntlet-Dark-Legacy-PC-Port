#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"

namespace gdl::game {

/** Complete outside observations accompanying an end-of-tick opponent pool.
 * They share the enclosing motion tick, not the interpolated display clock.
 * Fixtures and player conditions are observations, not authority to predict
 * their future mutations. The pool checkpoints retain the host's watch volume. */
struct OpponentFrame {
    static constexpr usize kMaxPlayers = 4;
    static constexpr usize kMaxFixtures = 512;
    static constexpr usize kMaxStops = 512;
    static constexpr u32 kMaxPendingPlacements = 65535;

    struct Player {
        u8 seat = 0;
        EnemyView view;
        bool operator==(const Player&) const;
    };

    s32 ticks = 1;
    f32 seconds = 1.0f / 60;
    f32 missileSpeed = 1;
    /** Load-time values, never the current mutable options or living roster. */
    f32 difficultyGain = 1;
    u32 initialPlayers = 1;
    u32 pendingPlacements = 0;
    bool held = false;
    bool timeStopped = false;
    /** Includes dormant combatants/statues and pending outside-pool gameplay
     * (carried loot, combatant/player missiles, damaging areas, or enemy shrinking). */
    bool unsupportedCombatants = false;
    std::vector<Player> players;
    std::vector<Obstacle> fixtures;
    std::vector<MissileStop> stops;

    bool valid() const;
    bool operator==(const OpponentFrame&) const;
};

/** Explicit little-endian values, with bounded counts before allocation and
 * exact consumption. Overflow refuses the whole context, never a partial list. */
class OpponentFrameCodec {
public:
    static constexpr usize kHeaderBytes = 48;
    static constexpr usize kPlayerBytes = 64;
    static constexpr usize kObstacleBytes = 40;
    static constexpr usize kStopBytes = kObstacleBytes + 12;
    static constexpr usize kMaxBytes = kHeaderBytes + OpponentFrame::kMaxPlayers * kPlayerBytes +
                                       OpponentFrame::kMaxFixtures * kObstacleBytes +
                                       OpponentFrame::kMaxStops * kStopBytes;

    static std::optional<std::vector<u8>> encode(const OpponentFrame& frame);
    static std::optional<OpponentFrame> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
