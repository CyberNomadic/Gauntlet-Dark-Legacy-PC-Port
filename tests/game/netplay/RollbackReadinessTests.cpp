#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/OnlinePair.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/PlayReplication.h"
#include "game/screens/SessionInputs.h"
#include "game/world/LevelCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Actual PlayScene/PlayReplication capture and native enemy/barrel collision.
 * Only the map is simplified: a flat floor removes incidental camera and ramp
 * differences from the exact contact that a guest must reproduce or decline. */
struct ReadinessScene {
    std::filesystem::path assets =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    std::filesystem::path directory = test::sampleLevel("rollback-readiness");
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene play;
    test::OnlinePair pair;
    PickupResources pickups;
    FixtureResources fixtures;
    ProjectileResources projectiles;
    PlayReplication driver;
    LocalMovementPrediction prediction;
    CombatSnapshot authority;

    explicit ReadinessScene(bool barrel = false) {
        const std::string items = barrel ? R"(
            "itemInfos":[{"type":10,"subtype":43,"name":"BARREL","radius":1,
                          "height":3,"collisionType":1,"hitPoints":10}],
            "itemInstances":[{"info":0,"position":[0,0,4]}],)"
                                         : "";
        writeTextFile(directory / "world.json", std::format(R"({{
            "objects":[{{"name":"FLOOR","position":[0,0,0],"flags":4}}],
            {} "animations":[]}})",
                                                            items));
        REQUIRE(catalog.load(assets));
        auto level = catalog.byName("G1");
        REQUIRE(level);
        level->directory = directory.string();
        REQUIRE(world.load(device, assets, *level));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): owned fixture world
        auto& collision = const_cast<WorldCollision&>(world.collision());
        collision.build(
            {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
             {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
        GameContext context;
        context.config = &config;
        context.unpackedRoot = assets;
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{0};
        std::vector<PartyMember> party;
        REQUIRE(pair.hostSession.party());
        for (usize seat = 0; seat < pair.hostSession.party()->size(); ++seat) {
            if (const auto& profile = (*pair.hostSession.party())[seat]) {
                party.push_back({static_cast<s32>(seat), profile->gameplayCopy()});
            }
        }
        REQUIRE(play.open(device, context, world, party, options));
        for (s32 tick = 0; tick < 400; ++tick) {
            REQUIRE(play.update(1.0 / 60, {}) == PlayOutcome::Running);
        }
        REQUIRE(play.movementPredictable());
        REQUIRE_FALSE(play.animator(1)->entering());
        place(0, {20, 0, 0});
        place(1, Vec3{0});
        REQUIRE(play.enemies().loadKind(kGruntKind));
        REQUIRE(pickups.bind(device, world.placedItems().archives()));
        auto archives = world.placedItems().archives();
        std::vector<ItemArchive*> sources(archives.begin(), archives.end());
        if (world.realmItems().loaded()) {
            sources.push_back(&world.realmItems());
        }
        REQUIRE(
            fixtures.bind(device, sources, play.generators(), play.safeRocks(), &play.statues()));
        REQUIRE(play.bindSceneProjectiles(projectiles));
    }

    PlayerRuntime& player(usize index = 1) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): owned native scene
        return const_cast<PlayerRuntime&>(play.participants()[index]);
    }
    void place(usize index, const Vec3& position) {
        auto& runtime = player(index);
        runtime.actor.place(position);
        runtime.actor.turnTo(0);
        runtime.floor = {};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): owned native party
        auto participants = std::span{const_cast<PlayerRuntime*>(play.participants().data()),
                                      play.participants().size()};
        FloorRiding::land(participants, index, position, world.collision());
    }
    s32 spawn(f32 gap) {
        auto& runtime = player();
        const auto enemy =
            play.enemies().spawn({.kind = kGruntKind,
                                  .position = runtime.actor.position() + Vec3{0, 0, gap},
                                  .placed = true},
                                 {});
        REQUIRE(enemy);
        // Keep the native enemy's observed contact geometry stationary without
        // bypassing its target/collision code or giving replay authority over AI.
        runtime.actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime,
                                                             0, -1);
        return *enemy;
    }
    void begin() {
        pair.prepare();
        REQUIRE(driver.bind(play, pair.host, pickups, fixtures));
        pair.ready();
        REQUIRE(driver.advance(pair.host, {}, projectiles) == PlayReplication::Result::Advanced);
        REQUIRE(driver.latest());
        const auto encoded = CombatPacket::encode(*driver.latest());
        REQUIRE(encoded);
        const auto decoded = CombatPacket::decode(*encoded);
        REQUIRE(decoded);
        authority = *decoded;
        REQUIRE(authority.players[1]->movement.current);
        prediction.begin(pair.host.context(), 2, world.collision(), &world.scene(),
                         &world.layout());
        REQUIRE(prediction.setPlayer(1, *player().figure->actionTree()));
        prediction.update(&authority, {});
    }
    InputCommand forward() const {
        InputCommand command;
        command.epoch = authority.motion.epoch;
        command.tick = authority.motion.tick + 1;
        command.seat = 1;
        command.grant = authority.motion.players[1]->grant;
        const f32 heading = -authority.players[1]->movement.cameraYaw;
        command.direction = {std::sin(heading), std::cos(heading)};
        command.magnitude = 1;
        return command;
    }
    void native(const InputCommand& command) {
        PlayScene::Inputs inputs;
        inputs[1] = SessionInputs::playInput(command);
        REQUIRE(play.update(1.0 / 60, inputs) == PlayOutcome::Running);
    }
    CombatSnapshot shown(const InputCommand& command) {
        const auto before = player().figure->animator().checkpoint();
        const auto inventory = player().actor.save().progress().inventory;
        const auto health = player().actor.save().health();
        prediction.update(&authority, std::array{command});
        auto result = authority;
        prediction.present(result, 1);
        REQUIRE(result.valid());
        CHECK(player().figure->animator().checkpoint() == before);
        CHECK(player().actor.save().progress().inventory == inventory);
        CHECK(player().actor.save().health() == health);
        return result;
    }
};

TEST_CASE("native contact rollback cannot walk through a stationary enemy or barrel",
          "[netplay][rollback-readiness][locomotion-contacts][assets]") {
    const bool barrel = GENERATE(false, true);
    ReadinessScene scene(barrel);
    if (barrel) {
        REQUIRE(scene.play.barrels().size() == 1);
        const auto& box = scene.play.barrels().barrel(0).box;
        scene.place(1, box.centre - Vec3{0, 0,
                                         scene.player().actor.radius() +
                                             std::max(box.cylinderRadius, box.halfAlong) + 0.01f});
    } else {
        scene.spawn(scene.player().actor.radius() + enemyKind(kGruntKind).radius + 0.01f);
    }
    scene.begin();
    const auto input = scene.forward();
    const Vec3 before = scene.player().actor.position();
    scene.native(input);
    const f32 moved = glm::distance(scene.player().actor.position(), before);
    REQUIRE(moved < scene.player().actor.speed() / 60 * 0.5f);
    const auto shown = scene.shown(input);
    REQUIRE(scene.prediction.replayed(1));
    CHECK(glm::distance(shown.motion.players[1]->position, scene.player().actor.position()) <
          1e-4f);
    CHECK(shown.players[1]->rollback == scene.player().figure->animator().checkpoint());
}

TEST_CASE("native unpressed automatic melee cannot be misrepresented as predicted running",
          "[netplay][rollback-readiness][locomotion-contacts][auto-melee][assets]") {
    const bool automatic = GENERATE(false, true);
    ReadinessScene scene;
    scene.config.combat.autoMelee = automatic;
    scene.player().actor.save().autoAttack = true;
    scene.spawn(scene.player().actor.reach() + enemyKind(kGruntKind).radius - 0.1f);
    scene.begin();
    const auto input = scene.forward();
    scene.native(input);
    CHECK(scene.player().figure->animator().meleeing() == automatic);
    const auto shown = scene.shown(input);
    if (automatic) {
        // Until automatic contact attacks are replayed completely, only this
        // positive unpressed acquisition must wait for the host action.
        CHECK_FALSE(scene.prediction.replayed(1));
        CHECK(shown.motion.players[1]->position == scene.authority.motion.players[1]->position);
        CHECK(shown.players[1]->rollback == scene.authority.players[1]->rollback);
    } else {
        REQUIRE(scene.prediction.replayed(1));
        CHECK(glm::distance(shown.motion.players[1]->position, scene.player().actor.position()) <
              1e-4f);
        CHECK(shown.players[1]->rollback == scene.player().figure->animator().checkpoint());
    }
}
} // namespace
