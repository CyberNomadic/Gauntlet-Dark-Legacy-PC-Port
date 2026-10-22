#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/ui/Canvas.h"

namespace gdl::game {
/** How a link feels to play on, judged from its round trip. */
enum class LinkQuality : u8 { Unknown, Poor, Fair, Good };

/** The signal-strength mark above the ping: four rising bars, all lit green for a good
 * link, three lit yellow for a fair one, one lit red for a poor one and none while the
 * ping is unknown; unlit bars stay grey. */
class LinkIndicator {
public:
    static constexpr s32 kGoodPingMs = 60; ///< an input delay of at most one tick
    static constexpr s32 kFairPingMs = 150;
    static constexpr u32 kBars = 4;
    static constexpr f32 kBarWidth = 4;
    static constexpr f32 kBarGap = 2;
    static constexpr f32 kBarStep = 3; ///< each bar rises this much over the last
    static constexpr f32 kShortestBar = 5;
    static LinkQuality quality(s32 pingMs);
    static Color color(LinkQuality quality);
    static Color unlit();
    /** How many bars a quality lights, from the shortest up. */
    static u32 lit(LinkQuality quality);
    /** The mark's size on screen. */
    static Vec2 size();
    /** Draws the mark with its top-left corner at `origin`; the bars stand on its bottom. */
    static void draw(Canvas& canvas, const Vec2& origin, LinkQuality quality);
};
} // namespace gdl::game
