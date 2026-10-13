#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/players/TurboMeter.h"
#include "game/players/TurboMove.h"

namespace gdl::game {

/** Value-only turbo clocks and timeline. Class rows remain trusted local data. */
struct PlayerTurboState {
    TurboMeter::Checkpoint meter;
    TurboMove::Checkpoint move;
    bool valid() const;
    bool operator==(const PlayerTurboState&) const = default;
};

/** Bounded little-endian turbo state, independent of pointers and object layout. */
class PlayerTurboCheckpoint {
public:
    static constexpr usize kHeaderBytes = 36;
    static constexpr usize kMaxBytes = kHeaderBytes + usize{3} * 4 * TurboMove::kMaxCheckpointRows;
    static std::optional<std::vector<u8>> encode(const PlayerTurboState& state);
    static std::optional<PlayerTurboState> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
