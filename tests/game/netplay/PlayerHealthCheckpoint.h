#pragma once

#include "game/netplay/RandomCheckpoint.h"
#include "game/screens/PlayerHealth.h"

namespace gdl::game {
/** The shared native pain/low-health random stream. Health and reactions themselves
 * belong to the complete participant records, not this party-wide owner. */
class PlayerHealthCheckpoint {
public:
    static constexpr usize kBytes = RandomCheckpoint::kBytes;
    static std::optional<std::vector<u8>> encode(const PlayerHealth::Checkpoint& state);
    static std::optional<PlayerHealth::Checkpoint> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
