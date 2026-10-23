#include "game/players/PlayerActor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

#include "game/characters/Progression.h"

namespace gdl::game {

bool PlayerActor::Checkpoint::valid() const {
    const auto bounded = [](f32 value, f32 low, f32 high) {
        return std::isfinite(value) && value >= low && value <= high;
    };
    const auto vector = [&](const Vec3& value) {
        return bounded(value.x, -kMaxCoordinate, kMaxCoordinate) &&
               bounded(value.y, -kMaxCoordinate, kMaxCoordinate) &&
               bounded(value.z, -kMaxCoordinate, kMaxCoordinate);
    };
    if (!vector(position) || !bounded(yaw, -kMaxCoordinate, kMaxCoordinate) ||
        !bounded(speed, kMinSpeed, kMaxSpeed) ||
        !bounded(paceBonus, -kMaxCoordinate, kMaxCoordinate) || !bounded(radius, 0.01f, 50) ||
        !bounded(height, 0.01f, 100) || !bounded(followHeight, 0, 100) ||
        !bounded(attentionHeight, 0, 100) || !bounded(floorBase, -kMaxCoordinate, kMaxCoordinate) ||
        floorObject < -1 || floorObject > kMaxObject || contacts.size() > kMaxContacts) {
        return false;
    }
    for (usize index = 0; index < contacts.size(); ++index) {
        const auto& contact = contacts[index];
        if (contact.object < -1 || contact.object > kMaxObject || !vector(contact.point)) {
            return false;
        }
        // WorldCollision keeps the first contact with each object for the tick.
        for (usize previous = 0; previous < index; ++previous) {
            if (contacts[previous].object == contact.object) {
                return false;
            }
        }
    }
    return true;
}

PlayerActor::Checkpoint PlayerActor::checkpoint() const {
    Checkpoint state{m_position, m_yaw,       m_speed,        m_paceBonus,
                     m_radius,   m_height,    m_followHeight, m_attentionHeight,
                     m_moving,   m_floorBase, m_floorObject,  m_floorFlags,
                     {}};
    state.contacts.reserve(m_wallContacts.size());
    for (const auto& contact : m_wallContacts) {
        state.contacts.push_back({contact.object, contact.point});
    }
    return state;
}

bool PlayerActor::restore(const Checkpoint& state) {
    if (!state.valid()) {
        return false;
    }
    std::vector<WallContact> contacts;
    contacts.reserve(state.contacts.size());
    for (const auto& contact : state.contacts) {
        contacts.push_back({contact.object, contact.point});
    }
    m_position = state.position;
    m_yaw = state.yaw;
    m_speed = state.speed;
    m_paceBonus = state.paceBonus;
    m_radius = state.radius;
    m_height = state.height;
    m_followHeight = state.followHeight;
    m_attentionHeight = state.attentionHeight;
    m_moving = state.moving;
    m_stepOffset = Vec3{0.0f};
    m_floorBase = state.floorBase;
    m_floorObject = state.floorObject;
    m_floorFlags = state.floorFlags;
    m_wallContacts = std::move(contacts);
    return true;
}

void PlayerActor::spawn(s32 player, const CharacterSave& save, const ClassStats* stats,
                        const Vec3& position, f32 yaw) {
    m_player = player;
    m_save = save;
    m_position = position;
    m_yaw = yaw;
    m_moving = false;
    m_stepOffset = Vec3{0.0f};
    m_floorBase = position.y;
    m_floorObject = -1;
    m_floorFlags = 0;
    m_wallContacts.clear();
    f32 speedStat = 0.0f;
    m_radius = kDefaultWidth * 0.5f;
    m_height = kDefaultHeight;
    m_followHeight = kDefaultFollowHeight;
    m_attentionHeight = kDefaultFollowHeight;
    if (stats != nullptr) {
        speedStat = static_cast<f32>(characterStats(*stats, save).speed());
        if (stats->width > 0.0f) {
            m_radius = stats->width * 0.5f;
        }
        if (stats->height > 0.0f) {
            m_height = stats->height;
        }
        if (stats->collisionY > 0.0f) {
            m_followHeight = stats->collisionY;
        }
        if (stats->attentionY > 0.0f) {
            m_attentionHeight = stats->attentionY;
        }
    }
    m_speed = kMinSpeed + std::clamp(speedStat * kStatScale, 0.0f, 1.0f) * (kMaxSpeed - kMinSpeed);
}

void PlayerActor::restoreMotion(const Vec3& position, f32 yaw, f32 speed, f32 radius, f32 height,
                                f32 followHeight) {
    m_position = position;
    m_yaw = yaw;
    m_speed = speed;
    m_paceBonus = 0;
    m_radius = radius;
    m_height = height;
    m_followHeight = followHeight;
    m_moving = false;
    m_stepOffset = Vec3{0.0f};
    m_floorBase = position.y;
    m_floorObject = -1;
    m_floorFlags = 0;
    m_wallContacts.clear();
}

f32 PlayerActor::headingOf(const MoveInput& input, f32 cameraYaw) {
    return portable::atan2(input.direction.x, input.direction.y) + cameraYaw;
}

void PlayerActor::faceToward(const Vec3& point) {
    const Vec3 offset = point - m_position;
    if (portable::hypot(offset.x, offset.z) > 1e-5f) {
        m_yaw = portable::atan2(offset.x, offset.z);
    }
}

void PlayerActor::update(const MoveInput& input, f32 cameraYaw, f32 seconds,
                         const WorldCollision* collision, f32 moveScale, bool keepFacing) {
    m_moving = input.any() && seconds > 0.0f;
    m_stepOffset = Vec3{0.0f};
    if (!m_moving) {
        return;
    }
    const f32 heading = headingOf(input, cameraYaw);
    const f32 pace = speed();
    const f32 distance =
        std::min(pace * input.magnitude * seconds, kMoveLimit * pace * seconds) * moveScale;
    if (!keepFacing) {
        m_yaw = heading;
    }
    if (distance <= 0.0f) {
        m_moving = false;
        return;
    }
    const Vec3 direction{portable::sin(heading), 0.0f, portable::cos(heading)};
    m_stepOffset = direction * distance;
    travel(m_stepOffset, collision);
}

void PlayerActor::slide(const Vec3& offset, const WorldCollision* collision) {
    travel(Vec3{offset.x, 0.0f, offset.z}, collision);
}

f32 PlayerActor::minimumWallY(f32 footHeight) const {
    constexpr f32 kWallOriginLift = 1.0f;
    return footHeight + m_followHeight + kWallOriginLift - reach();
}

void PlayerActor::travel(const Vec3& offset, const WorldCollision* collision) {
    Vec3 dpos{offset.x, 0.0f, offset.z};
    if (glm::dot(dpos, dpos) <= 0.0f) {
        return;
    }
    if (collision == nullptr) {
        m_position += dpos;
        m_floorBase = m_position.y;
        return;
    }
    // fn_80088714: the wall sphere stands at the collision centre a unit up, as wide as the
    // body (PlayerWallCollide's radius is the class width). A wall met turns the step along
    // it (SlideAlongWall); a second wall against the first stops it.
    const f32 width = reach();
    const Vec3 centre = m_position + Vec3{0.0f, m_followHeight + kWallSphereLift, 0.0f};
    bool walled = false;
    if (const auto wall = collision->sweepSphere(centre, centre + dpos, width)) {
        walled = true;
        noteContact(wall->object, wall->point);
        slideAlongWall(dpos, centre, *wall, width);
        if (const auto second =
                collision->sweepSphere(centre, centre + dpos, kSecondSweep * width)) {
            noteContact(second->object, second->point);
            if (second->time > 0.0f) {
                if (glm::dot(second->normal, wall->normal) < 0.5f) {
                    dpos = Vec3{0.0f};
                }
            } else {
                // A wall the sphere already overlaps (a lift carried the body into it; the
                // original's sweeps never leave one overlapping) pushes it out instead of
                // holding it: the body works its way free.
                slideAlongWall(dpos, centre, *second, width);
            }
        }
        if (glm::dot(dpos, dpos) <= 0.0f) {
            return;
        }
    }
    // PlayerCollideFloor: the floor under where the step ends, found by FloorCollide's
    // sphere swept down from the collision centre by three and the body's half height.
    const f32 depth = kFloorProbe + m_height * 0.5f;
    const auto floorAt = [&](const Vec3& feet) {
        return collision->floorUnder(feet + Vec3{0.0f, m_followHeight, 0.0f}, depth, width);
    };
    if (m_floorObject < 0) {
        if (const auto here = floorAt(m_position)) {
            m_floorBase = here->y;
            m_floorObject = here->object;
            m_floorFlags = here->objectFlags;
        }
    }
    Vec3 end = m_position + dpos;
    auto floor = floorAt(end);
    if (!floor) {
        return; // nothing to stand on there: the step is refused, and fall() takes it
    }
    // PlayerNewFloor: a lift that holds its riders keeps them on it.
    if (collision->floorExitBlocked(m_floorObject) && floor->object != m_floorObject) {
        return;
    }
    // A floor more than a step from floor_base is refused: up, and down too (the lip),
    // with a unit more on the floor it already stands on.
    const f32 limit =
        kStepUp + (m_floorObject >= 0 && floor->object == m_floorObject ? kSameFloorStep : 0.0f);
    if (std::abs(floor->y - m_floorBase) > limit) {
        m_floorBase = m_position.y;
        return;
    }
    // Only a face under the point lets the step be taken as meant. With no wall, a sphere's
    // width further on is looked at too: an edge met there (a drop beyond it) pulls the
    // step back so the body stops a width short of the edge. An edge met under the end
    // itself pulls the step back onto it. A step towards the contact passes.
    // A gap narrower than the body, with floor of the same height a width beyond it, is
    // bridged by the feet (the Underworld's pillars stand a hand's breadth from the ground
    // about them); the original's levels have no such seams for its rule to meet.
    const Vec3 look = glm::normalize(dpos) * width;
    const auto bridged = [&](const Vec3& from, const FloorHit& contact) {
        const auto beyond = floorAt(from + look);
        return beyond && std::abs(beyond->y - contact.y) <= limit &&
               portable::hypot(beyond->point.x - (from.x + look.x),
                               beyond->point.z - (from.z + look.z)) < kContactEpsilon;
    };
    Vec2 away{end.x - floor->point.x, end.z - floor->point.z};
    f32 distance = glm::length(away);
    f32 towards = away.x * dpos.x + away.y * dpos.z;
    if (distance >= kContactEpsilon && bridged(end, *floor)) {
        distance = 0.0f;
    }
    bool lookedAhead = false;
    if (distance < kContactEpsilon && !walled) {
        lookedAhead = true;
        const Vec3 ahead = end + look;
        if (const auto next = floorAt(ahead)) {
            if (((next->objectFlags & kSlopedFloor) != 0 &&
                 std::abs(next->y - floor->y) < kSlopeAllowance) ||
                bridged(ahead, *next)) {
                distance = 0.0f;
            } else {
                away = Vec2{ahead.x - next->point.x, ahead.z - next->point.z};
                distance = glm::length(away);
                towards = away.x * dpos.x + away.y * dpos.z;
            }
        } else {
            away = Vec2{look.x, look.z};
            distance = width;
            towards = 0.0f;
        }
    }
    if (distance >= kContactEpsilon && towards >= 0.0f) {
        // An end over no face is pulled onto the edge met. The original takes the whole of
        // a looked-ahead overhang off as well, which turns a step about when a body already
        // stands nearer an edge than its width; here no more than the step's own way out
        // comes off then, so such a body holds still there and slides along.
        f32 taken = distance;
        const Vec2 out = away / distance;
        if (lookedAhead) {
            const f32 length = portable::hypot(dpos.x, dpos.z);
            const f32 against = (out.x * dpos.x + out.y * dpos.z) / length;
            if (against > kContactEpsilon) {
                taken = std::min(taken, length / against);
            }
        }
        dpos.x -= out.x * taken;
        dpos.z -= out.y * taken;
        if (glm::dot(dpos, dpos) <= 0.0f) {
            return;
        }
        end = m_position + dpos;
        floor = floorAt(end);
        if (!floor) {
            return;
        }
    }
    m_floorBase = floor->y;
    m_floorObject = floor->object;
    m_floorFlags = floor->objectFlags;
    // PlayerMotion lifts the body to a floor above it at once, and scales a climbing step
    // down so the way travelled, rise included, is the way meant; a floor below is sunk
    // to by fall() at the native pace.
    const f32 rise = m_floorBase - m_position.y;
    if (rise > 0.0f) {
        const f32 horizontal = portable::hypot(dpos.x, dpos.z);
        if (horizontal > kSlopeScaleFrom && rise > kClimbFraction * horizontal) {
            dpos *= horizontal / portable::hypot(horizontal, rise);
        }
        m_position = Vec3{m_position.x + dpos.x, m_floorBase, m_position.z + dpos.z};
        return;
    }
    m_position.x += dpos.x;
    m_position.z += dpos.z;
}

void PlayerActor::slideAlongWall(Vec3& dpos, const Vec3& centre, const SphereHit& wall, f32 width) {
    // SlideAlongWall: what of the step's end is within the width of the wall's plane is
    // taken off along the normal, an axis at a time; a correction out of all proportion
    // (over one and three quarter widths and the step) stops that axis instead.
    const Vec3 end = centre + dpos;
    const f32 depth =
        (end.x - wall.point.x) * wall.normal.x + (end.z - wall.point.z) * wall.normal.z - width;
    if (depth >= 0.0f) {
        return;
    }
    const f32 outX = wall.normal.x * depth;
    const f32 outZ = wall.normal.z * depth;
    if (std::abs(outX) < kSlideLimit * width + std::abs(dpos.x)) {
        dpos.x -= outX;
    } else {
        dpos.x = 0.0f;
    }
    if (std::abs(outZ) < kSlideLimit * width + std::abs(dpos.z)) {
        dpos.z -= outZ;
    } else {
        dpos.z = 0.0f;
    }
}

void PlayerActor::noteContact(s32 object, const Vec3& point) {
    if (std::ranges::none_of(m_wallContacts,
                             [&](const WallContact& seen) { return seen.object == object; })) {
        m_wallContacts.push_back(WallContact{object, point});
    }
}

bool PlayerActor::fall(f32 seconds, const WorldCollision& collision) {
    // PlayerCollideFloor standing: the floor under the body replaces floor_base when it is
    // more than a step away or moves; PlayerMotion then rises to it at once or sinks to it
    // at sixteen a second. A floor just above the feet lifts them too, as nothing in the
    // original ever leaves a body below its floor.
    const f32 depth = kFloorProbe + m_height * 0.5f;
    const auto floor =
        collision.floorUnder(m_position + Vec3{0.0f, m_followHeight, 0.0f}, depth, reach());
    if (floor) {
        if (collision.floorExitBlocked(m_floorObject) && floor->object != m_floorObject) {
            // A lift that holds its riders keeps this one at its height (PlayerCollideFloor
            // refuses any other floor while the flags hold).
        } else if (m_floorObject < 0 || floor->object != m_floorObject ||
                   std::abs(floor->y - m_floorBase) > kStepUp || floor->y > m_position.y ||
                   collision.moving(floor->object)) {
            m_floorBase = floor->y;
            m_floorObject = floor->object;
            m_floorFlags = floor->objectFlags;
        }
    } else if (collision.floorExitBlocked(m_floorObject)) {
        // held by its lift still
    } else {
        // Nothing within reach: it sinks, never through whatever floor is far below.
        const auto far = collision.floorAt(m_position, m_followHeight, kFallReach, reach());
        m_floorBase = far ? far->y : m_position.y - kFallReach;
        m_floorObject = -1;
        m_floorFlags = 0;
    }
    if (m_floorBase >= m_position.y) {
        m_position.y = m_floorBase;
        return false;
    }
    m_position.y = std::max(m_floorBase, m_position.y - kFallSpeed * std::max(seconds, 0.0f));
    return true;
}

void PlayerActor::settle(const WorldCollision& collision) {
    if (const auto floor = collision.floorAt(m_position, kDrop, kDrop, kFloorEdgeReach)) {
        m_position.y = floor->y;
        m_floorBase = floor->y;
        m_floorObject = floor->object;
        m_floorFlags = floor->objectFlags;
    }
}

Mat4 PlayerActor::transform() const {
    return portable::rotate(glm::translate(Mat4{1.0f}, m_position), m_yaw, Vec3{0.0f, 1.0f, 0.0f});
}

} // namespace gdl::game
