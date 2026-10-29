#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/ui/Canvas.h"
#include "engine/world/WorldCamera.h"

#include "game/party/PlayerRuntime.h"

namespace gdl::game {

/**
 * Small arrows at the edge of the view, each pointing the way to a companion off this
 * screen in that character's colour: how a party on solo cameras keeps track of itself.
 * A companion on screen gets none, and the band of status boxes along the bottom is kept
 * clear. Presentation only, read fresh from the party every frame.
 */
class CompanionArrows {
public:
    static constexpr f32 kInset = 12.0f;    ///< from the view's edge to an arrow's tip
    static constexpr f32 kLength = 10.0f;   ///< tip to base
    static constexpr f32 kHalfWidth = 5.0f; ///< half the base
    static constexpr f32 kOutline = 1.5f;   ///< the dark rim about each
    static constexpr u8 kAlpha = 208;       ///< a little see-through, to stay out of the way

    struct Arrow {
        Vec2 tip{0.0f};
        Vec2 direction{0.0f, -1.0f}; ///< unit, the way the tip points (y down the screen)
        Color color;
        s32 seat = -1;
    };

    /** The arrows for the standing members of `players` other than the seats in `ownSeats`
     * (a bit each) who are off the view: a canvas `width` across and `height` deep whose
     * arrows keep above `bottom`. `camera` orients a companion behind the eye, whom `clip`
     * cannot place. */
    static std::vector<Arrow> place(std::span<const PlayerRuntime> players, u8 ownSeats,
                                    const WorldCamera& camera, const Mat4& clip,
                                    const Mat4& canvasProjection, f32 width, f32 height,
                                    f32 bottom, f32 frameBlend = 1.0f);
    /** The unit direction across the view from its centre toward `point`, or nothing with
     * the point on the view. */
    static std::optional<Vec2> wayOff(const Vec3& point, const WorldCamera& camera,
                                      const Mat4& clip, const Mat4& canvasProjection, f32 width,
                                      f32 height);
    /** Where a ray from the view's centre along `direction` meets the inset edge. */
    static Vec2 edgePoint(const Vec2& direction, f32 width, f32 height, f32 bottom);

    void draw(Canvas& canvas, std::span<const PlayerRuntime> players, u8 ownSeats,
              const WorldCamera& camera, const Mat4& clip, const Mat4& canvasProjection,
              f32 width, f32 height, f32 bottom, f32 frameBlend = 1.0f);
    /** How many arrows the last draw showed. */
    usize shown() const { return m_shown; }

private:
    usize m_shown = 0;
};

} // namespace gdl::game
