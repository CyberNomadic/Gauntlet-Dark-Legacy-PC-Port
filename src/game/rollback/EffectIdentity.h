#pragma once

#include <compare>

#include "engine/core/Types.h"

namespace gdl::game {
/** One authored B/C strike effect at its completed native birth tick. Chain
 * occurrences distinguish repeated strike rows; the effect ordinal counts every
 * authored chain entry, including missing artwork and standalone particle rows.
 * This identifies presentation only, never an accepted input or damage event. */
struct EffectIdentity {
    static constexpr u16 kMaxOccurrences = 128;
    static constexpr u16 kMaxEffects = 128;
    u64 epoch = 0;
    u64 tick = 0;
    u64 generation = 0;
    u32 grant = 0;
    u8 seat = 0;
    u16 occurrence = 0;
    u16 effectOrdinal = 0;
    bool valid() const;
    auto operator<=>(const EffectIdentity&) const = default;
};
} // namespace gdl::game
