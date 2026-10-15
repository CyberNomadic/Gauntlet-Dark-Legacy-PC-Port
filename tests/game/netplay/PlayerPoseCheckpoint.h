#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/players/PlayerAnimator.h"

namespace gdl::game {
/** Exact transient stance-blend poses. The enclosing snapshot owns presence;
 * no bytes are needed outside a transition. The shared roster is written once. */
class PlayerPoseCheckpoint {
public:
    static constexpr usize kHeaderBytes = 8;
    static constexpr usize kNodeBytes = 100;
    static constexpr usize kMaxBytes =
        kHeaderBytes + TreePose::Checkpoint::kMaxName +
        TreePose::Checkpoint::kMaxNodes * (kNodeBytes + TreePose::Checkpoint::kMaxName);
    static std::optional<std::vector<u8>> encode(const PlayerAnimator::TransitionPose& state);
    static std::optional<PlayerAnimator::TransitionPose> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
