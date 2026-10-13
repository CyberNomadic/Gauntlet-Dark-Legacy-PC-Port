#pragma once

#include <compare>

#include "engine/core/Types.h"

namespace gdl::game {
/** One native weapon release at an exact completed simulation
 * tick. Animation generation is sampled after that tick's update, not inferred
 * from the last received or consumed input. Multiple visual components of the
 * same physical shot share this identity. Fan members retain their native launch
 * order even if another member expires before the next checkpoint. */
struct ShotIdentity {
    static constexpr u8 kMaxShots = 5;
    u64 epoch = 0;
    u64 tick = 0;
    u64 generation = 0;
    u32 grant = 0;
    u8 seat = 0;
    bool strong = false;
    u8 ordinal = 0;
    u32 emission = 0; ///< ordinary throw zero; turbo chain occurrence plus volley counter
    bool valid() const;
    std::strong_ordering operator<=>(const ShotIdentity& other) const;
    bool operator==(const ShotIdentity&) const = default;
};
} // namespace gdl::game
