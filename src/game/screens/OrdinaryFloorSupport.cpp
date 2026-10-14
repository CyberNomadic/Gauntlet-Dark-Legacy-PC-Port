#include "game/screens/OrdinaryFloorSupport.h"

#include <cmath>

#include "game/screens/FloorRiding.h"
#include "game/screens/PartyMotion.h"

namespace gdl::game {
namespace {
bool finite(const Mat4& matrix) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

bool invertiblePlacement(const Mat4& matrix) {
    if (!finite(matrix) || matrix[0][3] != 0 || matrix[1][3] != 0 || matrix[2][3] != 0 ||
        matrix[3][3] != 1) {
        return false;
    }
    const f32 determinant = glm::determinant(matrix);
    // FloorRiding takes this inverse without an error channel. Reject a
    // collapsed/malformed support before entering native carry, not afterward.
    return std::isfinite(determinant) && determinant != 0 && finite(glm::inverse(matrix));
}

bool grounded(const PlayerActor& actor, const WorldCollision& collision, const FloorHit& hit) {
    return std::isfinite(hit.y) && std::abs(hit.y - actor.position().y) <= 0.0001f &&
           actor.position().y > collision.lowest() - PartyMotion::kLostDepth;
}
} // namespace

bool OrdinaryFloorSupport::accepts(const PlayerRuntime& player, const WorldCollision& collision,
                                   Phase phase, bool allowMoving) {
    if (!collision.loaded() || player.life != PlayerLife::Standing || player.departed ||
        (phase != Phase::BeforeCarry && phase != Phase::AfterLanding)) {
        return false;
    }
    const auto body = player.actor.checkpoint();
    if (!body.valid()) {
        return false;
    }
    const auto& floor = player.floor;
    if (floor.object < -1 || floor.object > PlayerActor::Checkpoint::kMaxObject) {
        return false;
    }
    const bool rider = (floor.flags & FloorRiding::kMoving) != 0;
    if (rider != floor.placement.has_value()) {
        return false;
    }
    if (rider) {
        if (!allowMoving || floor.object < 0 || !collision.moving(floor.object) ||
            !invertiblePlacement(*floor.placement)) {
            return false;
        }
        const auto current = collision.objectTransform(floor.object);
        if (!current || !invertiblePlacement(*current)) {
            return false;
        }
        if (phase == Phase::BeforeCarry) {
            PlayerRuntime carried;
            if (!carried.actor.restore(body)) {
                return false;
            }
            carried.floor = floor;
            FloorRiding::carry(carried, collision);
            if (!carried.actor.checkpoint().valid()) {
                return false;
            }
            const auto support = FloorRiding::support(carried.actor, collision);
            return support && support->object == floor.object &&
                   support->objectFlags == floor.flags &&
                   grounded(carried.actor, collision, *support);
        }
        // land() records the current placement. Requiring this only in the
        // player-motion phase avoids rejecting legitimate post-trigger lag.
        if (*floor.placement != *current) {
            return false;
        }
    }
    const auto support = FloorRiding::support(player.actor, collision);
    if (!support || !grounded(player.actor, collision, *support) ||
        (!allowMoving && collision.floorExitBlocked(support->object))) {
        return false;
    }
    const bool movingSupport =
        collision.moving(support->object) || (support->objectFlags & FloorRiding::kMoving) != 0;
    if (movingSupport != rider) {
        // A visible deck under the feet is not a substitute for the native
        // support link needed to carry them on the next simulation tick.
        return false;
    }
    if (rider) {
        return floor.object == support->object && floor.flags == support->objectFlags;
    }
    // Fixture pushes run after land() without another landing pass. A static
    // coplanar seam can retain the preceding static object's link until the
    // next native land(); it has no transform to carry. Trusted owner membership
    // is checked by the enclosing stage checkpoint, not inferred from this probe.
    // An empty post-motion link, however, also marks native rescue and cannot
    // be accepted merely because rescue left the feet over a floor.
    return phase == Phase::BeforeCarry || floor.object != -1 || floor.flags != 0;
}

} // namespace gdl::game
