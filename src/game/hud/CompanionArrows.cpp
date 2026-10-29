#include "game/hud/CompanionArrows.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/core/Types.h"

#include "game/characters/ClassData.h"
#include "game/party/PartyFigures.h"
#include "game/party/PartyNames.h"

namespace gdl::game {
namespace {
constexpr f32 kAhead = 1e-4f; ///< the least clip depth a point in front of the eye has
constexpr f32 kFlat = 1e-5f;  ///< below this a direction's component is taken as nought

std::optional<Vec2> unit(const Vec2& v) {
    const f32 length = glm::length(v);
    if (!(length > kFlat)) {
        return std::nullopt;
    }
    return v / length;
}
} // namespace

std::optional<Vec2> CompanionArrows::wayOff(const Vec3& point, const WorldCamera& camera,
                                            const Mat4& clip, const Mat4& canvasProjection,
                                            f32 width, f32 height) {
    const Vec2 centre{width * 0.5f, height * 0.5f};
    const Vec4 at = clip * Vec4{point, 1.0f};
    if (at.w > kAhead) {
        const auto on = PartyNames::screenOf(clip, point, canvasProjection);
        if (!on) {
            return std::nullopt;
        }
        if (on->x >= 0.0f && on->x <= width && on->y >= 0.0f && on->y <= height) {
            return std::nullopt; // on the view
        }
        if (const auto way = unit(*on - centre)) {
            return way;
        }
    }
    // Behind the eye the picture is no guide: the way across the ground is, the camera's
    // right being the view's right and its forward the way up the view.
    const Vec3 away = point - camera.position;
    const Vec3 right = camera.right();
    const Vec3 forward = camera.forward();
    const Vec2 ground{away.x * right.x + away.z * right.z, -(away.x * forward.x + away.z * forward.z)};
    return unit(ground).value_or(Vec2{0.0f, 1.0f});
}

Vec2 CompanionArrows::edgePoint(const Vec2& direction, f32 width, f32 height, f32 bottom) {
    const Vec2 centre{width * 0.5f, height * 0.5f};
    const f32 left = kInset;
    const f32 right = width - kInset;
    const f32 top = kInset;
    const f32 low = std::max(top, bottom - kInset);
    f32 reach = std::numeric_limits<f32>::max();
    if (direction.x > kFlat) {
        reach = std::min(reach, (right - centre.x) / direction.x);
    } else if (direction.x < -kFlat) {
        reach = std::min(reach, (left - centre.x) / direction.x);
    }
    if (direction.y > kFlat) {
        reach = std::min(reach, (low - centre.y) / direction.y);
    } else if (direction.y < -kFlat) {
        reach = std::min(reach, (top - centre.y) / direction.y);
    }
    if (!(reach < std::numeric_limits<f32>::max())) {
        return centre;
    }
    return centre + direction * std::max(0.0f, reach);
}

std::vector<CompanionArrows::Arrow>
CompanionArrows::place(std::span<const PlayerRuntime> players, u8 ownSeats,
                       const WorldCamera& camera, const Mat4& clip, const Mat4& canvasProjection,
                       f32 width, f32 height, f32 bottom, f32 frameBlend) {
    std::vector<Arrow> arrows;
    for (const PlayerRuntime& runtime : players) {
        const s32 seat = runtime.actor.player();
        if (runtime.life != PlayerLife::Standing || runtime.departed || seat < 0 || seat > 7 ||
            ((static_cast<u32>(ownSeats) >> static_cast<u32>(seat)) & 1U) != 0) {
            continue;
        }
        const Vec3 offset = runtime.actor.attentionPoint() - runtime.actor.position();
        const Vec3 body = Vec3{PartyFigures::presentationBody(runtime, frameBlend)[3]} + offset;
        const auto way = wayOff(body, camera, clip, canvasProjection, width, height);
        if (!way) {
            continue;
        }
        Color tint = playerColor(runtime.actor.save().color);
        tint.a = kAlpha;
        arrows.push_back({edgePoint(*way, width, height, bottom), *way, tint, seat});
    }
    return arrows;
}

void CompanionArrows::draw(Canvas& canvas, std::span<const PlayerRuntime> players, u8 ownSeats,
                           const WorldCamera& camera, const Mat4& clip,
                           const Mat4& canvasProjection, f32 width, f32 height, f32 bottom,
                           f32 frameBlend) {
    const auto arrows =
        place(players, ownSeats, camera, clip, canvasProjection, width, height, bottom, frameBlend);
    m_shown = arrows.size();
    for (const Arrow& arrow : arrows) {
        const Vec2 d = arrow.direction;
        const Vec2 across{-d.y, d.x};
        // The rim first, a little larger all round, then the arrow in its colour.
        const Vec2 rimTip = arrow.tip + d * kOutline;
        const Vec2 rimBase = rimTip - d * (kLength + 2.5f * kOutline);
        canvas.fillTriangle(rimTip, rimBase + across * (kHalfWidth + kOutline),
                            rimBase - across * (kHalfWidth + kOutline),
                            Color::rgba(0, 0, 0, kAlpha));
        const Vec2 base = arrow.tip - d * kLength;
        canvas.fillTriangle(arrow.tip, base + across * kHalfWidth, base - across * kHalfWidth,
                            arrow.color);
    }
}

} // namespace gdl::game
