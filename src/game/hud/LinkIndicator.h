#pragma once

#include <memory>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"

namespace gdl::game {
/** How a link feels to play on, judged from its round trip. */
enum class LinkQuality : u8 { Unknown, Poor, Fair, Good };

/** The connection mark above the ping: a broadcast icon and a bar filled green, yellow
 * or red for a good, fair or poor link, left empty and grey while the ping is unknown. */
class LinkIndicator {
public:
    static constexpr s32 kGoodPingMs = 60; ///< an input delay of at most one tick
    static constexpr s32 kFairPingMs = 150;
    static constexpr f32 kBarHeight = 6;
    static constexpr f32 kGap = 4; ///< between the icon and the bar
    static LinkQuality quality(s32 pingMs);
    static Color color(LinkQuality quality);
    /** The share of the bar a quality fills. */
    static f32 fill(LinkQuality quality);
    /** The icon's pixels: white where the glyph is, clear elsewhere. */
    static Image icon();
    /** The icon's size on screen, two screen pixels per glyph pixel. */
    static Vec2 iconSize();
    /** Draws the icon at the area's left and the bar across the rest, both centred on
     * the area's height. */
    void draw(Canvas& canvas, RenderDevice& device, const Rect& area, LinkQuality quality);
    void release();

private:
    std::unique_ptr<Texture> m_icon;
};
} // namespace gdl::game
