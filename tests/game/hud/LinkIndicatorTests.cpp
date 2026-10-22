#include <algorithm>
#include <functional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "game/hud/LinkIndicator.h"

using namespace gdl;
using namespace gdl::game;

namespace {
usize quadsOf(const test::FakeRenderDevice& device, Color color) {
    usize vertices = 0;
    for (const auto& draw : device.draws) {
        vertices += static_cast<usize>(std::ranges::count_if(
            draw.vertices, [&](const ImmediateVertex& v) { return v.color == color; }));
    }
    return vertices / 6; // a bar is two triangles
}

void drawOnce(test::FakeRenderDevice& device, LinkQuality quality) {
    device.draws.clear();
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    LinkIndicator::draw(canvas, {100, 6}, quality);
    canvas.end();
}
} // namespace

TEST_CASE("a link reads good under 60 ms, fair under 120, weak under 200 and poor beyond",
          "[hud][link-indicator]") {
    CHECK(LinkIndicator::quality(-1) == LinkQuality::Unknown);
    CHECK(LinkIndicator::quality(0) == LinkQuality::Good);
    CHECK(LinkIndicator::quality(59) == LinkQuality::Good);
    CHECK(LinkIndicator::quality(60) == LinkQuality::Fair);
    CHECK(LinkIndicator::quality(119) == LinkQuality::Fair);
    CHECK(LinkIndicator::quality(120) == LinkQuality::Weak);
    CHECK(LinkIndicator::quality(199) == LinkQuality::Weak);
    CHECK(LinkIndicator::quality(200) == LinkQuality::Poor);
    CHECK(LinkIndicator::quality(900) == LinkQuality::Poor);
}

TEST_CASE("each quality lights its own number of bars in its own colour", "[hud][link-indicator]") {
    CHECK(LinkIndicator::lit(LinkQuality::Good) == 4);
    CHECK(LinkIndicator::lit(LinkQuality::Fair) == 3);
    CHECK(LinkIndicator::lit(LinkQuality::Weak) == 2);
    CHECK(LinkIndicator::lit(LinkQuality::Poor) == 1);
    CHECK(LinkIndicator::lit(LinkQuality::Unknown) == 0);
    const auto good = LinkIndicator::color(LinkQuality::Good);
    const auto fair = LinkIndicator::color(LinkQuality::Fair);
    const auto weak = LinkIndicator::color(LinkQuality::Weak);
    const auto poor = LinkIndicator::color(LinkQuality::Poor);
    CHECK(good.g > good.r);
    CHECK(fair.r > fair.b);
    CHECK(fair.g > fair.b);
    CHECK(weak.r > weak.g); // orange: between yellow and red
    CHECK(weak.g > weak.b);
    CHECK(weak.g < fair.g);
    CHECK(weak.g > poor.g);
    CHECK(poor.r > poor.g);
    CHECK_FALSE(good == fair);
    CHECK_FALSE(fair == weak);
    CHECK_FALSE(weak == poor);
    CHECK(LinkIndicator::color(LinkQuality::Unknown) == LinkIndicator::unlit());
    CHECK(LinkIndicator::size() == Vec2{22, 14});
}

TEST_CASE("the mark is four rising bars, lit from the shortest up", "[hud][link-indicator]") {
    test::FakeRenderDevice device;
    drawOnce(device, LinkQuality::Good);
    CHECK(device.texturesCreated == 0);
    CHECK(quadsOf(device, LinkIndicator::color(LinkQuality::Good)) == 4);
    CHECK(quadsOf(device, LinkIndicator::unlit()) == 0);
    std::vector<f32> tops;
    std::vector<f32> bottoms;
    for (const auto& draw : device.draws) {
        for (usize i = 0; i + 5 < draw.vertices.size(); i += 6) {
            f32 top = draw.vertices[i].position.y;
            f32 bottom = top;
            for (usize j = i; j < i + 6; ++j) {
                top = std::min(top, draw.vertices[j].position.y);
                bottom = std::max(bottom, draw.vertices[j].position.y);
            }
            tops.push_back(top);
            bottoms.push_back(bottom);
        }
    }
    REQUIRE(tops.size() == 4);
    CHECK(std::ranges::is_sorted(tops, std::greater<>{})); // each bar rises over the last
    CHECK(std::ranges::all_of(bottoms, [&](f32 b) { return b == bottoms.front(); }));
    CHECK(bottoms.front() == 6 + LinkIndicator::size().y);

    drawOnce(device, LinkQuality::Fair);
    CHECK(quadsOf(device, LinkIndicator::color(LinkQuality::Fair)) == 3);
    CHECK(quadsOf(device, LinkIndicator::unlit()) == 1);

    drawOnce(device, LinkQuality::Weak);
    CHECK(quadsOf(device, LinkIndicator::color(LinkQuality::Weak)) == 2);
    CHECK(quadsOf(device, LinkIndicator::unlit()) == 2);

    drawOnce(device, LinkQuality::Poor);
    CHECK(quadsOf(device, LinkIndicator::color(LinkQuality::Poor)) == 1);
    CHECK(quadsOf(device, LinkIndicator::unlit()) == 3);

    drawOnce(device, LinkQuality::Unknown);
    CHECK(quadsOf(device, LinkIndicator::unlit()) == 4);
    CHECK(quadsOf(device, LinkIndicator::color(LinkQuality::Good)) == 0);
}
