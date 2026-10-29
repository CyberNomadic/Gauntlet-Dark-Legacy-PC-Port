#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/LegendsN64Texture.h"
#include "game/bosses/LegendsBosses.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct ArenaFixture {
    test::FakeRenderDevice device;
    LegendsBossEncounter encounter;
    std::unique_ptr<LegendsSceneAdapter> scene;

    explicit ArenaFixture(std::string_view name) {
        const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
        const auto directory = root / "levels" / name;
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Native N64 Legends extraction is not installed");
        }
        const auto decoded = legendsN64Boss(name, readFile(root / "_program/game.bin"),
                                            readFile(root / "_program/Boss.bin"));
        REQUIRE(decoded);
        encounter = *decoded;
        const auto models = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                                 readFile(directory / "ipr.rom"));
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(models, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    }

    std::vector<ImmediateVertex> geometryNear(Vec3 centre) {
        device.draws.clear();
        const auto camera = CameraFrame::of(WorldCamera{});
        scene->draw(device, Mat4{1}, camera, TreeModel::Pass::Opaque);
        scene->draw(device, Mat4{1}, camera, TreeModel::Pass::Blended);
        std::vector<ImmediateVertex> vertices;
        for (const auto& draw : device.draws) {
            for (const auto& vertex : draw.vertices) {
                if (glm::distance(Vec2{vertex.position.x, vertex.position.z},
                                  Vec2{centre.x, centre.z}) < 5) {
                    vertices.push_back(vertex);
                }
            }
        }
        return vertices;
    }
};
} // namespace

TEST_CASE("Native boss cover uses authored coordinates, tiers and damageable projectile bodies",
          "[legends][arena][assets]") {
    std::string_view stage = "mount6";
    usize count = 6;
    s32 tierHealth = 40;
    SECTION("Dragon cover") {}
    SECTION("Plague cover") {
        stage = "town5";
        count = 3;
        tierHealth = 80;
    }
    ArenaFixture fixture{stage};
    auto& scene = *fixture.scene;
    const auto anchors = scene.arenaAnchors();
    const auto targets = scene.arenaTargets();
    REQUIRE(targets.size() == count);
    REQUIRE(anchors.size() == count);
    REQUIRE(scene.arenaObstacles().size() == count);
    auto stops = scene.arenaMissileStops();
    REQUIRE(stops.size() == count);
    for (usize index = 0; index < count; ++index) {
        const auto& native = fixture.encounter.combat.arenaRocks[index];
        CHECK(targets[index].index == index);
        CHECK(targets[index].active);
        CHECK(Vec3{targets[index].placement[3]} == native.position);
        CHECK(anchors[index] == targets[index].placement);
        CHECK(stops[index].rock == 16384 + static_cast<s32>(index));
        CHECK(stops[index].rockHealth == tierHealth * 3);
        CHECK(stops[index].rockArmor == 0);
        CHECK(stops[index].box.centre == native.position);
        CHECK(stops[index].box.cylinderRadius == Approx(2.3f));
        CHECK(stops[index].box.height == Approx(5));
    }

    const Vec3 centre = fixture.encounter.combat.arenaRocks[0].position;
    const Vec3 from = centre + Vec3{-4, 2.5f, 0};
    const Vec3 to = centre + Vec3{4, 2.5f, 0};
    REQUIRE(scene.arenaBlocksSegment(from, to, .1f));
    const auto whole = fixture.geometryNear(centre);
    REQUIRE_FALSE(whole.empty());
    const auto allocations = fixture.device.texturesCreated;
    CHECK_FALSE(scene.strikeArenaRock(count, 1));
    CHECK_FALSE(scene.strikeArenaRock(0, -1));
    CHECK_FALSE(scene.strikeArenaRock(0, std::numeric_limits<f32>::infinity()));
    CHECK_FALSE(scene.strikeArenaRock(0, std::numeric_limits<f32>::quiet_NaN()));
    REQUIRE(scene.strikeArenaRock(0, static_cast<f32>(tierHealth)));
    CHECK(scene.arenaMissileStops()[0].rockHealth == tierHealth * 2);
    const auto damaged = fixture.geometryNear(centre);
    REQUIRE_FALSE(damaged.empty());
    CHECK(damaged != whole);
    REQUIRE(scene.strikeArenaRock(0, static_cast<f32>(tierHealth)));
    CHECK(scene.arenaMissileStops()[0].rockHealth == tierHealth);
    const auto low = fixture.geometryNear(centre);
    REQUIRE_FALSE(low.empty());
    CHECK(low != damaged);
    REQUIRE(scene.strikeArenaRock(0, static_cast<f32>(tierHealth)));
    CHECK_FALSE(scene.strikeArenaRock(0, 1));
    CHECK_FALSE(scene.arenaTargets()[0].active);
    CHECK_FALSE(scene.arenaBlocksSegment(from, to, .1f));
    CHECK(scene.arenaObstacles().size() == count - 1);
    stops = scene.arenaMissileStops();
    REQUIRE(stops.size() == count - 1);
    CHECK(stops.front().rock == 16385);     // Stable roster IDs do not compact after destruction.
    CHECK(scene.arenaAnchors() == anchors); // Boss effects retain destroyed cover anchors.
    for (usize tick = 0; tick < 180; ++tick) {
        scene.update(1.0f / 60);
    }
    CHECK_FALSE(scene.arenaTargets()[0].active);
    CHECK(fixture.device.texturesCreated == allocations);
}

TEST_CASE("Imported Yeti eruptions activate native anchors without borrowing GDL stage coordinates",
          "[legends][arena][assets]") {
    ArenaFixture fixture{"ice5"};
    auto& scene = *fixture.scene;
    const auto anchors = scene.arenaAnchors();
    REQUIRE(anchors.size() == 8);
    REQUIRE(scene.arenaTargets().size() == 8);
    REQUIRE(scene.arenaObstacles().empty());
    REQUIRE(scene.arenaMissileStops().empty());
    CHECK(std::ranges::none_of(scene.arenaTargets(), &CombatArenaTarget::active));
    CHECK_FALSE(scene.strikeArenaRock(3, 1));
    const auto allocations = fixture.device.texturesCreated;
    scene.scheduleArenaActivation(3, .5f);
    scene.update(.25f);
    CHECK_FALSE(scene.arenaTargets()[3].active);
    scene.update(.25f);
    REQUIRE(scene.arenaTargets()[3].active);
    auto stops = scene.arenaMissileStops();
    REQUIRE(stops.size() == 1);
    CHECK(stops[0].rock == 16387);
    CHECK(stops[0].rockHealth == 120);
    CHECK(stops[0].box.centre == fixture.encounter.combat.arenaRocks[3].position);
    REQUIRE_FALSE(fixture.geometryNear(stops[0].box.centre).empty());
    REQUIRE(scene.strikeArenaRock(3, 120));
    CHECK_FALSE(scene.arenaTargets()[3].active);
    scene.scheduleArenaActivation(3, .5f);
    scene.update(.5f);
    REQUIRE(scene.arenaTargets()[3].active);
    REQUIRE(scene.arenaMissileStops().size() == 1);
    CHECK(scene.arenaMissileStops()[0].rockHealth == 120);
    CHECK(scene.arenaAnchors() == anchors);
    CHECK(fixture.device.texturesCreated == allocations);
}
