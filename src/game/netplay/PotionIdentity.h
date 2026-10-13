#pragma once

#include <compare>

#include "engine/core/Types.h"

namespace gdl::game {
/** A successful native potion release, independent of ordinary weapon volleys.
 * Its completed simulation tick and animation generation survive corrections;
 * replaying a release never spends live inventory or applies damage. */
struct PotionIdentity {
    enum class Kind : u8 { Use, Throw };
    u64 epoch = 0;
    u64 tick = 0;
    u64 generation = 0;
    u32 grant = 0;
    u8 seat = 0;
    Kind kind = Kind::Use;
    bool valid() const;
    auto operator<=>(const PotionIdentity&) const = default;
};
} // namespace gdl::game
