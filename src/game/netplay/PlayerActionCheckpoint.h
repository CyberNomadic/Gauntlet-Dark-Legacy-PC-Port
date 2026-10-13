#pragma once

#include <optional>
#include <span>
#include <vector>

#include "game/players/PlayerAnimator.h"

namespace gdl::game {

/** Explicit, pointer-free native action state. These bytes are simulation clocks
 * and latches, not a request to play effects or apply damage on receipt. */
class PlayerActionCheckpoint {
public:
    static constexpr usize kBytes = 124;
    static std::optional<std::vector<u8>> encode(const PlayerAnimator::Checkpoint& state);
    static std::optional<PlayerAnimator::Checkpoint> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
