#pragma once

#include <optional>
#include <span>
#include <vector>

#include "game/enemies/OpponentState.h"
#include "game/netplay/EnemyMissilesCheckpoint.h"
#include "game/netplay/EnemyPoolCheckpoint.h"
#include "game/netplay/GeneratorCheckpoint.h"

namespace gdl::game {

/** Complete, bounded value encoding for a single opponent correction. This is
 * not a standalone network message: the match envelope must authenticate the
 * sender and bind it to an epoch, simulation tick and trusted resource roster. */
class OpponentCheckpoint {
public:
    static constexpr usize kHeaderBytes = 20;
    static constexpr usize kMaxBytes = kHeaderBytes + GeneratorCheckpoint::kMaxBytes +
                                       EnemyPoolCheckpoint::kMaxBytes +
                                       EnemyMissilesCheckpoint::kMaxBytes;
    static std::optional<std::vector<u8>> encode(const OpponentState& state);
    static std::optional<OpponentState> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
