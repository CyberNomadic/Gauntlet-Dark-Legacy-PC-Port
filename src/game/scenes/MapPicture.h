#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/Image.h"

namespace gdl::game {

/**
 * The loading screen's map and stage preview as one picture: four tiles joined at their
 * places, each slid into line with the tiles before it where the art was cut with an
 * offset (the Mothership's preview has its lower-left quarter five texels to one side and
 * five rows down, which the original shows as a break in the runway), and the joins
 * softened so no hard line is left. Presentation only; the original draws the tiles as
 * they are.
 */
class MapPicture {
public:
    static constexpr s32 kMostSlide = 8;     ///< texels a tile may be slid either way
    static constexpr f32 kClearGain = 0.85f; ///< a slide must leave this much of the plain join's
                                             ///< difference, or the art is taken as continuous
    static constexpr u32 kSoftRows = 2;      ///< rows (or columns) blended on each side of a join
    static constexpr u32 kMargin = 16;       ///< texels at the ends of a join left out of measuring
    static constexpr u32 kLeastMeasured = 32; ///< texels a join must offer to be measured
    /** A slide may leave no more than this share of the texels measured in place
     * unmeasured: a tile never slides away from a neighbour. */
    static constexpr f32 kKeptJoin = 0.9f;

    struct Tile {
        const Image* image = nullptr;
        u32 x = 0;
        u32 y = 0;
    };
    struct Slide {
        s32 dx = 0;
        s32 dy = 0;
        bool operator==(const Slide&) const = default;
    };

    struct Join {
        /** How far the tile is from continuing what the sheet holds: where it lies over
         * texels already there they should match; across its top and left edges the next
         * line should differ about as much as the tile's own neighbouring lines do (a
         * duplicated line not at all, a misaligned one far more). Mean per texel. */
        f32 error = 0;
        u32 count = 0; ///< how many texels were measured
    };
    /** How `tile`, placed with its corner at (px, py) over `sheet`, meets what the sheet
     * already holds under and beside it, or nothing with too few texels to measure. */
    static std::optional<Join> joinCost(const Image& sheet, const Tile& tile, s32 px, s32 py);
    /** Each tile's slide from its place that best continues the tiles before it, the first
     * never moved; none unless clearly better than staying. */
    static std::vector<Slide> alignment(std::span<const Tile> tiles);
    /** The tiles at their places plus their slides, later tiles over earlier, every texel
     * no tile covers taking the nearest covered one; empty without every tile. */
    static Image join(std::span<const Tile> tiles, std::span<const Slide> slides);
    static Image join(std::span<const Tile> tiles);
    /** Blends the texels on each side of every later tile's top and left edge. */
    static void soften(Image& sheet, std::span<const Tile> tiles, std::span<const Slide> slides);
    /** The whole treatment: aligned, joined and softened. */
    static Image seamless(std::span<const Tile> tiles);
};

} // namespace gdl::game
