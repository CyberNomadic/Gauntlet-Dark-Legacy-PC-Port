#include "game/scenes/MapPicture.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

/** The sum of the channel differences between two texels. */
u32 apart(Color a, Color b) {
    return static_cast<u32>(std::abs(s32{a.r} - s32{b.r})) +
           static_cast<u32>(std::abs(s32{a.g} - s32{b.g})) +
           static_cast<u32>(std::abs(s32{a.b} - s32{b.b}));
}

bool inside(const Image& image, s32 x, s32 y) {
    return x >= 0 && y >= 0 && x < static_cast<s32>(image.width) &&
           y < static_cast<s32>(image.height);
}

bool covered(const Image& sheet, s32 x, s32 y) {
    return inside(sheet, x, y) && sheet.pixel(static_cast<u32>(x), static_cast<u32>(y)).a != 0;
}

/** The size the tiles take at their places. */
std::pair<u32, u32> boundsOf(std::span<const MapPicture::Tile> tiles) {
    u32 width = 0;
    u32 height = 0;
    for (const MapPicture::Tile& tile : tiles) {
        width = std::max(width, tile.x + tile.image->width);
        height = std::max(height, tile.y + tile.image->height);
    }
    return {width, height};
}

void paste(Image& sheet, const MapPicture::Tile& tile, s32 px, s32 py) {
    for (u32 v = 0; v < tile.image->height; ++v) {
        const s32 y = py + static_cast<s32>(v);
        if (y < 0 || y >= static_cast<s32>(sheet.height)) {
            continue;
        }
        for (u32 u = 0; u < tile.image->width; ++u) {
            const s32 x = px + static_cast<s32>(u);
            if (x < 0 || x >= static_cast<s32>(sheet.width)) {
                continue;
            }
            Color texel = tile.image->pixel(u, v);
            texel.a = std::max<u8>(texel.a, 1); // covered, however clear the art
            sheet.setPixel(static_cast<u32>(x), static_cast<u32>(y), texel);
        }
    }
}

/** Every texel no tile covers takes the nearest covered one along its row, or the row above
 * or below when the whole row is bare. */
void fillBare(Image& sheet) {
    std::vector<bool> rowCovered(sheet.height, false);
    for (u32 y = 0; y < sheet.height; ++y) {
        s32 last = -1;
        for (u32 x = 0; x < sheet.width; ++x) {
            if (sheet.pixel(x, y).a != 0) {
                rowCovered[y] = true;
                if (last >= 0 && static_cast<u32>(last) + 1 < x) {
                    // Bare texels between two covered ones take the nearer.
                    for (u32 bare = static_cast<u32>(last) + 1; bare < x; ++bare) {
                        const bool nearerLeft = bare - static_cast<u32>(last) <= x - bare;
                        sheet.setPixel(bare, y, sheet.pixel(nearerLeft ? static_cast<u32>(last) : x, y));
                    }
                } else if (last < 0) {
                    for (u32 bare = 0; bare < x; ++bare) {
                        sheet.setPixel(bare, y, sheet.pixel(x, y));
                    }
                }
                last = static_cast<s32>(x);
            }
        }
        if (last >= 0) {
            for (u32 bare = static_cast<u32>(last) + 1; bare < sheet.width; ++bare) {
                sheet.setPixel(bare, y, sheet.pixel(static_cast<u32>(last), y));
            }
        }
    }
    // A wholly bare row takes the nearest covered row.
    for (u32 y = 0; y < sheet.height; ++y) {
        if (rowCovered[y]) {
            continue;
        }
        std::optional<u32> source;
        for (u32 reach = 1; reach < sheet.height && !source; ++reach) {
            if (y >= reach && rowCovered[y - reach]) {
                source = y - reach;
            } else if (y + reach < sheet.height && rowCovered[y + reach]) {
                source = y + reach;
            }
        }
        if (source) {
            std::ranges::copy(sheet.row(*source), sheet.row(y).begin());
        }
    }
}

Color averaged(const Image& image, u32 x, u32 y, bool vertical) {
    u32 r = 0;
    u32 g = 0;
    u32 b = 0;
    u32 count = 0;
    for (s32 step = -1; step <= 1; ++step) {
        const s32 sx = vertical ? static_cast<s32>(x) : static_cast<s32>(x) + step;
        const s32 sy = vertical ? static_cast<s32>(y) + step : static_cast<s32>(y);
        if (!inside(image, sx, sy)) {
            continue;
        }
        const Color c = image.pixel(static_cast<u32>(sx), static_cast<u32>(sy));
        r += c.r;
        g += c.g;
        b += c.b;
        ++count;
    }
    const Color own = image.pixel(x, y);
    if (count == 0) {
        return own;
    }
    return Color{static_cast<u8>(r / count), static_cast<u8>(g / count),
                 static_cast<u8>(b / count), own.a};
}

/** Blends the lines on each side of a join: a horizontal one at row `at` over columns
 * [from, to), or a vertical one at column `at` over rows [from, to). */
void softenLine(Image& sheet, const Image& source, bool horizontalJoin, u32 at, u32 from, u32 to) {
    const u32 length = horizontalJoin ? sheet.height : sheet.width;
    if (at == 0 || at >= length) {
        return;
    }
    const u32 first = at >= MapPicture::kSoftRows ? at - MapPicture::kSoftRows : 0;
    const u32 last = std::min(at + MapPicture::kSoftRows, length);
    const u32 end = std::min(to, horizontalJoin ? sheet.width : sheet.height);
    for (u32 line = first; line < last; ++line) {
        for (u32 i = from; i < end; ++i) {
            const u32 x = horizontalJoin ? i : line;
            const u32 y = horizontalJoin ? line : i;
            sheet.setPixel(x, y, averaged(source, x, y, horizontalJoin));
        }
    }
}

} // namespace

std::optional<MapPicture::Join> MapPicture::joinCost(const Image& sheet, const Tile& tile, s32 px,
                                                     s32 py) {
    if (tile.image == nullptr) {
        return std::nullopt;
    }
    const Image& art = *tile.image;
    const auto width = static_cast<s32>(art.width);
    const auto height = static_cast<s32>(art.height);
    const auto margin = static_cast<s32>(kMargin);
    // Where the tile lies over texels the sheet already holds (its first rows and columns,
    // when slid up or left into a neighbour) they should be the same texels.
    u64 overlap = 0;
    u32 overlapped = 0;
    const s32 reach = kMostSlide + 1;
    for (s32 v = 0; v < std::min(height, reach); ++v) {
        for (s32 u = margin; u + margin < width; ++u) {
            if (covered(sheet, px + u, py + v)) {
                overlap += apart(sheet.pixel(static_cast<u32>(px + u), static_cast<u32>(py + v)),
                                 art.pixel(static_cast<u32>(u), static_cast<u32>(v)));
                ++overlapped;
            }
        }
    }
    for (s32 u = 0; u < std::min(width, reach); ++u) {
        for (s32 v = std::max(margin, reach); v + margin < height; ++v) {
            if (covered(sheet, px + u, py + v)) {
                overlap += apart(sheet.pixel(static_cast<u32>(px + u), static_cast<u32>(py + v)),
                                 art.pixel(static_cast<u32>(u), static_cast<u32>(v)));
                ++overlapped;
            }
        }
    }
    // Across the top and left edges the sheet's line just beyond should differ from the
    // tile's first line about as much as that line differs from the tile's second.
    u64 across = 0;
    u64 within = 0;
    u32 edged = 0;
    for (s32 u = margin; u + margin < width; ++u) {
        if (covered(sheet, px + u, py - 1)) {
            const auto tu = static_cast<u32>(u);
            across += apart(sheet.pixel(static_cast<u32>(px + u), static_cast<u32>(py - 1)),
                            art.pixel(tu, 0));
            within += apart(art.pixel(tu, 0), art.pixel(tu, std::min(1U, art.height - 1)));
            ++edged;
        }
    }
    for (s32 v = margin; v + margin < height; ++v) {
        if (covered(sheet, px - 1, py + v)) {
            const auto tv = static_cast<u32>(v);
            across += apart(sheet.pixel(static_cast<u32>(px - 1), static_cast<u32>(py + v)),
                            art.pixel(0, tv));
            within += apart(art.pixel(0, tv), art.pixel(std::min(1U, art.width - 1), tv));
            ++edged;
        }
    }
    const u32 count = overlapped + edged;
    if (count < kLeastMeasured) {
        return std::nullopt;
    }
    const u64 strayed = across > within ? across - within : within - across;
    return Join{static_cast<f32>(overlap + strayed) / static_cast<f32>(count), count};
}

std::vector<MapPicture::Slide> MapPicture::alignment(std::span<const Tile> tiles) {
    std::vector<Slide> slides(tiles.size());
    if (tiles.empty() || std::ranges::any_of(tiles, [](const Tile& t) { return t.image == nullptr; })) {
        return slides;
    }
    const auto [width, height] = boundsOf(tiles);
    Image sheet = Image::filled(width, height, Color::rgba(0, 0, 0, 0));
    paste(sheet, tiles[0], static_cast<s32>(tiles[0].x), static_cast<s32>(tiles[0].y));
    for (usize k = 1; k < tiles.size(); ++k) {
        const Tile& tile = tiles[k];
        const auto x = static_cast<s32>(tile.x);
        const auto y = static_cast<s32>(tile.y);
        if (const auto staying = joinCost(sheet, tile, x, y)) {
            Slide best;
            f32 least = staying->error;
            const auto kept = static_cast<u32>(static_cast<f32>(staying->count) * kKeptJoin);
            for (s32 dy = -kMostSlide; dy <= kMostSlide; ++dy) {
                for (s32 dx = -kMostSlide; dx <= kMostSlide; ++dx) {
                    const auto join = joinCost(sheet, tile, x + dx, y + dy);
                    if (!join || join->count < kept) {
                        continue; // away from a neighbour is no better, however it reads
                    }
                    const bool nearer =
                        std::abs(dx) + std::abs(dy) < std::abs(best.dx) + std::abs(best.dy);
                    if (join->error < least || (join->error == least && nearer)) {
                        least = join->error;
                        best = Slide{dx, dy};
                    }
                }
            }
            if (least < staying->error * kClearGain) {
                slides[k] = best;
            }
        }
        paste(sheet, tile, x + slides[k].dx, y + slides[k].dy);
    }
    return slides;
}

Image MapPicture::join(std::span<const Tile> tiles, std::span<const Slide> slides) {
    if (tiles.empty() || std::ranges::any_of(tiles, [](const Tile& t) { return t.image == nullptr; })) {
        return {};
    }
    const auto [width, height] = boundsOf(tiles);
    if (width == 0 || height == 0) {
        return {};
    }
    Image sheet = Image::filled(width, height, Color::rgba(0, 0, 0, 0));
    for (usize k = 0; k < tiles.size(); ++k) {
        const Slide slide = k < slides.size() ? slides[k] : Slide{};
        paste(sheet, tiles[k], static_cast<s32>(tiles[k].x) + slide.dx,
              static_cast<s32>(tiles[k].y) + slide.dy);
    }
    fillBare(sheet);
    return sheet;
}

Image MapPicture::join(std::span<const Tile> tiles) {
    return join(tiles, {});
}

void MapPicture::soften(Image& sheet, std::span<const Tile> tiles, std::span<const Slide> slides) {
    const Image source = sheet;
    for (usize k = 1; k < tiles.size(); ++k) {
        const Slide slide = k < slides.size() ? slides[k] : Slide{};
        const s32 px = static_cast<s32>(tiles[k].x) + slide.dx;
        const s32 py = static_cast<s32>(tiles[k].y) + slide.dy;
        const auto left = static_cast<u32>(std::max(px, 0));
        const auto top = static_cast<u32>(std::max(py, 0));
        if (py > 0) {
            softenLine(sheet, source, true, static_cast<u32>(py), left,
                       left + tiles[k].image->width);
        }
        if (px > 0) {
            softenLine(sheet, source, false, static_cast<u32>(px), top,
                       top + tiles[k].image->height);
        }
    }
}

Image MapPicture::seamless(std::span<const Tile> tiles) {
    const std::vector<Slide> slides = alignment(tiles);
    Image sheet = join(tiles, slides);
    if (!sheet.pixels.empty()) {
        soften(sheet, tiles, slides);
    }
    return sheet;
}

} // namespace gdl::game
