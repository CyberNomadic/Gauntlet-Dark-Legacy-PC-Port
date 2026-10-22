#include <algorithm>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "game/hud/LinkIndicator.h"

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

namespace {
bool anyVertex(const test::FakeRenderDevice& device, Color color) {
    return std::ranges::any_of(device.draws, [&](const test::RecordedDraw& draw) {
        return std::ranges::any_of(draw.vertices,
                                   [&](const ImmediateVertex& v) { return v.color == color; });
    });
}
} // namespace

TEST_CASE("a link reads good under 60 ms, fair under 150 and poor beyond",
          "[hud][link-indicator]") {
    CHECK(LinkIndicator::quality(-1) == LinkQuality::Unknown);
    CHECK(LinkIndicator::quality(0) == LinkQuality::Good);
    CHECK(LinkIndicator::quality(59) == LinkQuality::Good);
    CHECK(LinkIndicator::quality(60) == LinkQuality::Fair);
    CHECK(LinkIndicator::quality(149) == LinkQuality::Fair);
    CHECK(LinkIndicator::quality(150) == LinkQuality::Poor);
    CHECK(LinkIndicator::quality(900) == LinkQuality::Poor);
}

TEST_CASE("each quality has its own colour and share of the bar", "[hud][link-indicator]") {
    CHECK(LinkIndicator::fill(LinkQuality::Good) == 1.0f);
    CHECK(LinkIndicator::fill(LinkQuality::Fair) == Approx(2.0f / 3.0f));
    CHECK(LinkIndicator::fill(LinkQuality::Poor) == Approx(1.0f / 3.0f));
    CHECK(LinkIndicator::fill(LinkQuality::Unknown) == 0.0f);
    const auto good = LinkIndicator::color(LinkQuality::Good);
    const auto fair = LinkIndicator::color(LinkQuality::Fair);
    const auto poor = LinkIndicator::color(LinkQuality::Poor);
    CHECK(good.g > good.r);
    CHECK(fair.r > fair.b);
    CHECK(fair.g > fair.b);
    CHECK(poor.r > poor.g);
    CHECK_FALSE(good == fair);
    CHECK_FALSE(fair == poor);
}

TEST_CASE("the icon is a symmetric broadcast glyph with a clear corner", "[hud][link-indicator]") {
    const auto image = LinkIndicator::icon();
    REQUIRE(image.width == 11);
    REQUIRE(image.height == 7);
    REQUIRE(image.pixels.size() == usize{11} * 7 * 4);
    CHECK(image.pixels[3] == 0); // the top-left corner is clear
    const auto alphaAt = [&](u32 x, u32 y) { return image.pixels[(y * image.width + x) * 4 + 3]; };
    CHECK(alphaAt(5, 3) == 255); // the dot
    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = 0; x < image.width; ++x) {
            CHECK(alphaAt(x, y) == alphaAt(image.width - 1 - x, y));
        }
    }
    CHECK(LinkIndicator::iconSize() == Vec2{22, 14});
}

TEST_CASE("drawing shows the icon once and fills the bar by quality", "[hud][link-indicator]") {
    test::FakeRenderDevice device;
    LinkIndicator indicator;
    const Rect area{100, 6, 70, 14};
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    indicator.draw(canvas, device, area, LinkQuality::Good);
    canvas.end();
    CHECK(device.texturesCreated == 1);
    const auto icon = std::ranges::find_if(device.draws, [&](const test::RecordedDraw& draw) {
        return draw.texture != &device.whiteTexture();
    });
    REQUIRE(icon != device.draws.end());
    CHECK(icon->texture->width() == 11);
    CHECK(icon->texture->height() == 7);
    CHECK(anyVertex(device, LinkIndicator::color(LinkQuality::Good)));
    CHECK_FALSE(anyVertex(device, LinkIndicator::color(LinkQuality::Poor)));

    device.draws.clear();
    canvas.begin(device, Mat4{1});
    indicator.draw(canvas, device, area, LinkQuality::Unknown);
    canvas.end();
    CHECK(device.texturesCreated == 1); // the icon is kept between frames
    CHECK_FALSE(anyVertex(device, LinkIndicator::color(LinkQuality::Unknown)));
    CHECK(anyVertex(device, LinkIndicator::color(LinkQuality::Good)) == false);

    indicator.release();
    canvas.begin(device, Mat4{1});
    indicator.draw(canvas, device, area, LinkQuality::Poor);
    canvas.end();
    CHECK(device.texturesCreated == 2);
    CHECK(anyVertex(device, LinkIndicator::color(LinkQuality::Poor)));
}
