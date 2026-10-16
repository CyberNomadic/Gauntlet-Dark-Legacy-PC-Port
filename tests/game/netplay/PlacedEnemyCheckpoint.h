#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "game/enemies/PlacedEnemiesCheckpoint.h"

namespace gdl::game {

/** Complete ordered placement state. Decoding does not authorize resources:
 * restoration must match the receiver's immutable native level roster. */
class PlacedEnemyCheckpoint {
public:
    /** Canonical encoded-byte SHA for an already validated native boundary.
     * Not a validity certificate; incoming state must still pass full restore. */
    static std::optional<std::array<u8, 32>>
    canonicalDigest(const PlacedEnemiesCheckpoint::State& state);

    static constexpr u32 kVersion = 1;
    static constexpr usize kMaxPlacements = 512;
    static constexpr usize kHeaderBytes = 32;
    static constexpr usize kPlacementBytes = 84;
    static constexpr usize kViewBytes = 64;
    static constexpr usize kMaxBytes =
        kHeaderBytes + kViewBytes + kMaxPlacements * (kPlacementBytes + 4);
    static std::optional<std::vector<u8>> encode(const PlacedEnemiesCheckpoint::State& state);
    static std::optional<PlacedEnemiesCheckpoint::State> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
