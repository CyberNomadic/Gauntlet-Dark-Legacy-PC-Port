#pragma once

#include <optional>
#include <span>
#include <vector>

#include "game/enemies/Generators.h"

namespace gdl::game {
/** Explicit transport of a native generator checkpoint. Art and support-object
 * identities are resolved against the receiver's already-bound trusted level;
 * decoding alone does not authorize restoration into a different roster. */
class GeneratorCheckpoint {
public:
    static constexpr u32 kVersion = 1;
    static constexpr usize kMaxGenerators = 512;
    static constexpr usize kMaxBodies = static_cast<usize>(kEnemyKindCount) + 2;
    static constexpr usize kHeaderBytes = 40;
    static constexpr usize kSlotBytes = 308;
    static constexpr usize kBodyBytes = 16;
    static constexpr usize kViewBytes = 64;
    static constexpr usize kAttentionBytes = 12;
    static constexpr usize kMaxBytes = kHeaderBytes + kViewBytes + kAttentionBytes +
                                       2 * kMaxGenerators * kSlotBytes + kMaxBodies * kBodyBytes;

    /** Oversized but locally valid pools are refused, never truncated. */
    static std::optional<std::vector<u8>> encode(const Generators::Checkpoint& state);
    static std::optional<Generators::Checkpoint> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
