#include <algorithm>
#include <array>
#include <format>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/render/Image.h"

#include "TestSupport.h"
#include "game/scenes/MapPicture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A picture of upright bright stripes over dark ground, each row a little brighter in
 * turn, so a join reads as continuous only where it truly continues. */
Image striped(u32 width, u32 height) {
    Image image = Image::filled(width, height, Color::rgba(20, 30, 10));
    for (u32 y = 0; y < height; ++y) {
        const auto tone = static_cast<u8>(200 + (y % 7) * 3);
        for (u32 x = 0; x < width; ++x) {
            if ((x / 6) % 3 == 0) {
                image.setPixel(x, y, Color::rgba(tone, 220, 120));
            }
        }
    }
    return image;
}

/** The region of `image` from (left, top), `width` by `height`, edge texels extended. */
Image region(const Image& image, s32 left, s32 top, u32 width, u32 height) {
    Image out = Image::filled(width, height, Color::black());
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const auto sx = static_cast<u32>(
                std::clamp(left + static_cast<s32>(x), 0, static_cast<s32>(image.width) - 1));
            const auto sy = static_cast<u32>(
                std::clamp(top + static_cast<s32>(y), 0, static_cast<s32>(image.height) - 1));
            out.setPixel(x, y, image.pixel(sx, sy));
        }
    }
    return out;
}

/** The whole picture cut into the four tiles the maps ship as, two wide over two half-high,
 * the lower-left one cut `off` texels to the right and `off` rows up of its place, as the
 * Mothership's preview is. */
struct Cut {
    Image whole;
    std::array<Image, 4> tiles;
    std::array<MapPicture::Tile, 4> placed;
    Cut(u32 width, u32 height, u32 lower, s32 off) : whole(striped(width, height)) {
        const u32 half = width / 2;
        const u32 upper = height - lower;
        tiles = {region(whole, 0, 0, half, upper),
                 region(whole, static_cast<s32>(half), 0, half, upper),
                 region(whole, off, static_cast<s32>(upper) - off, half, lower),
                 region(whole, static_cast<s32>(half), static_cast<s32>(upper), half, lower)};
        placed = {{{tiles.data(), 0, 0},
                   {&tiles[1], half, 0},
                   {&tiles[2], 0, upper},
                   {&tiles[3], half, upper}}};
    }
};

TEST_CASE("map tiles join at their places into one picture", "[game][level-loading][map-picture]") {
    const Cut cut(64, 48, 16, 0);
    const Image joined = MapPicture::join(cut.placed);
    REQUIRE(joined.width == 64);
    REQUIRE(joined.height == 48);
    for (u32 y = 0; y < 48; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            REQUIRE(joined.pixel(x, y) == cut.whole.pixel(x, y));
        }
    }
    const std::array<MapPicture::Tile, 1> missing{{{nullptr, 0, 0}}};
    CHECK(MapPicture::join(missing).pixels.empty());
}

TEST_CASE("a tile cut off its place is slid back into line, continuous ones left alone",
          "[game][level-loading][map-picture]") {
    const Cut cut(128, 96, 32, 5);
    const auto slides = MapPicture::alignment(cut.placed);
    REQUIRE(slides.size() == 4);
    CAPTURE(slides[2].dx, slides[2].dy, slides[3].dx, slides[3].dy);
    CHECK(slides[0] == MapPicture::Slide{0, 0});
    CHECK(slides[1] == MapPicture::Slide{0, 0});
    CHECK(slides[2] == MapPicture::Slide{5, -5}); // five to the right and five up
    CHECK(slides[3] == MapPicture::Slide{0, 0});
    // Slid, the picture continues the whole where the tile covers it; what it leaves
    // bare at the picture's edge takes the nearest texel.
    const Image joined = MapPicture::join(cut.placed, slides);
    for (u32 y = 0; y < 91; ++y) {
        for (u32 x = 5; x < 128; ++x) {
            REQUIRE(joined.pixel(x, y) == cut.whole.pixel(x, y));
        }
    }
    CHECK(joined.pixel(0, 80) == cut.whole.pixel(5, 80));
    // Nothing moves in continuous art.
    const Cut whole(128, 96, 32, 0);
    for (const MapPicture::Slide& slide : MapPicture::alignment(whole.placed)) {
        CHECK(slide == MapPicture::Slide{0, 0});
    }
}

TEST_CASE("softening a join blends only the rows beside it", "[game][level-loading][map-picture]") {
    Image image = Image::filled(32, 32, Color::rgba(0, 0, 0));
    for (u32 y = 16; y < 32; ++y) {
        for (u32 x = 0; x < 32; ++x) {
            image.setPixel(x, y, Color::rgba(90, 90, 90));
        }
    }
    const Image lower = region(image, 0, 16, 32, 16);
    const Image upper = region(image, 0, 0, 32, 16);
    const std::array<MapPicture::Tile, 2> tiles{{{&upper, 0, 0}, {&lower, 0, 16}}};
    MapPicture::soften(image, tiles, {});
    CHECK(image.pixel(5, 13).r == 0);
    CHECK(image.pixel(5, 14).r == 0);
    CHECK(image.pixel(5, 15).r == 30); // (0 + 0 + 90) / 3
    CHECK(image.pixel(5, 16).r == 60); // (0 + 90 + 90) / 3
    CHECK(image.pixel(5, 17).r == 90);
    CHECK(image.pixel(5, 18).r == 90);
}

TEST_CASE("the seamless picture of cut tiles meets at every join",
          "[game][level-loading][map-picture]") {
    const Cut cut(128, 96, 32, 4);
    const Image picture = MapPicture::seamless(cut.placed);
    REQUIRE(picture.width == 128);
    REQUIRE(picture.height == 96);
    // Away from the softened bands and the bare left edge it is the whole picture.
    for (u32 y = 0; y < 60; ++y) {
        if (y >= 58) {
            continue;
        }
        for (u32 x = 8; x < 128; ++x) {
            if (x >= 62 && x < 66) {
                continue;
            }
            REQUIRE(picture.pixel(x, y) == cut.whole.pixel(x, y));
        }
    }
    // The lower-left tile left where it was cut does not meet the tiles above it.
    const Image upperOnly = MapPicture::join(std::array{cut.placed[0], cut.placed[1]});
    const auto plainCost = MapPicture::joinCost(upperOnly, cut.placed[2], 0, 64);
    const auto slidCost = MapPicture::joinCost(upperOnly, cut.placed[2], 4, 60);
    REQUIRE(plainCost);
    REQUIRE(slidCost);
    CHECK(slidCost->error < plainCost->error * MapPicture::kClearGain);
}

TEST_CASE("the Mothership preview's lower-left quarter is slid back into line",
          "[game][level-loading][map-picture][assets]") {
    const auto dir = test::unpackedOrSkip("MAPS/LEVELK1/textures.json").parent_path();
    TextureSet maps;
    REQUIRE(maps.load(dir));
    std::array<MapPicture::Tile, 4> placed{};
    for (usize i = 0; i < placed.size(); ++i) {
        const auto index = maps.find(std::format("LDMAP_K1_{:02}", i));
        REQUIRE(index);
        placed[i].image = &maps.image(*index);
        placed[i].x = i % 2 == 0 ? 0 : 256;
        placed[i].y = i < 2 ? 0 : 256;
    }
    const auto slides = MapPicture::alignment(placed);
    REQUIRE(slides.size() == 4);
    CAPTURE(slides[1].dx, slides[1].dy, slides[2].dx, slides[2].dy, slides[3].dx, slides[3].dy);
    // The lower-left quarter was cut five texels to the left of its place; the upper pair
    // meets as it is. (The two lower quarters also disagree by a few rows with each other,
    // so their join is a compromise, softened.)
    CHECK(slides[1] == MapPicture::Slide{0, 0});
    CHECK(slides[2].dx == 5);
    const Image upper = MapPicture::join(std::array{placed[0], placed[1]});
    const auto plain = MapPicture::joinCost(upper, placed[2], 0, 256);
    const auto slid =
        MapPicture::joinCost(upper, placed[2], 256 * 0 + slides[2].dx, 256 + slides[2].dy);
    REQUIRE(plain);
    REQUIRE(slid);
    CHECK(slid->error < plain->error * MapPicture::kClearGain);
    const Image picture = MapPicture::seamless(placed);
    REQUIRE(picture.width == 512);
    REQUIRE(picture.height == 384);
}

} // namespace
