#pragma once

#include <optional>
#include <span>
#include <vector>

#include "game/players/Knockback.h"

namespace gdl::game {

/** Native impulse accumulators and decay phase, not a request to deal damage
 * or emit hit effects. Explicit little-endian fields avoid object-layout bytes. */
class PlayerKnockbackCheckpoint {
public:
    static constexpr usize kBytes = 56;
    static std::optional<std::vector<u8>> encode(const Knockback::Checkpoint& state);
    static std::optional<Knockback::Checkpoint> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
