#pragma once

#include <optional>
#include <span>
#include <vector>

#include "game/enemies/Enemies.h"

namespace gdl::game {

/** Complete native swarm state, explicitly little-endian. Decoding establishes
 * bounded value syntax only; Enemies::prepareRestore resolves the named roster
 * against already-loaded trusted resources before any state becomes live.
 * Transport limits intentionally reject some larger locally valid checkpoints;
 * no queue, body, pose or contact is silently omitted to make a packet fit. */
class EnemyPoolCheckpoint {
public:
    static constexpr u32 kVersion = 2;
    static constexpr usize kMaxBytes = 512 * 1024;
    static constexpr usize kMaxNodes = 128;
    static constexpr usize kMaxTotalNodes = 2048;
    static constexpr usize kMaxEvents = 256;
    static constexpr usize kMaxTotalEvents = 1024;
    static constexpr usize kMaxCombatants = 256;

    static std::optional<std::vector<u8>> encode(const Enemies::Checkpoint& state);
    static std::optional<Enemies::Checkpoint> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
