#pragma once

#include <array>
#include <optional>
#include <span>

#include "game/world/WorldMotion.h"

namespace gdl::game {
/** Exact last native floor contact. Its placement can precede the scene's
 * post-motion transform; deriving it from current geometry loses that carry. */
struct PlayerFloorState {
    s32 object = -1;
    u32 flags = 0;
    std::optional<Mat4> placement;
    bool valid() const;
};

/** Wire-only envelope. Clocks refer to trusted, locally bound world tracks;
 * floor contacts stay available even when world replay is not certified. */
struct PlatformState {
    static constexpr u32 kMaxTicks = 12;
    u32 ticks = 0;
    std::array<std::optional<PlayerFloorState>, 4> floors;
    std::optional<WorldMotion::State> motion;
    bool valid() const;
};

class PlatformCheckpoint {
public:
    static constexpr usize kHeaderBytes = 40;
    static constexpr usize kFloorBytes = 60;
    static constexpr usize kCursorBytes = 16;
    static constexpr usize kMaxBytes =
        kHeaderBytes + 4 * kFloorBytes + WorldMotion::kMaxObjects +
        kCursorBytes * (WorldMotion::kMaxAnimations + LevelTriggers::Motion::kMaxTargets +
                        Rotators::kMaxCursors);
    static std::optional<std::vector<u8>> encode(const PlatformState& state);
    static std::optional<PlatformState> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
