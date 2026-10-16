#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "game/enemies/EnemyMissiles.h"
#include "game/netplay/RandomCheckpoint.h"

namespace gdl::game {

/** Complete native flight, burst and undelivered-output state. Model IDs resolve only
 * through a trusted, preloaded resource manifest after decoding; no model data travels.
 * Transport bounds are deliberately smaller than local checkpoint capture bounds.
 * An oversized pool is refused intact, never pruned to make a packet fit. */
class EnemyMissilesCheckpoint {
public:
    /** Canonical encoded-byte SHA for an already validated native boundary.
     * Not a validity certificate; incoming state must still pass full restore. */
    static std::optional<std::array<u8, 32>>
    canonicalDigest(const EnemyMissiles::Checkpoint& state);

    static constexpr usize kMaxMissiles = 512;
    static constexpr usize kMaxBursts = 64;
    static constexpr usize kMaxEvents = 2048;
    static constexpr usize kMaxContacts = 4096;
    static constexpr usize kMaxStages = 512;
    static constexpr usize kHeaderBytes = 44 + RandomCheckpoint::kBytes;
    static constexpr usize kFlightBytes = 124;
    static constexpr usize kBurstBytes = 64;
    static constexpr usize kHitBytes = 64;
    static constexpr usize kRockBytes = 8;
    static constexpr usize kPickupBytes = 24;
    static constexpr usize kMaxBytes = kHeaderBytes + kMaxMissiles * kFlightBytes +
                                       kMaxBursts * kBurstBytes + kMaxEvents * kHitBytes +
                                       kMaxContacts * 8 + kMaxStages * 4;

    static std::optional<std::vector<u8>> encode(const EnemyMissiles::Checkpoint& state);
    static std::optional<EnemyMissiles::Checkpoint> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
