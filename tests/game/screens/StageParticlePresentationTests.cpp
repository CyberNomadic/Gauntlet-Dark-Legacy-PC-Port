#include <algorithm>
#include <array>
#include <limits>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelFixtures.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/StageParticlePresentation.h"
#include "game/world/EffectTrees.h"
#include "game/world/ExitPortals.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = StageParticlePresentation::Phase;

void sameDraws(const std::vector<test::RecordedDraw>& actual,
               const std::vector<test::RecordedDraw>& expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize i = 0; i < actual.size(); ++i) {
        CAPTURE(i);
        CHECK(actual[i].texture == expected[i].texture);
        CHECK(actual[i].vertices == expected[i].vertices);
        CHECK(actual[i].transform == expected[i].transform);
        CHECK(actual[i].state.blend == expected[i].state.blend);
        CHECK(actual[i].state.depthWrite == expected[i].state.depthWrite);
        CHECK(actual[i].state.depthTest == expected[i].state.depthTest);
        CHECK(actual[i].state.nextTexture == expected[i].state.nextTexture);
        CHECK(actual[i].state.textureBlend == expected[i].state.textureBlend);
    }
}

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    LevelWorld world;
    EffectTrees projectiles, effects;
    ItemFigure figure;
    ExitPortals portals;
    LevelCatalog catalog;
    std::shared_ptr<const void> lifetime = std::make_shared<const u8>(0);
    u32 effect = 0;

    Fixture() {
        const auto root = test::sampleLevel("stage-particle-presentation");
        std::filesystem::create_directories(root / "wdata");
        writeTextFile(root / "wdata/SECRET.json", R"({"realm":12,"prefix":"levelS",
          "levels":[{"name":"S1","title":"Secret"}]})");
        writeTextFile(root / "world.json", R"({
          "objects":[{"name":"PSYSA","position":[1,2,3],"flags":2048,"next":1},
                     {"name":"FLOOR","position":[0,0,0],"flags":4}],
          "particles":[{"id":"A","preset":2,"enables":16929,"texture":"STONE",
            "particleLife":[0.5,0],"rate":[30,30,30,30]}],
          "itemInfos":[{"type":1,"subtype":1,"name":"PICKUP","value":5,"radius":1,"height":2},
            {"type":1,"subtype":15,"name":"GEMORANGE","value":4,"radius":1,"height":2},
            {"type":9,"subtype":50,"name":"PICKUP","radius":1}],
          "itemInstances":[{"info":0,"position":[0,0,0]},
            {"info":1,"position":[20,0,0]},
            {"info":2,"position":[10,0,0],"params":[0,0,0,0,83,49,0,0,0,0,0,0]}]})");
        writeTextFile(root / "animations.json", R"({
          "particles":[{"preset":2,"enables":16929,"texture":"STONE",
            "particleLife":[0.5,0],"rate":[30,30,30,30]}],
          "textureAnimations":[{"name":"STONE","texture":0,"source":0,"frames":2,"rate":1}],
          "trees":[
            {"name":"PICKUP","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
              {"name":"FIRE","particle":0,"parent":0,"position":[0,1,0]}],
              "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
            {"name":"GEMORANGE","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
              "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
            {"name":"SPRAY","nodes":[{"name":"FIRE","particle":0,"parent":-1,"position":[0,0,0]}],
              "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
            {"name":"GETGEMORANGE","nodes":[{"name":"FIRE","particle":0,"parent":-1,"position":[0,0,0]}],
              "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]}]})");
        REQUIRE(archive.load(root));
        REQUIRE(world.load(device, root, {.name = "TEST", .directory = "."}));
        // This synthetic world deliberately borrows the separately owned
        // particle archive; normal LevelWorld loading binds its level banks.
        REQUIRE(const_cast<PlacedItems&>(world.placedItems())
                    .bind(device, world.layout(), &world.collision(), std::array{&archive}));
        world.setPlayerCount(1);
        REQUIRE(world.placedItems().size() == 2);
        REQUIRE(world.particles().size() == 1);
        REQUIRE(figure.place(device, archive, "PICKUP", world.layout().itemInstances()[0],
                             &world.collision()));
        figure.play(0, true);
        REQUIRE(catalog.load(root));
        REQUIRE(
            portals.bind(device, world.layout(), AssetScope(archive), catalog, &world.collision()));
        REQUIRE(portals.size() == 1);
        REQUIRE(portals.portal(0).secret);
        effect = effects.startSet(device, archive, "SPRAY", {3, 4, 5}, {.seconds = 3});
        REQUIRE(effect != 0);
        REQUIRE(projectiles.startSet(device, archive, "SPRAY", {-3, 4, 5}, {.seconds = 3}) != 0);
        const auto descriptor =
            ParticleDescriptor::fromTemplate(archive.trees.particleTemplates()[0]);
        effects.attachTrail(effect, descriptor, device.whiteTexture());
        REQUIRE(effects.startParticles(device, archive, descriptor, 0,
                                       glm::translate(Mat4{1}, Vec3{8, 7, 6})) != 0);
    }
    void step(f32 seconds) {
        world.capturePresentation();
        world.update(seconds);
        figure.capturePresentation();
        figure.update(seconds);
        portals.animate(seconds);
        projectiles.capturePresentation();
        projectiles.update(seconds);
        effects.capturePresentation();
        effects.update(seconds);
    }
    std::unique_ptr<StageParticlePresentation> capture(bool projectileAdvance = true) const {
        const std::array figures{StageParticlePresentation::Figure{&figure, true}};
        return StageParticlePresentation::capture(
            7, 25, lifetime, {world, projectiles, effects, &portals, figures, projectileAdvance});
    }
};

TEST_CASE("stage particles retain exact native ambient effect and trail samples without GPU work",
          "[rollback][stage-particle-presentation]") {
    const s32 rate = GENERATE(30, 60);
    const f32 alpha = GENERATE(-1.0f, 0.0f, 0.25f, 0.5f, 1.0f);
    const bool held = GENERATE(false, true);
    Fixture fixture;
    for (s32 i = 0; i < 7; ++i) {
        fixture.step(1.0f / static_cast<f32>(rate));
    }
    if (held) {
        fixture.step(0);
    }
    REQUIRE(fixture.world.particles().particleCount() > 0);
    REQUIRE(fixture.effects.effect(0).trails.particleCount() > 0);
    const auto uploads = fixture.device.texturesCreated;
    const auto updates = fixture.device.textureUpdates;
    const auto effectState = fixture.effects.checkpoint(fixture.effect);
    const auto picture = fixture.capture(!held);
    REQUIRE(picture);
    CHECK(picture->matches(7, 25));
    CHECK_FALSE(picture->matches(8, 25));
    CHECK_FALSE(picture->matches(7, 26));
    CHECK(picture->particleCount() > 0);
    const CameraFrame camera;
    for (const auto phase : {Phase::World, Phase::Projectiles, Phase::Effects}) {
        fixture.device.draws.clear();
        if (phase == Phase::World) {
            fixture.world.particles().draw(fixture.device, Mat4{1}, camera.right, camera.up,
                                           held ? -1 : alpha);
        } else if (phase == Phase::Projectiles) {
            fixture.projectiles.draw(fixture.device, Mat4{1}, {}, &camera, held ? -1 : alpha);
        } else {
            fixture.effects.draw(fixture.device, Mat4{1}, {}, &camera, alpha);
        }
        const auto expected = fixture.device.draws;
        REQUIRE_FALSE(expected.empty());
        fixture.device.draws.clear();
        REQUIRE(picture->draw(phase, fixture.device, Mat4{1}, camera, alpha));
        sameDraws(fixture.device.draws, expected);
    }
    CHECK(fixture.effects.checkpoint(fixture.effect) == effectState);
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.device.textureUpdates == updates);
}

TEST_CASE("stage pickup bursts and secret icon particles use native independent clocks",
          "[rollback][stage-particle-presentation]") {
    const s32 rate = GENERATE(30, 60);
    const f32 alpha = GENERATE(-1.0f, 0.0f, 0.5f, 1.0f);
    const bool held = GENERATE(false, true);
    Fixture fixture;
    const auto& items = fixture.world.placedItems();
    REQUIRE(
        fixture.world.collect(fixture.device, std::array{Collector{items.item(1).position, 1, 1}})
            .size() == 1);
    for (s32 i = 0; i < 7; ++i) {
        fixture.step(1.0f / static_cast<f32>(rate));
    }
    if (held) {
        fixture.step(0);
    }
    REQUIRE(items.burstParticleCount() > 0);
    REQUIRE(items.item(0).particles.field().particleCount() > 0);
    REQUIRE(fixture.portals.portal(0).icon.particles().field().particleCount() > 0);
    const auto picture = fixture.capture();
    REQUIRE(picture);
    const CameraFrame camera;
    // Invoke the actual native draw first, then retain only its particle subpass.
    // This samples the native owner's texture bank instead of reproducing its rules.
    items.draw(fixture.device, Mat4{1}, {}, &camera, TreeModel::Pass::Blended, alpha);
    fixture.device.draws.clear();
    items.item(0).particles.draw(fixture.device, Mat4{1}, camera.right, camera.up,
                                 held ? -1 : alpha);
    items.bursts().draw(fixture.device, Mat4{1}, camera.right, camera.up, held ? -1 : alpha);
    const auto pickups = fixture.device.draws;
    fixture.device.draws.clear();
    REQUIRE(picture->draw(Phase::Pickups, fixture.device, Mat4{1}, camera, alpha));
    sameDraws(fixture.device.draws, pickups);

    fixture.portals.draw(fixture.device, Mat4{1}, {}, &camera, TreeModel::Pass::Effects);
    fixture.figure.draw(fixture.device, Mat4{1}, {}, 1, 1, &camera, TreeModel::Pass::Effects,
                        alpha);
    fixture.device.draws.clear();
    fixture.portals.portal(0).icon.particles().draw(fixture.device, Mat4{1}, camera.right,
                                                    camera.up, -1);
    fixture.figure.particles().draw(fixture.device, Mat4{1}, camera.right, camera.up,
                                    held ? -1 : alpha);
    const auto icons = fixture.device.draws;
    fixture.device.draws.clear();
    REQUIRE(picture->draw(Phase::Fixtures, fixture.device, Mat4{1}, camera, alpha));
    sameDraws(fixture.device.draws, icons);
}

TEST_CASE("owned stage particles survive native retirement and rewind but not asset teardown",
          "[rollback][stage-particle-presentation]") {
    Fixture fixture;
    for (s32 i = 0; i < 7; ++i) {
        fixture.step(1.0f / 60);
    }
    fixture.effects.finish(fixture.effect);
    fixture.step(1.0f / 60);
    REQUIRE(fixture.effects.effect(0).retiring);
    auto picture = fixture.capture();
    REQUIRE(picture);
    const CameraFrame camera;
    fixture.device.draws.clear();
    fixture.effects.draw(fixture.device, Mat4{1}, {}, &camera, 0.5f);
    const auto expected = fixture.device.draws;
    REQUIRE_FALSE(expected.empty());
    fixture.effects.clear();
    fixture.projectiles.clear();
    // Release only mutable pickup instances, retaining the immutable banks
    // borrowed by the picture until its lifetime token is invalidated below.
    const_cast<PlacedItems&>(fixture.world.placedItems()).clear();
    fixture.device.draws.clear();
    REQUIRE(picture->draw(Phase::Effects, fixture.device, Mat4{1}, camera, 0.5f));
    sameDraws(fixture.device.draws, expected);
    CHECK_FALSE(picture->draw(Phase::Effects, fixture.device, Mat4{1}, camera,
                              std::numeric_limits<f32>::quiet_NaN()));
    fixture.lifetime.reset();
    CHECK_FALSE(picture->matches(7, 25));
    fixture.device.draws.clear();
    CHECK_FALSE(picture->draw(Phase::Effects, fixture.device, Mat4{1}, camera, 0.5f));
    CHECK(fixture.device.draws.empty());
}

TEST_CASE("stage figure collection preserves native eligibility order pass and billboard basis",
          "[rollback][stage-particle-presentation][stage-figure-collection]") {
    const auto root = test::sampleLevel("stage-figure-collection");
    writeTextFile(root / "animations.json", R"({"trees":[{"name":"FIGURE",
      "nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"OFF","frames":8,"frameRate":30}]}]})");
    writeTextFile(root / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4}],
      "itemInfos":[
        {"type":2,"subtype":46,"name":"FIGURE","radius":1,"height":2,"activeType":16},
        {"type":7,"subtype":0,"name":"FIGURE","radius":1,"height":2},
        {"type":2,"subtype":43,"name":"FIGURE","radius":1,"height":2},
        {"type":8,"subtype":0,"name":"FIGURE","radius":1,"height":2},
        {"type":10,"subtype":0,"name":"FIGURE","radius":1,"height":2},
        {"type":5,"subtype":0,"name":"FIGURE","radius":1,"height":2},
        {"type":12,"subtype":2,"name":"FIGURE","radius":1,"height":2},
        {"type":1,"subtype":2,"name":"FIGURE","radius":1,"value":1}],
      "itemInstances":[
        {"info":0,"position":[0,0,5],"minPlayers":1,"params":[7,0,0,0,1,0,0,0,0,0,0,0]},
        {"info":0,"position":[12,0,5],"minPlayers":2,"params":[7,0,0,0,1,0,0,0,0,0,0,0]},
        {"info":1,"position":[0,0,10],"minPlayers":1},
        {"info":1,"position":[12,0,10],"minPlayers":2},
        {"info":2,"position":[0,0,15],"minPlayers":1},
        {"info":2,"position":[12,0,15],"minPlayers":2},
        {"info":3,"position":[0,0,20],"minPlayers":1},
        {"info":3,"position":[12,0,20],"minPlayers":2},
        {"info":4,"position":[0,0,25],"minPlayers":1},
        {"info":4,"position":[12,0,25],"minPlayers":2},
        {"info":5,"position":[0,0,30],"minPlayers":1},
        {"info":5,"position":[12,0,30],"minPlayers":2},
        {"info":6,"position":[0,0,35]},
        {"info":6,"position":[12,0,35],"flags":2}]})");
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, {.name = "TEST", .directory = "."}));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    fixtures.bind({device, world, weapons, effects, audio, 1});
    LevelOpponents opponents;
    REQUIRE(world.triggers().size() == 2);
    REQUIRE(world.rotators().size() == 2);
    REQUIRE(world.rotators().rotator(0).pad);
    REQUIRE_FALSE(world.rotators().rotator(1).pad);
    REQUIRE(fixtures.chests().size() == 2);
    REQUIRE(fixtures.gates().size() == 2);
    REQUIRE(fixtures.barrels().size() == 2);
    REQUIRE(fixtures.traps().size() == 2);
    REQUIRE(fixtures.scenery().size() == 2);
    const s32 players = GENERATE(1, 2);
    world.setPlayerCount(players);
    fixtures.setPlayerCount(players);
    const std::array visitor{
        ChestVisitor{.position = fixtures.chests().chest(0).figure.position(), .xray = true}};
    REQUIRE(fixtures.chests().updateXray(device, world.itemScope(), world.items(), visitor) == 1);
    REQUIRE(fixtures.chests().chest(0).preview.hasFigure());
    auto rows = StageParticlePresentation::collectFigures(world, fixtures, opponents);
    REQUIRE(rows);
    usize next = 0;
    const auto expect = [&](const ItemFigure* figure, Phase phase, bool interpolated,
                            bool cameraFacing, bool mesh = false) {
        REQUIRE(next < rows->size());
        CAPTURE(next);
        CHECK((*rows)[next].figure == figure);
        CHECK((*rows)[next].phase == phase);
        CHECK((*rows)[next].interpolated == interpolated);
        CHECK((*rows)[next].cameraFacing == cameraFacing);
        CHECK((*rows)[next].retainMesh == mesh);
        ++next;
    };
    for (s32 i = 0; i < players; ++i) {
        expect(world.triggers().figure(static_cast<usize>(i)), Phase::WorldFixtures, false, false);
    }
    expect(world.rotators().rotator(0).pad.get(), Phase::WorldFixtures, false, false, true);
    expect(&fixtures.chests().chest(0).preview, Phase::SolidFixtures, true, true);
    for (s32 i = 0; i < players; ++i) {
        expect(&fixtures.chests().chest(static_cast<usize>(i)).figure, Phase::SolidFixtures, true,
               true);
    }
    for (s32 i = 0; i < players; ++i) {
        expect(&fixtures.gates().gate(static_cast<usize>(i)).figure, Phase::SolidFixtures, true,
               false);
    }
    for (s32 i = 0; i < players; ++i) {
        expect(&fixtures.barrels().barrel(static_cast<usize>(i)).figure, Phase::SolidFixtures, true,
               false);
    }
    for (s32 i = 0; i < players; ++i) {
        expect(&fixtures.traps().trap(static_cast<usize>(i)).figure, Phase::Fixtures, true, true);
    }
    for (s32 i = 0; i < players; ++i) {
        expect(&fixtures.scenery().prop(static_cast<usize>(i)).figure, Phase::Fixtures, true, true,
               true);
    }
    CHECK(next == rows->size());
    fixtures.chests().remove(0);
    rows = StageParticlePresentation::collectFigures(world, fixtures, opponents);
    REQUIRE(rows);
    CHECK(rows->size() == next - 2); // Both removed chest and its preview vanish.
    CHECK(std::ranges::none_of(*rows, [&](const auto& row) {
        return row.figure == &fixtures.chests().chest(0).figure ||
               row.figure == &fixtures.chests().chest(0).preview;
    }));
}

TEST_CASE(
    "stage decorative copies preserve native meshes particles and static fallback after rewind",
    "[rollback][stage-particle-presentation][stage-decoration]") {
    Fixture fixture;
    const s32 kind = GENERATE(0, 1, 2); // pad, scenery, model-only fallback scenery
    const f32 alpha = GENERATE(-1.0f, 0.25f, 1.0f);
    for (s32 i = 0; i < 7; ++i) {
        fixture.step(1.0f / 60);
    }
    if (kind == 2) {
        REQUIRE(fixture.figure.placeStaticFallback(fixture.device, AssetScope(fixture.archive),
                                                   "WALL", {.position = Vec3{2, 3, 4}}, nullptr,
                                                   0));
    }
    CameraFrame camera;
    camera.right = {0, 0, 1};
    camera.up = {0, 1, 0};
    const std::array figures{StageParticlePresentation::Figure{
        &fixture.figure, kind != 0, kind == 0 ? Phase::WorldFixtures : Phase::Fixtures, kind != 0,
        true}};
    const auto uploads = fixture.device.texturesCreated;
    const auto updates = fixture.device.textureUpdates;
    const auto picture = StageParticlePresentation::capture(
        7, 25, fixture.lifetime,
        {fixture.world, fixture.projectiles, fixture.effects, nullptr, figures});
    REQUIRE(picture);
    std::vector<std::pair<Phase, std::vector<test::RecordedDraw>>> expected;
    for (const auto phase : {Phase::WorldFixtures, Phase::SolidFixtures, Phase::Fixtures}) {
        fixture.device.draws.clear();
        if (kind == 0 && phase == Phase::WorldFixtures) {
            fixture.figure.draw(fixture.device, Mat4{1}, {}, 1, 1, nullptr, TreeModel::Pass::All,
                                -1);
        } else if (kind != 0 && phase != Phase::WorldFixtures) {
            fixture.figure.draw(fixture.device, Mat4{1}, {}, 1, 1, &camera,
                                phase == Phase::SolidFixtures ? TreeModel::Pass::DepthWriting
                                                              : TreeModel::Pass::Effects,
                                alpha);
        }
        expected.emplace_back(phase, fixture.device.draws);
    }
    REQUIRE(std::ranges::any_of(expected, [](const auto& value) { return !value.second.empty(); }));
    fixture.figure = ItemFigure{}; // The native owner and its fallback TreeInfo may disappear.
    for (const auto& [phase, draws] : expected) {
        fixture.device.draws.clear();
        REQUIRE(picture->drawDecorations(phase, fixture.device, Mat4{1}, {}, camera, alpha));
        sameDraws(fixture.device.draws, draws);
        fixture.device.draws.clear();
        REQUIRE(picture->draw(phase, fixture.device, Mat4{1}, camera, alpha));
        CHECK(fixture.device.draws.empty()); // No duplicate decorative particles.
    }
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.device.textureUpdates == updates);
    fixture.lifetime.reset();
    fixture.device.draws.clear();
    CHECK_FALSE(
        picture->drawDecorations(Phase::Fixtures, fixture.device, Mat4{1}, {}, camera, alpha));
    CHECK(fixture.device.draws.empty());
}

TEST_CASE("native null camera figure particles retain their default axes in owned pictures",
          "[rollback][stage-particle-presentation]") {
    Fixture fixture;
    for (s32 i = 0; i < 7; ++i) {
        fixture.step(1.0f / 60);
    }
    CameraFrame camera;
    camera.right = {0, 0, 1};
    const std::array figures{
        StageParticlePresentation::Figure{&fixture.figure, true, Phase::SolidFixtures, false}};
    const auto picture = StageParticlePresentation::capture(
        7, 25, fixture.lifetime,
        {fixture.world, fixture.projectiles, fixture.effects, nullptr, figures});
    REQUIRE(picture);
    fixture.figure.draw(fixture.device, Mat4{1}, {}, 1, 1, nullptr, TreeModel::Pass::Effects, 0.5f);
    fixture.device.draws.clear();
    const CameraFrame native;
    fixture.figure.particles().draw(fixture.device, Mat4{1}, native.right, native.up, 0.5f);
    const auto expected = fixture.device.draws;
    REQUIRE_FALSE(expected.empty());
    fixture.device.draws.clear();
    REQUIRE(picture->draw(Phase::SolidFixtures, fixture.device, Mat4{1}, camera, 0.5f));
    sameDraws(fixture.device.draws, expected);
}

TEST_CASE("stage scenery copies preserve falling interpolation and native wall flashes",
          "[rollback][stage-particle-presentation][stage-world-scenery]") {
    const s32 rate = GENERATE(30, 60);
    const s32 players = GENERATE(1, 2);
    const auto directory = test::sampleLevel("stage-world-scenery");
    writeTextFile(directory / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4}],
      "bounds":{"min":[-20,-10,-20],"max":[100,20,20]},
      "itemInfos":[
        {"type":10,"subtype":40,"name":"WALL","radius":1,"height":3},
        {"type":10,"subtype":49,"name":"WALL","radius":1,"height":3},
        {"type":10,"subtype":52,"name":"WALL","radius":1,"height":3},
        {"type":10,"subtype":53,"name":"WALL","radius":1,"height":3},
        {"type":10,"subtype":42,"name":"WALL","hitPoints":25,"armor":1,"radius":2,"height":5}],
      "itemInstances":[
        {"info":0,"name":"WALL","position":[0,0,0]},
        {"info":1,"name":"WALL","position":[10,0,0]},
        {"info":2,"name":"WALL","position":[20,0,0]},
        {"info":3,"name":"WALL","position":[30,0,0],"minPlayers":2},
        {"info":4,"name":"WALL","position":[40,0,5],
         "collision":[{"normal":[0,0,1],"vertices":[[-3,0,0],[3,0,0],[0,5,0]]}]},
        {"info":4,"name":"WALL","position":[50,0,5],
         "collision":[{"normal":[0,0,1],"vertices":[[-3,0,0],[3,0,0],[0,5,0]]}]},
        {"info":4,"name":"WALL","position":[60,0,5],"minPlayers":2,
         "collision":[{"normal":[0,0,1],"vertices":[[-3,0,0],[3,0,0],[0,5,0]]}]}]})");
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, directory, {.name = "TEST", .directory = "."}));
    world.setPlayerCount(players);
    REQUIRE(world.fallingScenery().size() == 4);
    REQUIRE(world.walls().size() == 3);
    CHECK(world.fallingScenery().piece(3).shown == (players == 2));
    CHECK(world.walls().standing(2) == (players == 2));
    REQUIRE(world.fallingScenery().touch({0, 0, 0}, 0.5f).size() == 1);
    REQUIRE(world.fallingScenery().touch({10, 0, 0}, 0.5f).size() == 1);
    REQUIRE(world.fallingScenery().shoot({20, 0, 0}, 0.5f).size() == 1);
    CHECK(world.fallingScenery().touch({30, 0, 0}, 0.5f).size() ==
          static_cast<usize>(players == 2));
    world.revealWall(0);
    EffectTrees projectiles, effects;
    std::shared_ptr<const void> lifetime = std::make_shared<const u8>(0);
    const auto textures = device.texturesCreated;
    const auto uploads = device.textureUpdates;
    const CameraFrame camera;
    const WorldLighting lighting;
    bool sawWhite = false, sawBlinkOff = false;
    for (s32 tick = 0; tick <= 46; ++tick) {
        world.capturePresentation();
        world.update(tick == 0 || tick == 46 ? 0 : 1.0f / static_cast<f32>(rate));
        if (tick == 1) {
            REQUIRE(world.strikeWall(1, 2) == 24);
        }
        if (tick == 26) {
            REQUIRE(world.strikeWall(1, 100) == 0);
        }
        sawWhite |= world.walls().wall(0).white;
        sawBlinkOff |= world.walls().wall(0).blinks > 0 && !world.walls().wall(0).white;
        if (tick != 0 && tick != 1 && tick != 5 && tick != 11 && tick != 17 && tick != 26 &&
            tick != 45 && tick != 46) {
            continue;
        }
        CAPTURE(rate, players, tick);
        const auto picture = StageParticlePresentation::capture(7, static_cast<u64>(tick), lifetime,
                                                                {world, projectiles, effects});
        REQUIRE(picture);
        std::vector<std::pair<f32, std::vector<test::RecordedDraw>>> samples;
        for (const f32 alpha : {-1.0f, 0.0f, 0.35f, 1.0f}) {
            device.draws.clear();
            world.fallingScenery().draw(device, Mat4{1}, lighting,
                                        world.particlesAdvanced() ? alpha : -1);
            world.walls().draw(device, Mat4{1}, lighting);
            samples.emplace_back(alpha, device.draws);
        }
        REQUIRE_FALSE(samples.front().second.empty());
        if (tick == 46) {
            // The retained meshes must survive destruction of mutable pool
            // entries; their immutable level mesh/texture banks stay alive.
            world.fallingScenery().clear();
            const_cast<DestructibleWalls&>(world.walls()).clear();
        }
        for (const auto& [alpha, expected] : samples) {
            device.draws.clear();
            REQUIRE(picture->drawDecorations(Phase::WorldScenery, device, Mat4{1}, lighting, camera,
                                             alpha));
            sameDraws(device.draws, expected);
            device.draws.clear();
            REQUIRE(picture->draw(Phase::WorldScenery, device, Mat4{1}, camera, alpha));
            CHECK(device.draws.empty());
        }
        if (tick == 46) {
            lifetime.reset();
            device.draws.clear();
            CHECK_FALSE(picture->drawDecorations(Phase::WorldScenery, device, Mat4{1}, lighting,
                                                 camera, 0.5f));
            CHECK(device.draws.empty());
        }
    }
    CHECK(sawWhite);
    CHECK(sawBlinkOff);
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == uploads);
}

TEST_CASE("stage swarm anchors distinguish absent capture and expire with the borrowed assets",
          "[rollback][stage-particle-presentation]") {
    Fixture fixture;
    const auto without = fixture.capture();
    REQUIRE(without);
    CHECK_FALSE(without->swarm());
    Enemies enemies;
    const auto with = StageParticlePresentation::capture(
        7, 25, fixture.lifetime,
        {fixture.world, fixture.projectiles, fixture.effects, nullptr, {}, true, &enemies});
    REQUIRE(with);
    REQUIRE(with->swarm());
    CHECK(with->swarm()->enemies.empty());
    fixture.lifetime.reset();
    CHECK_FALSE(with->swarm());
}

TEST_CASE("stage point lights are bounded owned native-order endpoints with a resource fence",
          "[rollback][stage-particle-presentation][stage-point-lights]") {
    Fixture fixture;
    std::vector<PointLight> lights;
    for (usize index = 0; index < WorldLighting::kMostPoints; ++index) {
        lights.push_back(
            {{static_cast<f32>(index), 3, 2}, {0.2f, 0.5f, 0.7f}, static_cast<f32>(index), 2});
    }
    StageParticlePresentation::Sources sources{fixture.world, fixture.projectiles, fixture.effects};
    sources.lights = lights;
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    const auto captured = StageParticlePresentation::capture(7, 25, fixture.lifetime, sources);
    REQUIRE(captured);
    REQUIRE(captured->lights().size() == lights.size());
    for (usize index = 0; index < lights.size(); ++index) {
        CHECK(captured->lights()[index].position == lights[index].position);
        CHECK(captured->lights()[index].color == lights[index].color);
        CHECK(captured->lights()[index].radius == lights[index].radius);
        CHECK(captured->lights()[index].intensity == lights[index].intensity);
    }
    lights.front().position.x = 100;
    lights.clear();
    CHECK(captured->lights().front().position.x == 0);
    CHECK(captured->matches(7, 25, fixture.lifetime));
    CHECK_FALSE(captured->matches(7, 26));
    CHECK_FALSE(captured->matches(8, 25));
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
    fixture.lifetime.reset();
    CHECK(captured->lights().empty());
    CHECK_FALSE(captured->matches(7, 25));
}

TEST_CASE("stage point light capture rejects malformed lists without changing the sources",
          "[rollback][stage-particle-presentation][stage-point-lights]") {
    const s32 invalid = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    Fixture fixture;
    std::vector<PointLight> lights(1);
    const auto nan = std::numeric_limits<f32>::quiet_NaN();
    if (invalid == 0) {
        lights.resize(WorldLighting::kMostPoints + 1);
    } else if (invalid == 1) {
        lights[0].position.x = nan;
    } else if (invalid == 2) {
        lights[0].position.y = std::numeric_limits<f32>::infinity();
    } else if (invalid == 3) {
        lights[0].color.z = nan;
    } else if (invalid == 4) {
        lights[0].radius = -1;
    } else if (invalid == 5) {
        lights[0].radius = nan;
    } else if (invalid == 6) {
        lights[0].intensity = nan;
    } else {
        lights[0].intensity = std::numeric_limits<f32>::infinity();
    }
    StageParticlePresentation::Sources sources{fixture.world, fixture.projectiles, fixture.effects};
    sources.lights = lights;
    const auto before = fixture.world.particles().checkpoint();
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    CHECK_FALSE(StageParticlePresentation::capture(7, 25, fixture.lifetime, sources));
    CHECK(fixture.world.particles().checkpoint() == before);
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
}
} // namespace
