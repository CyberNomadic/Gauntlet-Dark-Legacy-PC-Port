#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/math/Mt19937.h"

namespace gdl::game {

/** Canonical MT19937 words followed by its cursor, all explicitly little-endian. */
class RandomCheckpoint {
public:
    static constexpr usize kBytes = 4 * (Mt19937::state_size + 1);
    static std::optional<std::vector<u8>> encode(const Mt19937::Checkpoint& state);
    static std::optional<Mt19937::Checkpoint> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
