#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/LevelDebugOverlay.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/party/PlayerRuntime.h"

namespace {
using namespace gdl;
using namespace gdl::game;

bool mentions(std::span<const std::string> lines, std::string_view text) {
    return std::ranges::any_of(lines, [&](const std::string& line) { return line.contains(text); });
}

TEST_CASE("the level debug overlay names the level, the players, the camera and the "
          "surroundings",
          "[game][level-debug][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("A1")));
    REQUIRE(world.startPoint(0));
    std::array<PlayerRuntime, 2> players;
    const Vec3 start = world.startPoint(0)->position;
    players[0].actor.spawn(0, {}, nullptr, start, 0);
    players[1].life = PlayerLife::InTower;
    TowerCamera::State camera;
    camera.camera.position = start + Vec3{0, 20, -20};
    camera.camera.pitch = 0.79f;
    camera.distance = 24.0f;
    camera.marker = 0;
    camera.aimedMarker = 0;
    const auto lines = LevelDebugOverlay::describe(world, players, camera, 3);
    REQUIRE(lines.size() >= 5);
    CHECK(lines[0].starts_with("Level A1"));
    CHECK(mentions(lines, "Player 1: at"));
    CHECK_FALSE(mentions(lines, "Player 2"));
    CHECK(mentions(lines, std::format("{:.2f}", start.x)));
    CHECK(mentions(lines, "Camera: at"));
    CHECK(mentions(lines, std::format("Marker 0 of {}", world.cameraMarkers().size())));
    CHECK(mentions(lines, "Nearest objects:"));
    CHECK(mentions(lines, "Enemies alive 3"));
    // Nothing is drawn while hidden, and the overlay never touches gameplay state.
    LevelDebugOverlay overlay;
    device.draws.clear();
    overlay.render(device, Extent2D{1280, 720}, lines);
    CHECK(device.draws.empty());
    overlay.toggle();
    CHECK(overlay.visible());
    overlay.release();
}

} // namespace
