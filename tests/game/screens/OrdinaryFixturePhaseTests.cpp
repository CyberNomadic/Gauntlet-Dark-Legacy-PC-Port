#include <array>
#include <cmath>
#include <limits>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/ItemPickup.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/LevelFixturesCheckpoint.h"
#include "game/screens/OrdinaryFixturePhase.h"
#include "game/screens/PlayerRuntimeCheckpoint.h"
#include "game/world/ExitPortals.h"
#include "game/world/LevelTransporters.h"
#include "game/world/LevelWorldCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path fixtureStage(std::string_view tag) {
    const auto root = test::sampleLevel(tag);
    writeTextFile(root / "animations.json", R"({"trees":[
      {"name":"CHEST","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
         {"name":"NULL1","parent":0,"position":[0,0,0]}],
       "sequences":[{"name":"OFF","frames":8,"rate":30},
         {"name":"OPENING","frames":8,"rate":30},{"name":"OPEN","frames":8,"rate":30}]},
      {"name":"GATE","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"OFF","frames":8,"rate":30},
         {"name":"OPENING","frames":8,"rate":30},{"name":"OPEN","frames":8,"rate":30}]},
      {"name":"TRAP","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"OFF","frames":8,"rate":30},
         {"name":"WINDUP","frames":3,"rate":30},{"name":"STRIKE","frames":3,"rate":30}]},
      {"name":"KEY","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"OFF","frames":8,"rate":30}]}]})");
    writeTextFile(root / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4100}],
      "bounds":{"min":[-40,-20,-40],"max":[40,20,40]},
      "animations":[{"object":0,"frames":30,"state":257,"start":0,
        "track":{"flags":32,"frames":[0,29],"values":[0,1]}}],
      "itemInfos":[
       {"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2,"activeType":16},
       {"type":7,"subtype":0,"name":"GATE","radius":1,"height":2},
       {"type":8,"subtype":0,"name":"TRAP","radius":1,"height":2,"value":20,"activeOff":15},
       {"type":1,"subtype":2,"name":"KEY","radius":1,"value":1}],
      "itemInstances":[{"info":0,"position":[0,0,5],"params":[3,0,0,0,1,0,0,0,0,0,0,0]},
        {"info":1,"position":[-12,0,5]},{"info":2,"position":[12,0,5]}]})");
    writeTextFile(root / "collision.json", R"({"objects":[
      {"object":0,"normals":[0,1,0,0,1,0],
       "vertices":[-30,0,-30,-30,0,30,30,0,30,-30,0,-30,30,0,30,30,0,-30]}]})");
    return root;
}

struct Fixture {
    struct State {
        LevelWorldCheckpoint::State world;
        LevelFixturesCheckpoint::State fixtures;
        EffectTrees::PoolCheckpoint effects;
        std::array<PlayerRuntimeCheckpoint::State, 2> players;
        bool operator==(const State&) const = default;
    };
    struct Rewind {
        LevelWorldCheckpoint::Prepared world;
        LevelFixturesCheckpoint::Prepared fixtures;
        EffectTrees::PreparedRestore effects;
        std::array<PlayerRuntimeCheckpoint::Prepared, 2> players;
    };
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    std::array<PlayerRuntime, 2> players;
    LevelWorldCheckpoint worldState{world};
    LevelFixturesCheckpoint fixtureState{fixtures};
    std::array<std::unique_ptr<PlayerRuntimeCheckpoint>, 2> playerState;
    usize external = 0;
    LevelFixtures::Events events{
        .hurt = [&](usize, f32, HurtKind, bool, const PlayerImpact&) { ++external; },
        .help =
            [&](s32, usize) {
                ++external;
                return true;
            },
        .card = [&](s32, std::string_view) { ++external; },
        .opponents = [&](const Vec3&, f32, f32, std::vector<s32>&, u32) { ++external; },
        .releaseEnemy =
            [&](s32, const Vec3&, s32) {
                ++external;
                return false;
            },
        .shatterPotion = [&](s32, const Vec3&) { ++external; }};

    Fixture(const std::filesystem::path& root, const LevelRef& level, bool entrance = false) {
        REQUIRE(world.load(device, root, level));
        world.setPlayerCount(2);
        fixtures.bind({device, world, weapons, effects, audio, 1});
        fixtures.setPlayerCount(2);
        for (usize index = 0; index < fixtures.chests().size(); ++index) {
            REQUIRE(fixtures.chests().chest(index).figure.hasFigure());
        }
        // Fixture binding appends collision surfaces, so bind the complete
        // world catalog only after that native setup phase has finished.
        REQUIRE(worldState.bindResources(device));
        REQUIRE(fixtureState.bindResources());
        for (usize index = 0; index < players.size(); ++index) {
            Vec3 position{index == 0 ? -5.0f : 5.0f, 0, -5};
            f32 yaw = 0;
            if (entrance) {
                const auto* start = world.startPoint(0);
                REQUIRE(start);
                yaw = start->rotation.y + kPi;
                position = start->position +
                           Vec3{std::cos(yaw), 0, -std::sin(yaw)} * (index == 0 ? -1.5f : 1.5f);
            }
            auto& player = players[index];
            player.actor.spawn(index == 0 ? 3 : 1, {}, nullptr, position, yaw);
            player.actor.settle(world.collision());
            player.entrySave = player.actor.save();
            player.cloudGap = 0.5f;
            player.hitSoundGap = 12;
            playerState[index] = std::make_unique<PlayerRuntimeCheckpoint>(player, nullptr);
        }
    }
    State state() const {
        auto w = worldState.checkpoint();
        auto f = fixtureState.checkpoint();
        auto e = effects.checkpointPool();
        auto p0 = playerState[0]->checkpoint();
        auto p1 = playerState[1]->checkpoint();
        REQUIRE(w);
        REQUIRE(f);
        REQUIRE(e);
        REQUIRE(p0);
        REQUIRE(p1);
        return {std::move(*w), std::move(*f), std::move(*e), {std::move(*p0), std::move(*p1)}};
    }
    Rewind prepare(const State& state) {
        auto w = worldState.prepareRestore(state.world);
        auto f = fixtureState.prepareRestore(state.fixtures);
        auto e = effects.prepareRestorePool(state.effects);
        auto p0 = playerState[0]->prepareRestore(state.players[0]);
        auto p1 = playerState[1]->prepareRestore(state.players[1]);
        REQUIRE(w);
        REQUIRE(f);
        REQUIRE(e);
        REQUIRE(p0);
        REQUIRE(p1);
        return {std::move(*w), std::move(*f), std::move(*e), {std::move(*p0), std::move(*p1)}};
    }
    void rewind(Rewind&& value) {
        REQUIRE(worldState.canCommit(value.world));
        REQUIRE(fixtureState.canCommit(value.fixtures));
        REQUIRE(effects.canCommit(value.effects));
        REQUIRE(playerState[0]->canCommit(value.players[0]));
        REQUIRE(playerState[1]->canCommit(value.players[1]));
        REQUIRE(worldState.commitRestore(std::move(value.world)));
        REQUIRE(fixtureState.commitRestore(std::move(value.fixtures)));
        REQUIRE(effects.commitRestore(std::move(value.effects)));
        REQUIRE(playerState[0]->commitRestore(std::move(value.players[0])));
        REQUIRE(playerState[1]->commitRestore(std::move(value.players[1])));
    }
};

TEST_CASE("passive fixture phase matches native clocks and moving supports",
          "[rollback][ordinary-fixture-phase]") {
    const auto directory = fixtureStage("ordinary-fixture-phase-native");
    const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
    Fixture native(directory, level), replay(directory, level);
    const auto rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    const s32 ticks = 60 / rate;
    REQUIRE(native.fixtures.chests().size() == 1);
    REQUIRE(native.fixtures.gates().size() == 1);
    REQUIRE(native.fixtures.traps().size() == 1);
    const auto start = replay.state();
    const auto chestPosition = replay.fixtures.chests().chest(0).figure.position();
    auto undo = replay.prepare(start);
    const auto textures = replay.device.texturesCreated;
    const auto uploads = replay.device.textureUpdates;
    bool moved = false, woke = false;
    for (s32 tick = 0; tick < rate; ++tick) {
        CAPTURE(rate, tick);
        native.world.update(seconds, false);
        replay.world.update(seconds, false);
        native.fixtures.update(ticks, seconds, native.players, native.events);
        const auto result =
            OrdinaryFixturePhase::advance(replay.fixtures, ticks, seconds, replay.players);
        REQUIRE_FALSE(result.refusal);
        CHECK(replay.state() == native.state());
        moved |= replay.fixtures.chests().chest(0).figure.position().y != chestPosition.y;
        woke |= !replay.fixtures.traps().wakes().empty();
    }
    CHECK(moved);
    CHECK(woke);
    CHECK(replay.players[0].cloudGap == 0);
    CHECK(replay.players[1].hitSoundGap == 0);
    CHECK(native.external == 0);
    CHECK(replay.external == 0);
    CHECK(replay.device.texturesCreated == textures);
    CHECK(replay.device.textureUpdates == uploads);
    replay.rewind(std::move(undo));
    CHECK(replay.state() == start);
}

TEST_CASE("staged chest refusal restores contact clocks RNG and the whole bound stage",
          "[rollback][ordinary-fixture-phase]") {
    const auto directory = fixtureStage("ordinary-fixture-phase-chest");
    const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
    Fixture fixture(directory, level);
    auto& actor = fixture.players[0].actor;
    const auto position = fixture.fixtures.chests().chest(0).figure.position();
    actor.slide(position - actor.position(), nullptr);
    actor.save().progress().inventory.keys = 0;
    const auto before = fixture.state();
    auto undo = fixture.prepare(before);
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    const auto result =
        OrdinaryFixturePhase::advance(fixture.fixtures, 1, 1.0f / 60, fixture.players);
    REQUIRE(result.refusal == LevelFixtures::Boundary::Chest);
    CHECK(result.index == 0);
    CHECK(fixture.external == 0);
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
    CHECK(fixture.fixtures.chests().chest(0).refusalLeft > 0);
    fixture.rewind(std::move(undo));
    CHECK(fixture.state() == before);
    // The normal path still dispatches its real help callback at this boundary.
    fixture.fixtures.update(1, 1.0f / 60, fixture.players, fixture.events);
    CHECK(fixture.external == 1);
}

TEST_CASE("malformed fixture phase requests leave every owner unchanged",
          "[rollback][ordinary-fixture-phase]") {
    const auto directory = fixtureStage("ordinary-fixture-phase-malformed");
    const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
    Fixture fixture(directory, level);
    const auto before = fixture.state();
    for (const auto seconds : {0.0f, -1.0f, std::numeric_limits<f32>::quiet_NaN()}) {
        CHECK(
            OrdinaryFixturePhase::advance(fixture.fixtures, 1, seconds, fixture.players).refusal ==
            LevelFixtures::Boundary::InvalidStep);
        CHECK(fixture.state() == before);
    }
    CHECK(OrdinaryFixturePhase::advance(fixture.fixtures, 1, 1.0f / 60, {}).refusal ==
          LevelFixtures::Boundary::InvalidStep);
    CHECK(fixture.state() == before);
}

TEST_CASE("staged chest unlock and lid completion stop before inventory or item resources",
          "[rollback][ordinary-fixture-phase]") {
    const auto directory = fixtureStage("ordinary-fixture-phase-opening");
    const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
    Fixture fixture(directory, level);
    auto& actor = fixture.players[0].actor;
    const auto position = fixture.fixtures.chests().chest(0).figure.position();
    actor.save().progress().inventory.keys = 1;
    const bool opening = GENERATE(false, true);
    if (opening) {
        // Enter the exact leaf opening state without executing scene dispatch.
        const std::array visitors{ChestVisitor{position, actor.radius(), 1}};
        const auto events = fixture.fixtures.chests().update(0, visitors);
        REQUIRE(events.size() == 1);
        REQUIRE(events[0].kind == ChestEvent::Kind::Unlocked);
        REQUIRE(fixture.fixtures.chests().chest(0).state == Chests::kOpening);
    } else {
        actor.slide(position - actor.position(), nullptr);
    }
    const auto before = fixture.state();
    auto undo = fixture.prepare(before);
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    const auto pickups = fixture.world.placedItems().size();
    LevelFixtures::Phase result;
    for (s32 tick = 0; tick < 30 && !result.refusal; ++tick) {
        result = OrdinaryFixturePhase::advance(fixture.fixtures, 1, 1.0f / 60, fixture.players);
    }
    REQUIRE(result.refusal == LevelFixtures::Boundary::Chest);
    CHECK(fixture.fixtures.chests().chest(0).state == (opening ? Chests::kOpen : Chests::kOpening));
    CHECK(actor.save().progress().inventory.keys == 1);
    CHECK(fixture.world.placedItems().size() == pickups);
    CHECK(fixture.external == 0);
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
    fixture.rewind(std::move(undo));
    CHECK(fixture.state() == before);
}

TEST_CASE("staged X-Ray refuses before creating the native chest preview",
          "[rollback][ordinary-fixture-phase]") {
    const auto directory = fixtureStage("ordinary-fixture-phase-xray");
    const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
    Fixture fixture(directory, level);
    auto& actor = fixture.players[0].actor;
    const auto position = fixture.fixtures.chests().chest(0).figure.position();
    actor.slide(position + Vec3{0, 0, -5} - actor.position(), nullptr);
    actor.save().progress().inventory.addPowerup(static_cast<s32>(ItemKind::SpecialPowerup),
                                                 powerup::kXRay, 0, 30);
    const auto before = fixture.state();
    auto undo = fixture.prepare(before);
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    const auto result =
        OrdinaryFixturePhase::advance(fixture.fixtures, 1, 1.0f / 60, fixture.players);
    REQUIRE(result.refusal == LevelFixtures::Boundary::Xray);
    CHECK_FALSE(fixture.fixtures.chests().chest(0).preview.hasFigure());
    CHECK(fixture.external == 0);
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
    fixture.rewind(std::move(undo));
    CHECK(fixture.state() == before);
    // The same content creates a genuinely bound native preview outside the
    // staged path. Its WALL textures are already cached by the chest's figure;
    // a new figure does not imply another GPU texture upload.
    const std::array visitors{ChestVisitor{actor.position(), actor.radius(), 0, true}};
    REQUIRE(fixture.fixtures.chests().updateXray(fixture.device, fixture.world.items(),
                                                 fixture.world.powerups(), 0, visitors,
                                                 &fixture.world.realmItems()) == 1);
    CHECK(fixture.fixtures.chests().chest(0).preview.hasFigure());
    CHECK(fixture.state().fixtures.chests != before.fixtures.chests);
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
}

TEST_CASE("native trap contact refuses before damage or help and rolls back its cooldown",
          "[rollback][ordinary-fixture-phase]") {
    const auto directory = fixtureStage("ordinary-fixture-phase-trap");
    const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
    Fixture fixture(directory, level);
    for (s32 tick = 0; tick < 180 && fixture.fixtures.traps().trap(0).action != 2; ++tick) {
        fixture.fixtures.update(1, 1.0f / 60, fixture.players, fixture.events);
    }
    REQUIRE(fixture.fixtures.traps().trap(0).action == 2);
    REQUIRE(fixture.external == 0);
    auto& actor = fixture.players[1].actor;
    actor.slide(fixture.fixtures.traps().trap(0).figure.position() - actor.position(), nullptr);
    const auto before = fixture.state();
    auto undo = fixture.prepare(before);
    const auto result =
        OrdinaryFixturePhase::advance(fixture.fixtures, 1, 1.0f / 60, fixture.players);
    REQUIRE(result.refusal == LevelFixtures::Boundary::Trap);
    CHECK(result.index == 0);
    CHECK(fixture.external == 0);
    CHECK(fixture.state().fixtures.traps != before.fixtures.traps);
    fixture.rewind(std::move(undo));
    CHECK(fixture.state() == before);
    fixture.fixtures.update(1, 1.0f / 60, fixture.players, fixture.events);
    CHECK(fixture.external == 2); // The native hurt and help callbacks both remain.
}

TEST_CASE("pending fixture hazard chains refuse without dispatching or allocating effects",
          "[rollback][ordinary-fixture-phase]") {
    const auto directory = fixtureStage("ordinary-fixture-phase-pending");
    const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
    Fixture fixture(directory, level);
    auto seed = fixture.state();
    const auto kind = GENERATE(0, 1, 2);
    LevelFixtures::Boundary expected = LevelFixtures::Boundary::Cloud;
    if (kind == 0) {
        seed.fixtures.clouds.push_back({{0, 0, 0}, 20, 0.001f, 0});
    } else if (kind == 1) {
        seed.fixtures.blasts.push_back({{0, 0, 0}, 12, 30});
        expected = LevelFixtures::Boundary::Blast;
    } else {
        seed.fixtures.doomedChests.push_back(0);
        expected = LevelFixtures::Boundary::DoomedChest;
    }
    REQUIRE(fixture.fixtureState.restore(seed.fixtures));
    const auto before = fixture.state();
    auto undo = fixture.prepare(before);
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    const auto result =
        OrdinaryFixturePhase::advance(fixture.fixtures, 1, 1.0f / 60, fixture.players);
    REQUIRE(result.refusal == expected);
    CHECK(fixture.external == 0);
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
    CHECK(fixture.effects.checkpointPool() == before.effects);
    fixture.rewind(std::move(undo));
    CHECK(fixture.state() == before);
}

TEST_CASE("native Ghost Town fixture roster runs at the entrance without omitting scenery",
          "[rollback][ordinary-fixture-phase][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    Fixture native(root, *level, true), replay(root, *level, true);
    const auto before = replay.state();
    REQUIRE_FALSE(before.fixtures.chests.chests.empty());
    REQUIRE_FALSE(before.fixtures.barrels.barrels.empty());
    const auto textures = replay.device.texturesCreated;
    const auto uploads = replay.device.textureUpdates;
    auto undo = replay.prepare(before);
    for (s32 tick = 0; tick < 6; ++tick) {
        CAPTURE(tick);
        native.world.update(1.0f / 60, false);
        replay.world.update(1.0f / 60, false);
        native.fixtures.update(1, 1.0f / 60, native.players, native.events);
        const auto result =
            OrdinaryFixturePhase::advance(replay.fixtures, 1, 1.0f / 60, replay.players);
        CAPTURE(result.index);
        REQUIRE_FALSE(result.refusal);
        CHECK(replay.state() == native.state());
    }
    CHECK(native.external == 0);
    CHECK(replay.external == 0);
    CHECK(replay.device.texturesCreated == textures);
    CHECK(replay.device.textureUpdates == uploads);
    replay.rewind(std::move(undo));
    CHECK(replay.state() == before);
}

TEST_CASE("travel contact values retain native authored boundaries and party filtering",
          "[ordinary-fixture-phase][stage-travel-contact]") {
    const auto directory = test::scratchDirectory("ordinary-stage-travel-contact");
    writeTextFile(directory / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0]}],
      "itemInfos":[{"type":9,"radius":3,"height":2},
                   {"type":11,"radius":2,"height":2}],
      "itemInstances":[{"info":0,"position":[0,0,0]},
        {"info":1,"position":[20,0,0],"params":[1,0,0,0,2,0,0,0]},
        {"info":1,"position":[40,0,0],"params":[2,0,0,0,1,0,0,0]},
        {"info":1,"position":[60,0,0],"minPlayers":2}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    test::FakeRenderDevice device;
    ItemArchive archive;
    LevelCatalog catalog;
    ExitPortals exits;
    REQUIRE(exits.bind(device, layout, archive, catalog, nullptr));
    const auto exitContacts = ExitPortals::contacts(layout, nullptr);
    REQUIRE(exitContacts.size() == exits.size());
    for (const f32 horizontal : {3.74f, 3.75f, 3.76f}) {
        for (const f32 vertical : {-3.01f, -3.0f, 3.0f, 3.01f}) {
            const std::array visitors{PortalVisitor{{horizontal, vertical, 0}, .75f}};
            CHECK(exitContacts[0].touches(visitors[0], 0) ==
                  exits.flamePosition(visitors).has_value());
        }
    }
    CHECK(exitContacts[0].touches({{3.75f, 3, 0}, .75f}, 0));
    CHECK_FALSE(exitContacts[0].touches({{3.76f, 3, 0}, .75f}, 0));
    CHECK(exitContacts[0].touches({{4.75f, 3, 0}, .75f}, 1));
    LevelTransporters pads;
    for (const s32 party : {1, 2}) {
        pads.bind(device, layout, archive, party);
        const auto contacts = LevelTransporters::contacts(layout, party);
        REQUIRE(contacts.size() == pads.size());
        CHECK(contacts.size() == static_cast<usize>(party + 1));
        CHECK(contacts[0].destination == 1);
        CHECK(contacts[1].destination == 0);
        for (const f32 horizontal : {2.74f, 2.75f, 2.76f}) {
            for (const f32 vertical : {-5.01f, -5.0f, 2.0f, 2.01f}) {
                const Vec3 feet{20 + horizontal, vertical, 0};
                CHECK(contacts[0].touches(feet, .75f, 5) ==
                      pads.contact(feet, .75f, 5).has_value());
            }
        }
        CHECK_FALSE(contacts[0].touches({22.75f, 0, 0}, .75f, 5));
        CHECK(contacts[0].touches({22.74f, 2, 0}, .75f, 5));
    }
    CHECK(device.texturesCreated == 0);
    CHECK(device.textureUpdates == 0);
}
} // namespace
