#include <array>
#include <filesystem>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/hud/AutoSaveIcon.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("autosave rune animates only after success and expires across coarse updates",
          "[autosave]") {
    test::FakeRenderDevice device;
    AutoSaveIcon icon;
    const std::array<Image, 2> frames{Image::filled(32, 32, Color::rgba(0, 255, 0)),
                                      Image::filled(32, 32, Color::rgba(0, 128, 0))};
    REQUIRE(icon.load(device, frames));
    CHECK_FALSE(icon.visible());
    icon.render(device, {640, 480});
    CHECK(device.draws.empty());
    icon.show();
    CHECK(icon.visible());
    CHECK(icon.frame() == 0);
    icon.render(device, {640, 480});
    REQUIRE(device.draws.size() == 1);
    const auto* first = device.draws.back().texture;
    icon.update(AutoSaveIcon::kFrameSeconds);
    CHECK(icon.frame() == 1);
    icon.render(device, {640, 480});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws.back().texture != first);
    CHECK_FALSE(device.draws.back().state.depthTest);
    icon.update(AutoSaveIcon::kFrameSeconds);
    CHECK(icon.frame() == 0);
    icon.update(-1);
    icon.update(std::numeric_limits<f64>::infinity());
    CHECK(icon.frame() == 0);
    icon.show();
    icon.update(AutoSaveIcon::kDuration - AutoSaveIcon::kFrameSeconds);
    CHECK(icon.visible());
    icon.update(100);
    CHECK_FALSE(icon.visible());
    const usize draws = device.draws.size();
    icon.render(device, {640, 480});
    CHECK(device.draws.size() == draws);
    icon.show();
    icon.release();
    CHECK_FALSE(icon.visible());
    CHECK(icon.frameCount() == 0);
}

TEST_CASE("autosave rune rejects malformed frames and releases old artwork", "[autosave]") {
    test::FakeRenderDevice device;
    AutoSaveIcon icon;
    std::array<Image, 1> frames{Image::filled(32, 32, Color::white())};
    REQUIRE(icon.load(device, frames));
    icon.show();
    frames[0].pixels.clear();
    CHECK_FALSE(icon.load(device, frames));
    CHECK_FALSE(icon.visible());
    CHECK(icon.frameCount() == 0);
}

TEST_CASE("autosave notification uses every native disc-icon frame", "[autosave][assets]") {
    const auto root = test::assetOrSkip("INVENTORY/objects.ngc").parent_path().parent_path();
    if (!std::filesystem::exists(root.parent_path() / "carddemo/icon.tpl")) {
        SKIP("memory-card artwork is unavailable");
    }
    test::FakeRenderDevice device;
    AutoSaveIcon icon;
    REQUIRE(icon.load(device, root));
    REQUIRE(icon.frameCount() == 8);
    icon.show();
    for (usize index = 0; index < 8; ++index) {
        CHECK(icon.frame() == index);
        icon.render(device, {640, 480});
        REQUIRE(device.draws.back().texture->width() == 32);
        REQUIRE(device.draws.back().texture->height() == 32);
        icon.update(AutoSaveIcon::kFrameSeconds);
    }
    CHECK(icon.frame() == 0);
}
} // namespace
