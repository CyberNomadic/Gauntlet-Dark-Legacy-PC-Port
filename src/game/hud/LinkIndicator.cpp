#include "game/hud/LinkIndicator.h"

#include <array>
#include <string_view>

#include "engine/render/RenderTypes.h"

namespace gdl::game {
namespace {
constexpr f32 kIconScale = 2;
// A dot with two arcs to each side: a broadcast.
constexpr std::array<std::string_view, 7> kGlyph{
    "..#.....#..", ".#.......#.", "#..#...#..#", "#.#..#..#.#",
    "#..#...#..#", ".#.......#.", "..#.....#..",
};
constexpr u32 kGlyphWidth = 11;
} // namespace

LinkQuality LinkIndicator::quality(s32 pingMs) {
    if (pingMs < 0) {
        return LinkQuality::Unknown;
    }
    if (pingMs < kGoodPingMs) {
        return LinkQuality::Good;
    }
    return pingMs < kFairPingMs ? LinkQuality::Fair : LinkQuality::Poor;
}

Color LinkIndicator::color(LinkQuality quality) {
    switch (quality) {
    case LinkQuality::Good: return Color::rgba(72, 208, 72);
    case LinkQuality::Fair: return Color::rgba(232, 200, 48);
    case LinkQuality::Poor: return Color::rgba(224, 64, 56);
    case LinkQuality::Unknown: break;
    }
    return Color::rgba(128, 128, 128);
}

f32 LinkIndicator::fill(LinkQuality quality) {
    switch (quality) {
    case LinkQuality::Good: return 1.0f;
    case LinkQuality::Fair: return 2.0f / 3.0f;
    case LinkQuality::Poor: return 1.0f / 3.0f;
    case LinkQuality::Unknown: break;
    }
    return 0.0f;
}

Image LinkIndicator::icon() {
    Image image = Image::filled(kGlyphWidth, static_cast<u32>(kGlyph.size()), Color::transparent());
    for (u32 y = 0; y < kGlyph.size(); ++y) {
        for (u32 x = 0; x < kGlyphWidth; ++x) {
            if (kGlyph[y][x] == '#') {
                const usize at = (static_cast<usize>(y) * kGlyphWidth + x) * 4;
                image.pixels[at] = image.pixels[at + 1] = image.pixels[at + 2] = 255;
                image.pixels[at + 3] = 255;
            }
        }
    }
    return image;
}

Vec2 LinkIndicator::iconSize() {
    return {kGlyphWidth * kIconScale, static_cast<f32>(kGlyph.size()) * kIconScale};
}

void LinkIndicator::draw(Canvas& canvas, RenderDevice& device, const Rect& area,
                         LinkQuality quality) {
    if (m_icon == nullptr) {
        const auto image = icon();
        m_icon = device.createTexture(
            {image.width, image.height, TextureFilter::Nearest, TextureWrap::ClampToEdge},
            image.pixels);
    }
    const auto size = iconSize();
    const f32 middle = area.y + area.height / 2;
    canvas.draw(*m_icon, {area.x, middle - size.y / 2, size.x, size.y});
    const Rect track{area.x + size.x + kGap, middle - kBarHeight / 2,
                     std::max(area.width - size.x - kGap, 0.0f), kBarHeight};
    canvas.fill(track, Color::rgba(255, 255, 255, 48));
    const f32 filled = track.width * fill(quality);
    if (filled > 0) {
        canvas.fill({track.x, track.y, filled, track.height}, color(quality));
    }
}

void LinkIndicator::release() {
    m_icon.reset();
}
} // namespace gdl::game
