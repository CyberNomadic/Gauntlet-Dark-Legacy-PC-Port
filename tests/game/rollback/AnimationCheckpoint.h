#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"

namespace gdl::game {
/** Pointer-free native animation clock. Sequence ownership is checked against
 * the trusted asset by AnimationPlayer::restore, not by this wire codec. */
class AnimationCheckpoint {
public:
    static constexpr usize kBytes = 44;
    static std::optional<std::vector<u8>> encode(const AnimationPlayer::Checkpoint& state);
    static std::optional<AnimationPlayer::Checkpoint> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
