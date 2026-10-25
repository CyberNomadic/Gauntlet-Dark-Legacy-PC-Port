#pragma once
#include <cmath>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/**
 * A flier's velocity that eases toward what is asked of it, the way a Source engine noclip
 * flier's does: a fixed share of the gap closes each second, so starts, turns and stops are
 * smooth instead of abrupt.
 */
struct FlightVelocity {
    static constexpr f32 kResponse = 6.0f; ///< the gap's share closed a second, compounded
    static constexpr f32 kRest = 0.01f;    ///< under this, with nothing asked, it stops dead

    Vec3 velocity{0.0f, 0.0f, 0.0f};

    /** Eases toward `wish` (units a second) over `seconds`; the way travelled meanwhile. */
    Vec3 advance(const Vec3& wish, f32 seconds) {
        const f32 share = 1.0f - std::exp(-kResponse * seconds);
        velocity += (wish - velocity) * share;
        if (wish == Vec3{0.0f, 0.0f, 0.0f} && glm::length(velocity) < kRest) {
            velocity = Vec3{0.0f, 0.0f, 0.0f};
        }
        return velocity * seconds;
    }

    void reset() { velocity = Vec3{0.0f, 0.0f, 0.0f}; }
};

} // namespace gdl::game
