#include "game/hud/LinkIndicator.h"

namespace gdl::game {
LinkQuality LinkIndicator::quality(s32 pingMs) {
    if (pingMs < 0) {
        return LinkQuality::Unknown;
    }
    if (pingMs < kGoodPingMs) {
        return LinkQuality::Good;
    }
    if (pingMs < kFairPingMs) {
        return LinkQuality::Fair;
    }
    return pingMs < kWeakPingMs ? LinkQuality::Weak : LinkQuality::Poor;
}

Color LinkIndicator::color(LinkQuality quality) {
    switch (quality) {
    case LinkQuality::Good: return Color::rgba(72, 208, 72);
    case LinkQuality::Fair: return Color::rgba(232, 200, 48);
    case LinkQuality::Weak: return Color::rgba(240, 140, 40);
    case LinkQuality::Poor: return Color::rgba(224, 64, 56);
    case LinkQuality::Unknown: break;
    }
    return unlit();
}

Color LinkIndicator::unlit() {
    return Color::rgba(255, 255, 255, 72);
}

u32 LinkIndicator::lit(LinkQuality quality) {
    switch (quality) {
    case LinkQuality::Good: return 4;
    case LinkQuality::Fair: return 3;
    case LinkQuality::Weak: return 2;
    case LinkQuality::Poor: return 1;
    case LinkQuality::Unknown: break;
    }
    return 0;
}

Vec2 LinkIndicator::size() {
    return {kBars * kBarWidth + (kBars - 1) * kBarGap, kShortestBar + (kBars - 1) * kBarStep};
}

void LinkIndicator::draw(Canvas& canvas, const Vec2& origin, LinkQuality quality) {
    const f32 bottom = origin.y + size().y;
    const u32 lit = LinkIndicator::lit(quality);
    for (u32 bar = 0; bar < kBars; ++bar) {
        const f32 height = kShortestBar + static_cast<f32>(bar) * kBarStep;
        canvas.fill({origin.x + static_cast<f32>(bar) * (kBarWidth + kBarGap), bottom - height,
                     kBarWidth, height},
                    bar < lit ? color(quality) : unlit());
    }
}
} // namespace gdl::game
