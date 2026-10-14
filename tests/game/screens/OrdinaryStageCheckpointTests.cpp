#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "game/players/ItemPickup.h"
#include "game/screens/AttackCheckpointFixture.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/LevelFixturesCheckpoint.h"
#include "game/screens/OrdinaryCombatCheckpoint.h"
#include "game/world/LevelWorldCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Boundary = OrdinaryCombatCheckpoint;

struct Fixture {
    std::filesystem::path root = test::attackCheckpointAssets();
    test::FakeRenderDevice device;
    ClassDataSet classes;
    LevelWorld world;
    ItemArchive weapons;
    TextureAnimator textures;
    EffectTrees prototypes;
    std::vector<EffectTrees::PoolResource> resources;
    EffectTrees effects;
    LevelSoundscape audio;
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    LevelOpponents opponents;
    LevelFixtures fixtures;
    PlayerAttacks attacks;
    std::array<PlayerRuntime, 2> players;
    PlayerHealth health;
    LevelWatch levels;
    LevelWorldCheckpoint worldOwner{world};
    LevelFixturesCheckpoint fixturesOwner{fixtures};
    LevelTransporters transporters;
    LevelTransportersCheckpoint transporterOwner{transporters};
    std::array<std::unique_ptr<PlayerRuntimeCheckpoint>, 2> runtimes;
    std::array<PlayerRuntimeCheckpoint*, 2> ordered{};
    std::array<PlayerMissiles::Resource, 1> missileResources{
        {{1, &MissileSpec::of(0), nullptr, nullptr}}};
    std::unique_ptr<CombatRollbackCheckpoint> combat;
    std::unique_ptr<Boundary> boundary;
    std::vector<std::string> outputs;

    explicit Fixture(std::string_view tag, bool pads = false) {
        // These are native archives and authored trees, not fabricated checkpoint
        // rows. The gas cloud's first tree really morphs into its looping tree.
        std::string trees = R"({"trees":[)";
        for (const auto* name : {"BURST", "POISONEXP1", "POISONEXP2", "POISONEXP3"}) {
            if (trees.back() != '[') {
                trees += ',';
            }
            trees += std::string{R"({"name":")"} + name + R"(",
                "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
                "sequences":[{"name":"ACTIVE","frames":4,"frameRate":30}]})";
        }
        trees += "]}";
        writeTextFile(root / "WEAPONS/animations.json", trees);
        const auto directory = test::sampleLevel(tag);
        writeTextFile(directory / "world.json", R"({
          "objects":[
            {"name":"FLOOR","position":[10,0,0],"next":1,"flags":4100},
            {"name":"FLOOR","position":[0,0,0],"next":-1}],
          "bounds":{"min":[-10,-10,-10],"max":[80,20,20]},
          "animations":[{"object":0,"frames":120,"state":257,"start":0,
            "track":{"flags":32,"frames":[0,119],"values":[0,6]}}],
          "itemInfos":[
            {"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2,"activeType":16},
            {"type":1,"subtype":1,"name":"COIN","radius":1,"height":2,"value":200},
            {"type":10,"subtype":41,"name":"ROCK","radius":2,"height":6,"hitPoints":40},
            {"type":10,"subtype":45,"name":"BARPOI","radius":1,"height":2,"hitPoints":10},
            {"type":11,"name":"TRANS","radius":1,"height":2}],
          "itemInstances":[
            {"info":0,"position":[0,0,0],"params":[1,0,0,0,1,0,0,0,0,0,0,0]},
            {"info":2,"position":[30,0,0],"params":[0,0,3,0,0,0,0,0,0,0,0,0]},
            {"info":3,"position":[50,0,0]},
            {"info":4,"position":[10,0,0],"params":[10,0,0,0,20,0,0,0]},
            {"info":4,"position":[0,0,1],"params":[20,0,0,0,10,0,0,0]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"CHEST","nodes":[
            {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
            {"name":"NULL1","parent":0,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"frameRate":30},
            {"name":"ACTIVE","frames":8,"frameRate":30,
             "tracks":[{"node":1,"flags":32,"frames":[0,7],"values":[0,2]}]},
            {"name":"OPEN","frames":0}]},
          {"name":"COIN","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":8,"frameRate":30}]},
          {"name":"BARPOI","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"frameRate":30},
             {"name":"BREAK","frames":8,"frameRate":30},
             {"name":"OPEN","frames":0}]},
          {"name":"TRANS","nodes":[{"name":"ROOT","object":"WALL","parent":-1,
              "position":[0,0,0]}],"sequences":[{"name":"ACTIVE","frames":4,"frameRate":30,
              "tracks":[{"node":0,"flags":16,"frames":[0,3],"values":[0,2]}]}]}]})");
        writeTextFile(directory / "collision.json", R"({"objects":[
          {"object":0,"normals":[0,1,0],"vertices":[-3,0,-3,3,0,-3,0,0,3]},
          {"object":1,"normals":[0,1,0],"vertices":[-5,0,-5,5,0,-5,0,0,5]}]})");
        REQUIRE(world.load(device, directory, LevelRef{"TEST", 0, "TEST", "", "", "", "", 0}));
        REQUIRE(classes.load(root / "pdata"));
        REQUIRE(weapons.load(root / "WEAPONS"));
        textures.bind(weapons.trees.textureAnimations(), weapons.textures, device);
        for (usize index = 0; index < weapons.trees.size(); ++index) {
            REQUIRE(prototypes.start(device, weapons,
                                     weapons.trees.tree(static_cast<u32>(index)).name, {}) != 0);
        }
        for (usize index = 0; index < prototypes.count(); ++index) {
            resources.push_back(
                {static_cast<u32>(index + 1), &weapons, {}, &textures, &prototypes.effect(index)});
        }
        arsenal.bind({device,
                      classes,
                      weapons,
                      world.collision(),
                      effects,
                      audio,
                      nullptr,
                      {},
                      false,
                      false,
                      &weapons});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        for (usize index = 0; index < players.size(); ++index) {
            auto& player = players[index];
            const s32 slot = index == 0 ? 1 : 3;
            player.actor.spawn(slot, {}, classes.stats(0),
                               index == 0 ? Vec3{10, 0, 0} : Vec3{0, 0, 1.5f}, 0);
            player.actor.save().progress().health = 2000;
            player.actor.save().progress().inventory.keys = 1;
            player.entrySave = player.actor.save();
            player.figure = PlayerFigure::load(device, root, player.actor.save(), false);
            REQUIRE(player.figure);
            levels.observe(slot, 1);
        }
        opponents.open({device, world, weapons, effects, audio, root, 1}, players);
        fixtures.bind({device, world, weapons, effects, audio, 1});
        fixtures.setPlayerCount(2);
        world.setPlayerCount(2);
        FloorRiding::land(players, 0, players[0].actor.position(), world.collision());
        REQUIRE(players[0].floor.object == 0);
        REQUIRE(fixtures.chests().size() == 1);
        REQUIRE(fixtures.chests().chest(0).figure.hasFigure());
        REQUIRE(fixtures.safeRocks().size() == 1);
        REQUIRE(fixtures.safeRocks().standing(0));
        REQUIRE(fixtures.barrels().size() == 1);
        REQUIRE(worldOwner.bindResources(device));
        REQUIRE(fixturesOwner.bindResources());
        if (pads) {
            transporters.bind(device, world.layout(), world.itemScope(), 2, &world.collision());
            REQUIRE(transporters.size() == 2);
            REQUIRE(transporterOwner.bindResources());
        }
        for (usize index = 0; index < players.size(); ++index) {
            runtimes[index] =
                std::make_unique<PlayerRuntimeCheckpoint>(players[index], classes.stats(0));
            ordered[index] = runtimes[index].get();
        }
        combat = std::make_unique<CombatRollbackCheckpoint>(
            ordered, health, levels, attacks, arsenal, effects,
            CombatRollbackCheckpoint::Resources{missileResources, {}, resources});
        boundary = std::make_unique<Boundary>(
            *combat, opponents, std::span<const EnemyMissiles::Resource>{}, &worldOwner,
            &fixturesOwner, nullptr, nullptr, pads ? &transporterOwner : nullptr);
    }
    ~Fixture() {
        boundary.reset();
        combat.reset();
        attacks.clear();
        arsenal.clear();
        opponents.close();
        fixtures.clear();
    }
    Boundary::State state() const {
        auto captured = boundary->checkpoint();
        REQUIRE(captured);
        REQUIRE(captured->stage);
        return std::move(*captured);
    }
    LevelFixtures::Events events() {
        return {.hurt =
                    [this](usize index, f32 damage, HurtKind kind, bool directed,
                           const PlayerImpact& impact) {
                        REQUIRE(index < players.size());
                        PlayerHealth::Events feedback;
                        feedback.block = [this](f32, f32) { outputs.emplace_back("block"); };
                        feedback.sound = [this](std::string_view cue) {
                            outputs.emplace_back(cue);
                        };
                        feedback.cry = feedback.sound;
                        feedback.named = [this](std::string_view cue, f32) {
                            outputs.emplace_back(cue);
                        };
                        feedback.learnBlock = [this] { outputs.emplace_back("learn-block"); };
                        feedback.vibrate = [this](s32) { outputs.emplace_back("vibrate"); };
                        health.hurt(players[index], damage, kind, directed, false, 1, feedback,
                                    impact, false, classes.stats(0));
                        outputs.push_back("hurt:" + std::to_string(players[index].actor.player()));
                    },
                .help =
                    [this](s32 lesson, usize index) {
                        outputs.push_back("help:" + std::to_string(lesson) + ":" +
                                          std::to_string(index));
                        return false;
                    },
                .card =
                    [this](s32 slot, std::string_view name) {
                        outputs.push_back("card:" + std::to_string(slot) + ":" + std::string{name});
                    },
                .opponents =
                    [this](const Vec3& position, f32 radius, f32 damage, std::vector<s32>& reached,
                           u32 flags) {
                        opponents.blast(position, radius, damage, reached, players, flags);
                    },
                .releaseEnemy = [](s32, const Vec3&, s32) { return false; },
                .shatterPotion = [this](s32, const Vec3&) { outputs.emplace_back("potion"); }};
    }
    void step(f32 seconds) {
        world.capturePresentation();
        fixtures.capturePresentation();
        transporters.capturePresentation();
        world.update(seconds);
        transporters.syncFloors(world.collision());
        transporters.animate(seconds);
        for (auto& player : players) {
            FloorRiding::carry(player, world.collision());
        }
        fixtures.update(seconds > 0.02f ? 2 : 1, seconds, players, events());
        effects.update(seconds);
    }
    usize collectChest(f32 seconds) {
        for (s32 tick = 0; tick < 90 && fixtures.chests().chest(0).state != Chests::kOpen; ++tick) {
            step(seconds);
        }
        const auto& chest = fixtures.chests().chest(0);
        REQUIRE(chest.state == Chests::kOpen);
        REQUIRE(chest.opener == 1); // Party entry, not input slot three.
        REQUIRE(chest.held >= 0);
        const usize held = static_cast<usize>(chest.held);
        REQUIRE(world.placedItems().item(held).opener == 3);
        const auto position = world.placedItems().item(held).position;
        const std::array collectors{Collector{Vec3{-100, 0, 0}}, Collector{position, 2, 6}};
        const auto collected = world.collect(device, collectors, [&](const Pickup& pickup) {
            REQUIRE(pickup.collector == 1);
            const auto taken =
                takeItem(players[pickup.collector].actor.save(),
                         {pickup.subtype, pickup.amount, pickup.flags, pickup.strength});
            REQUIRE(taken.took());
            return std::optional<s32>{taken.left};
        });
        REQUIRE(collected.size() == 1);
        REQUIRE(collected.front().item == held);
        REQUIRE(world.placedItems().item(held).taken);
        REQUIRE(fixtures.chests().chest(0).held == static_cast<s32>(held));
        return held;
    }
    u32 stoppedCloud() {
        fixtures.strikeBarrel(0, 1000, 3, players, events());
        auto state = fixturesOwner.checkpoint();
        REQUIRE(state);
        REQUIRE(state->clouds.size() == 1);
        const u32 id = state->clouds[0].effect;
        REQUIRE(id != 0);
        for (s32 tick = 0; tick < 30; ++tick) {
            effects.update(1.0f / 60);
        }
        // The shared effect owner can retire a handle before the fixture's next
        // hazard phase. Such historical links remain allocated, not necessarily
        // playing; demanding live membership would reject this native boundary.
        REQUIRE(effects.playing(id));
        effects.stop(id);
        REQUIRE_FALSE(effects.playing(id));
        REQUIRE(fixturesOwner.checkpoint()->clouds[0].effect == id);
        return id;
    }
    void queuedRockHit() {
        // Generate the pending queue with the real missile/fixture collision
        // path, then transport that exact native pool into the aggregate.
        EnemyMissiles flight;
        flight.launch(EnemyMissileKind::bolt(10, 25, 0.3f), {20, 3, 0}, {40, 3, 0}, 1, nullptr, -1);
        const auto stops = fixtures.missileStops();
        for (s32 tick = 0; tick < 90 && flight.count() != 0; ++tick) {
            flight.update(1.0f / 60, &world.collision(), {}, {}, stops);
        }
        auto missiles = flight.checkpoint();
        REQUIRE(missiles);
        REQUIRE(missiles->rockHits.size() == 1);
        REQUIRE(missiles->rockHits[0].rock == 0);
        REQUIRE(fixtures.safeRocks().strike(0, 10000));
        REQUIRE_FALSE(fixtures.safeRocks().standing(0));
        auto transported = state();
        transported.opponents.missiles = std::move(*missiles);
        REQUIRE(boundary->restore(transported));
        REQUIRE(state().opponents.missiles.rockHits.size() == 1);
    }
};

TEST_CASE("stage transaction restores native chest pickups and moving party support together",
          "[rollback][ordinary-stage-checkpoint]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    Fixture fixture("ordinary-stage-continuation");
    const auto initial = fixture.state();
    REQUIRE(initial.combat.players[0].player == 1);
    REQUIRE(initial.combat.players[1].player == 3);
    const usize held = fixture.collectChest(seconds);
    const auto collected = fixture.state();
    REQUIRE(collected.stage->world.pickups.items[held].taken);
    REQUIRE(collected.stage->fixtures.chests.chests[0].held == static_cast<s32>(held));
    REQUIRE(fixture.players[0].actor.position().y > 0);
    REQUIRE(collected.combat.players[0].floor.object == 0);
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    const auto emitted = fixture.outputs;
    REQUIRE(fixture.boundary->restore(initial));
    CHECK(fixture.state() == initial);
    CHECK(fixture.outputs == emitted);
    REQUIRE(fixture.collectChest(seconds) == held);
    CHECK(fixture.state() == collected);

    std::vector<Boundary::State> expected;
    for (s32 tick = 0; tick < 12; ++tick) {
        fixture.step(seconds);
        expected.push_back(fixture.state());
    }
    REQUIRE(fixture.boundary->restore(collected));
    for (s32 tick = 0; tick < 12; ++tick) {
        CAPTURE(tick, seconds);
        const auto before = fixture.state();
        auto undo = fixture.boundary->prepareRewind();
        REQUIRE(undo);
        fixture.step(seconds);
        CHECK(fixture.state() == expected[static_cast<usize>(tick)]);
        const auto calls = fixture.outputs;
        REQUIRE(fixture.boundary->canRewind(*undo));
        REQUIRE(fixture.boundary->rewind(std::move(*undo)));
        CHECK(fixture.state() == before);
        CHECK(fixture.outputs == calls);
        fixture.step(seconds);
        CHECK(fixture.state() == expected[static_cast<usize>(tick)]);
    }
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
}

TEST_CASE(
    "stage transaction preserves consumed pickup retired effect and destroyed rock references",
    "[rollback][ordinary-stage-checkpoint]") {
    Fixture fixture("ordinary-stage-historical-links");
    const usize held = fixture.collectChest(1.0f / 60);
    const u32 cloud = fixture.stoppedCloud();
    fixture.queuedRockHit();
    const auto baseline = fixture.state();
    REQUIRE(baseline.stage->world.pickups.items[held].taken);
    REQUIRE(baseline.stage->fixtures.clouds.front().effect == cloud);
    REQUIRE(baseline.stage->fixtures.rocks.rocks.front().health == 0);
    REQUIRE(baseline.opponents.missiles.rockHits.front().rock == 0);
    REQUIRE(baseline.valid());
    const auto emitted = fixture.outputs;
    const auto textures = fixture.device.texturesCreated;
    fixture.step(1.0f / 60);
    const auto after = fixture.state();
    REQUIRE(fixture.boundary->restore(baseline));
    CHECK(fixture.state() == baseline);
    CHECK(fixture.outputs == emitted);
    fixture.step(1.0f / 60);
    CHECK(fixture.state() == after);
    CHECK(fixture.device.texturesCreated == textures);

    // Each mutation is scalar-valid in its leaf, but names an absent owner in
    // this exact aggregate. A late refusal must leave every owner untouched.
    REQUIRE(fixture.boundary->restore(baseline));
    for (const s32 mutation : {0, 1, 2, 3}) {
        CAPTURE(mutation);
        auto invalid = baseline;
        switch (mutation) {
        case 0:
            invalid.stage->fixtures.chests.chests[0].held =
                static_cast<s32>(invalid.stage->world.pickups.items.size());
            break;
        case 1: invalid.stage->fixtures.clouds[0].effect = invalid.combat.effects.nextId; break;
        case 2:
            invalid.opponents.missiles.rockHits[0].rock =
                static_cast<s32>(invalid.stage->fixtures.rocks.rocks.size());
            break;
        case 3: invalid.combat.players[0].floor.object = 1234; break;
        }
        REQUIRE(invalid.combat.valid());
        REQUIRE(invalid.opponents.valid());
        REQUIRE(invalid.stage->world.valid());
        REQUIRE(invalid.stage->fixtures.valid());
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture.boundary->prepareRestore(invalid));
        auto issue = Boundary::RestoreIssue::None;
        CHECK_FALSE(fixture.boundary->restore(invalid, &issue));
        CHECK(issue == Boundary::RestoreIssue::State);
        CHECK(fixture.state() == baseline);
        CHECK(fixture.outputs == emitted);
        CHECK(fixture.device.texturesCreated == textures);
    }
}

TEST_CASE("stage blast victim ledgers use party indices rather than sparse input slots",
          "[rollback][ordinary-stage-checkpoint]") {
    Fixture fixture("ordinary-stage-party-indices");
    const auto before = fixture.state();
    auto undo = fixture.boundary->prepareRewind();
    REQUIRE(undo);
    fixture.fixtures.blast(fixture.players[0].actor.followPoint(), 3, 8, fixture.players,
                           fixture.events());
    const auto hit = fixture.state();
    REQUIRE(hit.stage->fixtures.blasts.size() == 1);
    REQUIRE(hit.stage->fixtures.blasts[0].players == std::vector<usize>{0});
    REQUIRE(hit.combat.players[0].save.progress().health <
            before.combat.players[0].save.progress().health);
    REQUIRE(hit.combat.players[0].save.progress().health > 0);
    const auto outputs = fixture.outputs;
    REQUIRE(fixture.boundary->canRewind(*undo));
    REQUIRE(fixture.boundary->rewind(std::move(*undo)));
    CHECK(fixture.state() == before);
    CHECK(fixture.outputs == outputs);
    REQUIRE(fixture.boundary->restore(hit));
    CHECK(fixture.state() == hit);

    auto invalid = hit;
    // Slot three exists, but there are only two party entries. It cannot be a
    // victim index in LevelFixtures' ordered players span.
    invalid.stage->fixtures.blasts[0].players[0] = 3;
    REQUIRE(invalid.stage->fixtures.valid());
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(fixture.boundary->restore(invalid));
    CHECK(fixture.state() == hit);
    CHECK(fixture.outputs == outputs);
}

TEST_CASE("stage transaction rejects unmatched owner pairs and fences child restores",
          "[rollback][ordinary-stage-checkpoint]") {
    Fixture fixture("ordinary-stage-binding");
    Fixture other("ordinary-stage-binding-other");
    const auto before = fixture.state();
    const auto foreignBefore = other.state();
    for (const s32 binding : {0, 1, 2, 3}) {
        CAPTURE(binding);
        auto* world = binding == 1 ? nullptr : &fixture.worldOwner;
        auto* fixtures = binding == 0 ? nullptr : &fixture.fixturesOwner;
        if (binding == 2) {
            world = &other.worldOwner;
        } else if (binding == 3) {
            fixtures = &other.fixturesOwner;
        }
        Boundary invalid{*fixture.combat, fixture.opponents,
                         std::span<const EnemyMissiles::Resource>{}, world, fixtures};
        CHECK_FALSE(invalid.checkpoint());
        CHECK_FALSE(invalid.prepareRestore(before));
        CHECK(fixture.state() == before);
        CHECK(other.state() == foreignBefore);
    }
    auto incomplete = before;
    incomplete.stage.reset();
    auto issue = Boundary::RestoreIssue::None;
    CHECK_FALSE(fixture.boundary->restore(incomplete, &issue));
    CHECK(issue == Boundary::RestoreIssue::Bindings);
    CHECK(fixture.state() == before);

    REQUIRE(fixture.boundary->restore(before, &issue));
    CHECK(issue == Boundary::RestoreIssue::None);

    auto stale = fixture.boundary->prepareRestore(before);
    auto rewind = fixture.boundary->prepareRewind();
    REQUIRE(stale);
    REQUIRE(rewind);
    REQUIRE(fixture.fixturesOwner.restore(before.stage->fixtures));
    CHECK_FALSE(fixture.boundary->canCommit(*stale));
    CHECK_FALSE(fixture.boundary->commitRestore(std::move(*stale)));
    CHECK_FALSE(fixture.boundary->canRewind(*rewind));
    CHECK_FALSE(fixture.boundary->rewind(std::move(*rewind)));
    CHECK(fixture.state() == before);
    CHECK(other.state() == foreignBefore);
}

TEST_CASE("stage transaction includes exact transporter clocks and historical carried placement",
          "[rollback][ordinary-stage-checkpoint][transporter-stage-checkpoint]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto owner = std::make_unique<Fixture>("ordinary-stage-pads", true);
    auto& fixture = *owner;
    for (s32 tick = 0; tick < 9; ++tick) {
        fixture.step(seconds);
    }
    // Native world motion can precede the next pad sync (including scripted
    // holds). The checkpoint must preserve this historical placement exactly.
    const auto padState = fixture.transporterOwner.checkpoint();
    REQUIRE(padState);
    const auto pad = padState->pads[0];
    REQUIRE(pad.support == 0);
    fixture.world.update(seconds);
    const auto before = fixture.state();
    REQUIRE(before.stage->transporters);
    CHECK(before.stage->transporters->pads[0].transform == pad.transform);
    REQUIRE(fixture.world.collision().objectTransform(pad.support));
    CHECK(before.stage->transporters->pads[0].transform !=
          *fixture.world.collision().objectTransform(pad.support) * pad.supportLocal);
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    auto rewind = fixture.boundary->prepareRewind();
    REQUIRE(rewind);
    fixture.step(seconds);
    const auto after = fixture.state();
    CHECK(after.stage->transporters != before.stage->transporters);
    REQUIRE(fixture.boundary->canRewind(*rewind));
    REQUIRE(fixture.boundary->rewind(std::move(*rewind)));
    CHECK(fixture.state() == before);
    fixture.step(seconds);
    CHECK(fixture.state() == after);
    REQUIRE(fixture.boundary->restore(before));
    CHECK(fixture.state() == before);
    fixture.step(seconds);
    CHECK(fixture.state() == after);
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
}

TEST_CASE(
    "transporter stage refuses mismatched participants resources links and stale plans atomically",
    "[rollback][ordinary-stage-checkpoint][transporter-stage-checkpoint]") {
    const auto owner = std::make_unique<Fixture>("ordinary-stage-pad-validation", true);
    const auto otherOwner = std::make_unique<Fixture>("ordinary-stage-pad-foreign", true);
    auto& fixture = *owner;
    auto& other = *otherOwner;
    const auto before = fixture.state();
    const auto foreign = other.state();
    for (const s32 binding : {0, 1}) {
        CAPTURE(binding);
        Boundary invalid{*fixture.combat,
                         fixture.opponents,
                         {},
                         binding == 0 ? nullptr : &fixture.worldOwner,
                         binding == 0 ? nullptr : &fixture.fixturesOwner,
                         nullptr,
                         nullptr,
                         binding == 0 ? &fixture.transporterOwner : &other.transporterOwner};
        CHECK_FALSE(invalid.checkpoint());
        CHECK_FALSE(invalid.prepareRestore(before));
        CHECK(fixture.state() == before);
        CHECK(other.state() == foreign);
    }
    Boundary absent{
        *fixture.combat, fixture.opponents, {}, &fixture.worldOwner, &fixture.fixturesOwner};
    CHECK_FALSE(absent.prepareRestore(before));
    for (const s32 mutation : {0, 1, 2, 3, 4}) {
        CAPTURE(mutation);
        auto invalid = before;
        invalid.combat.players[0].save.gold += 17;
        auto issue = Boundary::RestoreIssue::None;
        if (mutation == 0) {
            invalid.stage->transporters.reset();
        } else if (mutation == 1) {
            invalid.stage->transporters->catalog[0] ^= 1;
        } else if (mutation == 2) {
            invalid.stage->transporters->pads[0].support = 12345;
        } else if (mutation == 3) {
            invalid.stage->transporters->pads[0].destination = 0;
        } else {
            // Structurally coherent fake support still is not part of this
            // trusted world's collision-owner catalog.
            invalid.stage->transporters->pads[0].support = 12345;
            invalid.stage->world.core.moving.push_back({12345, Mat4{1}});
            REQUIRE(invalid.valid());
        }
        CHECK_FALSE(fixture.boundary->restore(invalid, &issue));
        CHECK(issue == (mutation == 0   ? Boundary::RestoreIssue::Bindings
                        : mutation == 1 ? Boundary::RestoreIssue::Transporters
                        : mutation == 4 ? Boundary::RestoreIssue::Supports
                                        : Boundary::RestoreIssue::State));
        CHECK(fixture.state() == before);
    }
    auto planned = fixture.boundary->prepareRestore(before);
    auto rewind = fixture.boundary->prepareRewind();
    REQUIRE(planned);
    REQUIRE(rewind);
    REQUIRE(fixture.transporterOwner.restore(*before.stage->transporters));
    CHECK_FALSE(fixture.boundary->canCommit(*planned));
    CHECK_FALSE(fixture.boundary->commitRestore(std::move(*planned)));
    CHECK_FALSE(fixture.boundary->canRewind(*rewind));
    CHECK_FALSE(fixture.boundary->rewind(std::move(*rewind)));
    CHECK(fixture.state() == before);

    planned = fixture.boundary->prepareRestore(before);
    rewind = fixture.boundary->prepareRewind();
    REQUIRE(planned);
    REQUIRE(rewind);
    fixture.transporters.clear();
    CHECK_FALSE(fixture.boundary->commitRestore(std::move(*planned)));
    CHECK_FALSE(fixture.boundary->rewind(std::move(*rewind)));
    // The deliberately cleared pad owner cannot be recaptured. Every other
    // participant must still retain its complete preflight state.
    CHECK(fixture.combat->checkpoint() == before.combat);
    CHECK(fixture.worldOwner.checkpoint() == before.stage->world);
    CHECK(fixture.fixturesOwner.checkpoint() == before.stage->fixtures);
    CHECK(other.state() == foreign);
}
} // namespace
