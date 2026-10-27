#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "game/effects/ElementAura.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Sprite = ElementAura::Sprite;

constexpr std::array<Sprite, ElementAura::kSprites> kAll{
    Sprite::Glow,   Sprite::Ring,   Sprite::Streak, Sprite::Beam,  Sprite::Flame0,
    Sprite::Flame1, Sprite::Flame2, Sprite::Flame3, Sprite::Glint, Sprite::Drop};

struct Atlas {
    std::vector<u8> pixels = ElementAura::pixels();
    /** The alpha at `x`, `y` of `sprite`'s place. */
    u8 alpha(Sprite sprite, u32 x, u32 y) const {
        const auto at = ElementAura::place(sprite);
        return pixels[((static_cast<usize>(at.y) + y) * ElementAura::kWidth + at.x + x) * 4 + 3];
    }
    /** The red, green and blue there. */
    std::array<u8, 3> rgb(Sprite sprite, u32 x, u32 y) const {
        const auto at = ElementAura::place(sprite);
        const usize index = ((static_cast<usize>(at.y) + y) * ElementAura::kWidth + at.x + x) * 4;
        return {pixels[index], pixels[index + 1], pixels[index + 2]};
    }
};

TEST_CASE("the aura atlas places its sprites apart, within it, premultiplied, half a texel in",
          "[game][effects][aggression][elemental]") {
    const Atlas atlas;
    REQUIRE(atlas.pixels.size() ==
            static_cast<usize>(ElementAura::kWidth) * ElementAura::kHeight * 4);
    for (usize i = 0; i < atlas.pixels.size(); i += 4) {
        REQUIRE(atlas.pixels[i] <= atlas.pixels[i + 3]);
        REQUIRE(atlas.pixels[i + 1] <= atlas.pixels[i + 3]);
        REQUIRE(atlas.pixels[i + 2] <= atlas.pixels[i + 3]);
    }
    for (const Sprite sprite : kAll) {
        CAPTURE(static_cast<int>(sprite));
        const auto at = ElementAura::place(sprite);
        CHECK(at.w > 0);
        CHECK(at.h > 0);
        CHECK(at.x + at.w <= ElementAura::kWidth);
        CHECK(at.y + at.h <= ElementAura::kHeight);
        const auto cell = ElementAura::cell(sprite);
        CHECK(cell.u0 > static_cast<f32>(at.x) / ElementAura::kWidth);
        CHECK(cell.u1 < static_cast<f32>(at.x + at.w) / ElementAura::kWidth);
        CHECK(cell.v0 > static_cast<f32>(at.y) / ElementAura::kHeight);
        CHECK(cell.v1 < static_cast<f32>(at.y + at.h) / ElementAura::kHeight);
        for (const Sprite other : kAll) {
            if (other == sprite) {
                continue;
            }
            const auto b = ElementAura::place(other);
            const bool apart = at.x + at.w < b.x || b.x + b.w < at.x || at.y + at.h < b.y ||
                               b.y + b.h < at.y;
            CHECK(apart);
        }
    }
    CHECK(ElementAura::flame(0) == Sprite::Flame0);
    CHECK(ElementAura::flame(3) == Sprite::Flame3);
    CHECK(ElementAura::flame(5) == Sprite::Flame1);
}

TEST_CASE("each sprite has its shape: glow, ring, streak, beam, flames, glint and drop",
          "[game][effects][aggression][elemental]") {
    const Atlas atlas;
    // The glow: full at the centre, nothing at the corners.
    CHECK(atlas.alpha(Sprite::Glow, 16, 16) >= 250);
    CHECK(atlas.alpha(Sprite::Glow, 0, 0) == 0);
    CHECK(atlas.alpha(Sprite::Glow, 31, 31) == 0);
    // The ring: clear within, bright near the rim, nothing at the corners.
    CHECK(atlas.alpha(Sprite::Ring, 16, 16) < 20);
    CHECK(atlas.alpha(Sprite::Ring, 16, 4) >= 180);
    CHECK(atlas.alpha(Sprite::Ring, 0, 0) == 0);
    // The streak: bright at its base, gone at its tip and above its base.
    CHECK(atlas.alpha(Sprite::Streak, 1, 8) >= 200);
    CHECK(atlas.alpha(Sprite::Streak, 62, 8) <= 30);
    CHECK(atlas.alpha(Sprite::Streak, 1, 0) <= 10);
    // The beam: bright down its middle, gone at its edges.
    CHECK(atlas.alpha(Sprite::Beam, 8, 16) >= 240);
    CHECK(atlas.alpha(Sprite::Beam, 8, 0) <= 30);
    CHECK(atlas.alpha(Sprite::Beam, 8, 31) <= 30);
    // The flames: full and warm low in the middle, clear at every corner, each its own.
    for (u32 frame = 0; frame < ElementAura::kFlames; ++frame) {
        CAPTURE(frame);
        const Sprite sprite = ElementAura::flame(frame);
        CHECK(atlas.alpha(sprite, 16, 36) >= 200);
        const auto core = atlas.rgb(sprite, 16, 36);
        CHECK(core[0] >= core[1]);
        CHECK(core[1] >= core[2]);
        CHECK(core[0] > core[2] + 40);
        CHECK(atlas.alpha(sprite, 1, 1) == 0);
        CHECK(atlas.alpha(sprite, 30, 1) == 0);
        CHECK(atlas.alpha(sprite, 1, 46) == 0);
        CHECK(atlas.alpha(sprite, 30, 46) == 0);
        for (u32 other = 0; other < frame; ++other) {
            bool differs = false;
            for (u32 y = 0; y < 48 && !differs; ++y) {
                for (u32 x = 0; x < 32 && !differs; ++x) {
                    differs = atlas.alpha(sprite, x, y) !=
                              atlas.alpha(ElementAura::flame(other), x, y);
                }
            }
            CHECK(differs);
        }
    }
    // The glint: full at the centre, brighter along its arms than between them.
    CHECK(atlas.alpha(Sprite::Glint, 16, 16) >= 240);
    CHECK(atlas.alpha(Sprite::Glint, 26, 16) > atlas.alpha(Sprite::Glint, 23, 23));
    CHECK(atlas.alpha(Sprite::Glint, 0, 0) == 0);
    // The drop: round and full above, pointed to nothing below.
    CHECK(atlas.alpha(Sprite::Drop, 8, 8) >= 200);
    CHECK(atlas.alpha(Sprite::Drop, 1, 30) == 0);
    CHECK(atlas.alpha(Sprite::Drop, 14, 30) == 0);
    CHECK(atlas.alpha(Sprite::Drop, 8, 31) <= 40);
}

TEST_CASE("the aura atlas goes onto the device once and again after a release",
          "[game][effects][aggression][elemental]") {
    test::FakeRenderDevice device;
    ElementAura aura;
    const Texture* atlas = aura.texture(device);
    REQUIRE(atlas != nullptr);
    CHECK(aura.texture(device) == atlas);
    CHECK(device.texturesCreated == 1);
    CHECK(device.lastTextureDesc.width == ElementAura::kWidth);
    CHECK(device.lastTextureDesc.height == ElementAura::kHeight);
    aura.release();
    CHECK(aura.texture(device) != nullptr);
    CHECK(device.texturesCreated == 2);
}

} // namespace
