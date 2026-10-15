#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/PlayerActor.h"
#include "game/screens/OpponentCheckpoint.h"
#include "game/screens/OpponentSimulation.h"
#include "game/world/BodyCollision.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Island {
    test::FakeRenderDevice device;
    WorldLayout layout;
    WorldCollision collision;
    Enemies enemies;
    Generators generators;
    EnemyMissiles missiles{45};
    OpponentSimulation simulation{generators, enemies, missiles};
    OpponentCheckpoint diagnostics{generators, enemies, missiles};
    std::array<EnemyView, 1> players;
    std::vector<EnemyMissiles::Resource> resources;

    explicit Island(bool brood = false) {
        const auto root =
            test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
        const auto directory = test::scratchDirectory("opponent-simulation");
        writeTextFile(directory / "world.json",
                      brood ? R"({"objects":[{"name":"FLOOR","position":[0,0,0]}],
            "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":10}],
            "itemInstances":[{"info":0,"minPlayers":1,"position":[60,0,60],
                              "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})"
                            : R"({"objects":[{"name":"FLOOR","position":[0,0,0]}]})");
        REQUIRE(layout.load(directory));
        collision.build(
            {{{0, 1, 0}, {Vec3{-200, 0, -200}, Vec3{200, 0, 200}, Vec3{200, 0, -200}}, 0, 4},
             {{0, 1, 0}, {Vec3{-200, 0, -200}, Vec3{-200, 0, 200}, Vec3{200, 0, 200}}, 0, 4}});
        enemies.open(device, root, &collision, Enemies::kMost, {}, 1125);
        REQUIRE(enemies.loadKind(kGruntKind));
        REQUIRE(generators.bind(device, layout, enemies, &collision, {}, 1));
        for (s32 slot = 0; slot < 3; ++slot) {
            const auto* model = enemies.projectileModel(kGruntKind, slot);
            if (model != nullptr && std::ranges::none_of(resources, [model](const auto& resource) {
                    return resource.model == model;
                })) {
                resources.push_back({static_cast<u32>(resources.size()) + 1, model});
            }
        }
        players[0].player = 0;
        players[0].position = {0, 0, 0};
        REQUIRE(diagnostics.checkpoint(resources));
    }

    OpponentSimulation::Frame frame() const {
        OpponentSimulation::Frame result;
        result.players = players;
        result.collision = &collision;
        return result;
    }

    s32 spawn(const Vec3& position, s32 way = kChaseWay, s32 tier = 1) {
        const auto slot = enemies.spawn({.kind = kGruntKind,
                                         .tier = tier,
                                         .algorithm = way,
                                         .position = position,
                                         .placed = true},
                                        players);
        REQUIRE(slot);
        return *slot;
    }

    // The pre-extraction LevelOpponents phase is kept as an independent oracle.
    void native(const OpponentSimulation::Frame& frame,
                const EnemyMissiles::SceneryBlast& scenery = {}) {
        generators.update(frame.ticks, enemies, frame.players, frame.fixtures, frame.timeStopped);
        auto boxes = generators.enemyObstacles();
        boxes.insert(boxes.end(), frame.fixtures.begin(), frame.fixtures.end());
        enemies.update(frame.ticks, frame.seconds, frame.players, boxes, &missiles,
                       frame.missileSpeed, frame.timeStopped);
        std::vector<MissileStop> stops;
        for (const auto& box : generators.obstacles()) {
            stops.push_back(MissileStop::of(box));
        }
        if (frame.stops.empty()) {
            for (const auto& box : frame.fixtures) {
                stops.push_back(MissileStop::of(box));
            }
        } else {
            stops.insert(stops.end(), frame.stops.begin(), frame.stops.end());
        }
        auto targets = enemies.targets();
        for (auto target : frame.critters) {
            target.id += 3000;
            targets.push_back(target);
        }
        for (auto target : frame.bosses) {
            target.id += 4000;
            targets.push_back(target);
        }
        missiles.update(frame.seconds, frame.collision, frame.players, targets, stops, scenery);
    }
};

TEST_CASE("opponent island preserves the native generator enemy and missile phase",
          "[diagnostics][opponent-simulation][assets]") {
    Island island(true);
    island.spawn({-5, 0, -18}, kThrowWay, kArcherStrength);
    const auto initial = island.diagnostics.checkpoint(island.resources);
    REQUIRE(initial);
    const auto uploads = island.device.texturesCreated;
    Obstacle box;
    box.centre = {5, 0, 6};
    box.halfAcross = 2;
    box.halfAlong = 1;
    box.height = 6;
    const std::array fixtures{box};
    const std::array stops{MissileStop::of(box)};
    const std::array critters{
        MissileTarget{.id = 2, .base = {20, 0, 20}, .radius = 1, .height = 4}};
    const std::array bosses{MissileTarget{.id = 3, .base = {40, 0, 40}, .radius = 2, .height = 8}};
    auto frame = island.frame();
    frame.fixtures = fixtures;
    frame.critters = critters;
    frame.bosses = bosses;
    std::vector<OpponentState> expected;
    for (s32 tick = 0; tick < 150; ++tick) {
        frame.timeStopped = tick >= 40 && tick < 50;
        frame.stops =
            tick % 2 == 0 ? std::span<const MissileStop>{stops} : std::span<const MissileStop>{};
        island.native(frame);
        const auto state = island.diagnostics.checkpoint(island.resources);
        REQUIRE(state);
        expected.push_back(*state);
    }
    REQUIRE(expected.back().enemies.nextInstance > initial->enemies.nextInstance);
    REQUIRE(expected.back().missiles.nextInstance > initial->missiles.nextInstance);
    REQUIRE(island.diagnostics.restore(*initial, island.resources));
    for (s32 tick = 0; tick < 150; ++tick) {
        CAPTURE(tick);
        frame.timeStopped = tick >= 40 && tick < 50;
        frame.stops =
            tick % 2 == 0 ? std::span<const MissileStop>{stops} : std::span<const MissileStop>{};
        island.simulation.advance(frame);
        CHECK(island.diagnostics.checkpoint(island.resources) ==
              expected[static_cast<usize>(tick)]);
    }
    const auto roster = OpponentSimulation::targets(island.enemies, critters, bosses);
    REQUIRE(roster.size() >= 2);
    CHECK(roster[roster.size() - 2].id == 3002);
    CHECK(roster.back().id == 4003);
    CHECK(island.device.texturesCreated == uploads);
}

TEST_CASE("birth publication occurs after native AI and before missile movement",
          "[opponent-simulation][enemy-birth-emission][assets]") {
    Island island;
    island.spawn({0, 0, 40});
    island.missiles.launch(EnemyMissileKind::arrow(), {80, 10, 80}, {80, 10, 100}, 1, nullptr, -1);
    const auto baseline = island.diagnostics.checkpoint(island.resources);
    REQUIRE(baseline);
    REQUIRE_FALSE(baseline->enemies.births.empty());
    REQUIRE_FALSE(baseline->missiles.missiles.empty());
    usize called = 0;
    island.simulation.advance(island.frame(), {}, [&] {
        ++called;
        const auto afterAi = island.enemies.checkpoint();
        REQUIRE(afterAi);
        CHECK(afterAi->pendingTicks != baseline->enemies.pendingTicks);
        CHECK(island.missiles.checkpoint(island.resources) == baseline->missiles);
        CHECK(island.enemies.takeBirths() == baseline->enemies.births);
    });
    CHECK(called == 1);
    const auto advanced = island.diagnostics.checkpoint(island.resources);
    REQUIRE(advanced);
    CHECK(advanced->enemies.births.empty());
    CHECK(advanced->missiles != baseline->missiles);
}

TEST_CASE("opponent bundle restores nothing if any one checkpoint or preparation is invalid",
          "[diagnostics][opponent-simulation][assets]") {
    Island island(true);
    island.spawn({0, 0, 15});
    const auto initial = island.diagnostics.checkpoint(island.resources);
    REQUIRE(initial);
    island.simulation.advance(island.frame());
    const auto current = island.diagnostics.checkpoint(island.resources);
    REQUIRE(current);
    auto invalid = *initial;
    const s32 pool = GENERATE(0, 1, 2);
    if (pool == 0) {
        invalid.generators.scales.rate = std::numeric_limits<f32>::quiet_NaN();
    } else if (pool == 1) {
        invalid.enemies.pendingTicks = 2;
    } else {
        invalid.missiles.shrink = std::numeric_limits<f32>::quiet_NaN();
    }
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(island.diagnostics.prepareRestore(invalid, island.resources));
    CHECK_FALSE(island.diagnostics.restore(invalid, island.resources));
    CHECK(island.diagnostics.checkpoint(island.resources) == current);

    auto prepared = island.diagnostics.prepareRestore(*initial, island.resources);
    REQUIRE(prepared);
    CHECK(island.diagnostics.checkpoint(island.resources) == current);
    // Invalidate only one component's token before the joint commit. The other
    // two must remain at their current state, not partially return to initial.
    if (pool == 0) {
        REQUIRE(island.generators.restore(current->generators));
    } else if (pool == 1) {
        REQUIRE(island.enemies.restore(current->enemies));
    } else {
        REQUIRE(island.missiles.restore(current->missiles, island.resources));
    }
    CHECK_FALSE(island.diagnostics.canCommit(*prepared));
    CHECK_FALSE(island.diagnostics.commitRestore(std::move(*prepared)));
    CHECK(island.diagnostics.checkpoint(island.resources) == current);

    prepared = island.diagnostics.prepareRestore(*initial, island.resources);
    REQUIRE(prepared);
    REQUIRE(island.diagnostics.commitRestore(std::move(*prepared)));
    CHECK_FALSE(island.diagnostics.commitRestore(std::move(*prepared)));
    CHECK(island.diagnostics.checkpoint(island.resources) == initial);
}

TEST_CASE("opponent bundle validates cross-pool generator references without rejecting orphans",
          "[diagnostics][opponent-simulation][assets]") {
    Island island(true);
    const auto slot = island.enemies.spawn(
        {.kind = kGruntKind, .position = {0, 0, 15}, .generator = 0, .placed = true},
        island.players);
    REQUIRE(slot);
    const auto initial = island.diagnostics.checkpoint(island.resources);
    REQUIRE(initial);
    REQUIRE(initial->generators.generators.size() == 1);
    REQUIRE_FALSE(initial->enemies.generatorEvents.empty());

    auto invalid = *initial;
    invalid.enemies.enemies[static_cast<usize>(*slot)].generator = 1;
    REQUIRE(invalid.enemies.valid());
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(island.diagnostics.prepareRestore(invalid, island.resources));
    CHECK_FALSE(island.diagnostics.restore(invalid, island.resources));
    CHECK(island.diagnostics.checkpoint(island.resources) == initial);

    // Individually valid native owners must not make an invalid cross-pool
    // reference acceptable on the optimized local-capture path either.
    REQUIRE(island.enemies.restore(invalid.enemies));
    CHECK_FALSE(island.diagnostics.checkpoint(island.resources));
    CHECK(island.enemies.checkpoint() == invalid.enemies);
    REQUIRE(island.enemies.restore(initial->enemies));
    CHECK(island.diagnostics.checkpoint(island.resources) == initial);

    invalid = *initial;
    invalid.enemies.generatorEvents.push_back({1, EnemyGeneratorEvent::Kind::Detached});
    REQUIRE(invalid.enemies.valid());
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(island.diagnostics.prepareRestore(invalid, island.resources));
    CHECK_FALSE(island.diagnostics.restore(invalid, island.resources));
    CHECK(island.diagnostics.checkpoint(island.resources) == initial);

    // Native destruction retains the generator slot. generatorGone detaches its
    // live children without erasing the earlier Born event waiting for do_items.
    const auto destroyed = island.generators.strike(0, 100000, 0);
    REQUIRE(destroyed);
    REQUIRE(destroyed->destroyed);
    island.enemies.generatorGone(0);
    const auto orphan = island.diagnostics.checkpoint(island.resources);
    REQUIRE(orphan);
    CHECK(orphan->enemies.enemies[static_cast<usize>(*slot)].generator == -1);
    CHECK(orphan->generators.generators.front().state == 0);
    CHECK(orphan->enemies.generatorEvents == initial->enemies.generatorEvents);
    REQUIRE(island.diagnostics.restore(*initial, island.resources));
    REQUIRE(island.diagnostics.restore(*orphan, island.resources));
    CHECK(island.diagnostics.checkpoint(island.resources) == orphan);
}

TEST_CASE("opponent capture equals independently validated native children after each phase",
          "[diagnostics][opponent-simulation][checkpoint-capture-validation][assets]") {
    Island island(true);
    island.spawn({0, 0, 15});
    for (s32 tick = 0; tick < 12; ++tick) {
        CAPTURE(tick);
        island.simulation.advance(island.frame());
        const auto generators = island.generators.checkpoint();
        const auto enemies = island.enemies.checkpoint();
        const auto missiles = island.missiles.checkpoint(island.resources);
        REQUIRE(generators);
        REQUIRE(enemies);
        REQUIRE(missiles);
        const OpponentState expected{*generators, *enemies, *missiles};
        REQUIRE(expected.valid());
        const auto captured = island.diagnostics.checkpoint(island.resources);
        REQUIRE(captured);
        CHECK(captured->valid());
        CHECK(*captured == expected);
    }
}

TEST_CASE("opponent transaction preserves the exhausted native missile identity sentinel",
          "[diagnostics][opponent-simulation][assets]") {
    Island island(true);
    auto exhausted = island.diagnostics.checkpoint(island.resources);
    REQUIRE(exhausted);
    exhausted->missiles.nextInstance = 0;
    REQUIRE(exhausted->valid());
    auto prepared = island.diagnostics.prepareRestore(*exhausted, island.resources);
    REQUIRE(prepared);
    REQUIRE(island.diagnostics.canCommit(*prepared));
    REQUIRE(island.diagnostics.commitRestore(std::move(*prepared)));
    CHECK(island.diagnostics.checkpoint(island.resources) == exhausted);
    island.missiles.launch(EnemyMissileKind::arrow(), {0, 3, 10}, {0, 3, 20}, 1, nullptr, -1);
    CHECK(island.missiles.count() == 0);
    CHECK(island.diagnostics.checkpoint(island.resources) == exhausted);
}

TEST_CASE("native opponent motion updates player collision",
          "[diagnostics][opponent-simulation][assets]") {
    Island island;
    island.players[0].position = {0, 0, -100};
    const s32 slot = island.spawn({0, 0, 10});
    for (s32 tick = 0; tick < 60; ++tick) {
        island.native(island.frame());
    }
    island.players[0].position = {0, 0, 0};
    const auto baseline = island.diagnostics.checkpoint(island.resources);
    REQUIRE(baseline);
    const auto frozen = island.enemies.targets();
    REQUIRE(frozen.size() == 1);
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    const Vec3 desired{0, 0, 12};
    const Vec3 stale = BodyCollision::resolveMovement(actor, actor.position(), desired, frozen, {},
                                                      &island.collision);
    bool collisionChanged = false;
    for (s32 tick = 0; tick < 90; ++tick) {
        const auto before = island.diagnostics.checkpoint(island.resources);
        REQUIRE(before);
        island.native(island.frame());
        const auto native = island.diagnostics.checkpoint(island.resources);
        REQUIRE(native);
        const auto nativeTargets = island.enemies.targets();
        const Vec3 expected = BodyCollision::resolveMovement(actor, actor.position(), desired,
                                                             nativeTargets, {}, &island.collision);
        REQUIRE(island.diagnostics.restore(*before, island.resources));
        island.simulation.advance(island.frame());
        CHECK(island.diagnostics.checkpoint(island.resources) == native);
        const Vec3 resolved = BodyCollision::resolveMovement(
            actor, actor.position(), desired, island.enemies.targets(), {}, &island.collision);
        CHECK(glm::distance(resolved, expected) < 0.00001f);
        collisionChanged = collisionChanged || glm::distance(resolved, stale) > 0.01f;
    }
    REQUIRE(collisionChanged);
    CHECK(island.enemies.positionOf(slot) !=
          baseline->enemies.enemies[static_cast<usize>(slot)].position);
    // Body resolution changes actual movement without mutating the character save.
    const auto save = actor.save().toJson();
    actor.place(BodyCollision::resolveMovement(actor, actor.position(), desired,
                                               island.enemies.targets(), {}, &island.collision));
    CHECK(glm::distance(actor.position(), stale) > 0.01f);
    CHECK(actor.save().toJson() == save);
    REQUIRE(island.diagnostics.restore(*baseline, island.resources));
    CHECK(island.diagnostics.checkpoint(island.resources) == baseline);
}

TEST_CASE("clearing one pool invalidates an entire prepared opponent transaction",
          "[diagnostics][opponent-simulation][assets]") {
    Island island(true);
    island.spawn({0, 0, 15});
    island.missiles.launch(EnemyMissileKind::arrow(), {20, 4, 20}, {20, 4, 40}, 1, nullptr, -1);
    const auto initial = island.diagnostics.checkpoint(island.resources);
    REQUIRE(initial);
    auto prepared = island.diagnostics.prepareRestore(*initial, island.resources);
    REQUIRE(prepared);
    island.simulation.advance(island.frame());
    island.missiles.clear();
    const auto cleared = island.diagnostics.checkpoint(island.resources);
    REQUIRE(cleared);
    CHECK_FALSE(island.diagnostics.canCommit(*prepared));
    CHECK_FALSE(island.diagnostics.commitRestore(std::move(*prepared)));
    CHECK(island.diagnostics.checkpoint(island.resources) == cleared);
    CHECK(cleared->enemies.nextInstance > initial->enemies.nextInstance);
    CHECK(cleared->missiles.missiles.empty());
}

TEST_CASE("opponent phase advances native flights and chasing bodies",
          "[diagnostics][opponent-simulation][assets]") {
    Island island;
    island.players[0].position = {0, 0, -100};
    const auto slot = island.spawn({0, 0, 20});
    for (s32 tick = 0; tick < 60; ++tick) {
        island.native(island.frame());
    }
    island.players[0].position = {0, 0, 0};
    island.missiles.launch(EnemyMissileKind::arrow(), {80, 10, 80}, {80, 10, 100}, 1, nullptr, -1);
    const auto initial = island.diagnostics.checkpoint(island.resources);
    REQUIRE(initial);
    REQUIRE(initial->missiles.missiles.size() == 1);
    for (s32 tick = 0; tick < 12; ++tick) {
        CAPTURE(tick);
        const auto before = island.diagnostics.checkpoint(island.resources);
        REQUIRE(before);
        island.native(island.frame());
        const auto expected = island.diagnostics.checkpoint(island.resources);
        REQUIRE(expected);
        REQUIRE(island.diagnostics.restore(*before, island.resources));
        island.simulation.advance(island.frame());
        CHECK(island.diagnostics.checkpoint(island.resources) == expected);
    }
    const auto result = island.diagnostics.checkpoint(island.resources);
    REQUIRE(result);
    REQUIRE(result->missiles.missiles.size() == 1);
    CHECK(result->missiles.missiles.front().position !=
          initial->missiles.missiles.front().position);
    CHECK(result->enemies.enemies[static_cast<usize>(slot)].position !=
          initial->enemies.enemies[static_cast<usize>(slot)].position);
}

TEST_CASE("opponent phase preserves native shooting minds and pending throws",
          "[diagnostics][opponent-simulation][assets]") {
    Island island(true);
    SECTION("shooting native mind") {
        const auto way = GENERATE(kThrowWay, kBombWay, kSkirmishWay, kSkirmishBombWay,
                                  kStandCastWay, kRangeCastWay, kCastWay);
        island.spawn({0, 0, 20}, way, kArcherStrength);
    }
    SECTION("previously requested throw survives a change to a chasing mind") {
        const auto slot = island.spawn({0, 0, 20});
        auto state = island.diagnostics.checkpoint(island.resources);
        REQUIRE(state);
        auto& animator = state->enemies.enemies[static_cast<usize>(slot)].animator;
        REQUIRE(animator);
        animator->requested = EnemyAction::Throw;
        REQUIRE(island.diagnostics.restore(*state, island.resources));
    }
    SECTION("brood can create a shooting mind in the same update") {
        auto state = island.diagnostics.checkpoint(island.resources);
        REQUIRE(state);
        REQUIRE(state->generators.generators.size() == 1);
        state->generators.generators.front().algorithm = kThrowWay;
        REQUIRE(island.diagnostics.restore(*state, island.resources));
    }
    const auto before = island.diagnostics.checkpoint(island.resources);
    REQUIRE(before);
    island.native(island.frame());
    const auto expected = island.diagnostics.checkpoint(island.resources);
    REQUIRE(expected);
    REQUIRE(island.diagnostics.restore(*before, island.resources));
    island.simulation.advance(island.frame());
    CHECK(island.diagnostics.checkpoint(island.resources) == expected);
}

TEST_CASE("archer and bomber releases match native phase through the first impact",
          "[diagnostics][opponent-simulation][enemy-launch][assets]") {
    const bool bred = GENERATE(false, true);
    const bool bomb = GENERATE(false, true);
    const s32 ticks = GENERATE(1, 2);
    CAPTURE(bred, bomb, ticks);
    Island island(bred);
    const s32 way = bomb ? kBombWay : kThrowWay;
    const s32 strength = bomb ? kBomberStrength : kArcherStrength;
    if (bred) {
        // Humanoids emerge ahead of the hut. Put the target on that same side
        // so the native launch sweep does not terminate inside the generator.
        island.players[0].position = {60, 0, 85};
        auto state = island.diagnostics.checkpoint(island.resources);
        REQUIRE(state);
        state->generators.generators.front().algorithm = way;
        state->generators.generators.front().tier = strength;
        REQUIRE(island.diagnostics.restore(*state, island.resources));
    } else {
        island.spawn({0, 0, -20}, way, strength);
    }
    auto frame = island.frame();
    frame.ticks = ticks;
    frame.seconds = static_cast<f32>(ticks) / 60;
    const auto baseline = island.diagnostics.checkpoint(island.resources);
    REQUIRE(baseline);
    const auto uploads = island.device.texturesCreated;
    bool released = false;
    bool reachedBoundary = false;
    for (s32 tick = 0; tick < 600; tick += ticks) {
        CAPTURE(bred, bomb, tick, ticks);
        const auto before = island.diagnostics.checkpoint(island.resources);
        REQUIRE(before);
        island.native(frame);
        const auto expected = island.diagnostics.checkpoint(island.resources);
        REQUIRE(expected);
        REQUIRE(island.diagnostics.restore(*before, island.resources));
        island.simulation.advance(frame);
        CHECK(island.diagnostics.checkpoint(island.resources) == expected);
        if (!expected->missiles.hits.empty() || !expected->enemies.blows.empty()) {
            reachedBoundary = true;
            break;
        }
        if (expected->missiles.nextInstance > baseline->missiles.nextInstance) {
            released = true;
            REQUIRE_FALSE(expected->missiles.missiles.empty());
            CHECK(expected->missiles.missiles.front().kind.slot ==
                  (bomb ? EnemyMissileKind::kBomb : EnemyMissileKind::kArrow));
        }
    }
    CHECK(released);
    CHECK(reachedBoundary);
    CHECK(island.device.texturesCreated == uploads);
}

TEST_CASE("native opponent phase still dispatches scenery in its missile phase",
          "[diagnostics][opponent-simulation][assets]") {
    Island island;
    island.missiles.burst({0, 0, 0}, 10, 5, 0, 1, -1);
    const auto before = island.diagnostics.checkpoint(island.resources);
    REQUIRE(before);
    usize called = 0;
    island.simulation.advance(island.frame(), [&](const auto&, auto& contacts) {
        ++called;
        contacts.push_back(12);
    });
    CHECK(called == 1);
    const auto state = island.diagnostics.checkpoint(island.resources);
    REQUIRE(state);
    REQUIRE(state->missiles.bursts.size() == 1);
    CHECK(state->missiles.bursts.front().scenery == std::vector<s32>{12});
    REQUIRE(island.diagnostics.restore(*before, island.resources));
    CHECK(called == 1);
}

PlacedEnemies::Placement ordinaryPlacement(Vec3 position, s32 kind = kGruntKind) {
    PlacedEnemies::Placement row;
    row.kind = kind;
    row.spawn.kind = kind;
    row.spawn.algorithm = kChaseWay;
    row.spawn.position = position;
    row.spawn.placed = true;
    row.spawn.priority = EnemySpawn::Priority::Visible;
    row.viewRadius = 1;
    return row;
}

TEST_CASE("ordinary placements run their native watch phase before generators and swarm",
          "[diagnostics][opponent-simulation][opponent-placements][assets]") {
    Island island(true);
    PlacedEnemies placements;
    placements.bind({ordinaryPlacement({0, 0, 20}), ordinaryPlacement({8, 0, 25}),
                     ordinaryPlacement({0, 0, 80})});
    placements.observe(ViewVolume{}, {});
    OpponentSimulation combined{island.generators, island.enemies, island.missiles, &placements};
    OpponentCheckpoint diagnostics{island.generators, island.enemies, island.missiles, &placements};
    const auto initial = diagnostics.checkpoint(island.resources);
    REQUIRE(initial);
    REQUIRE(initial->placements);
    const auto uploads = island.device.texturesCreated;
    std::vector<OpponentState> expected;
    for (s32 tick = 0; tick < 12; ++tick) {
        // Independent native LevelOpponents::watch -> update order; no replay
        // helper participates in the expected activation or motion.
        placements.activate([&](const auto& row) {
            island.enemies.spawn(row.spawn, {}, island.generators.obstacles());
        });
        island.native(island.frame());
        const auto state = diagnostics.checkpoint(island.resources);
        REQUIRE(state);
        expected.push_back(*state);
    }
    REQUIRE(expected.front().placements->pending == std::vector<usize>{2});
    REQUIRE(expected.front().enemies.nextInstance > initial->enemies.nextInstance);
    REQUIRE(diagnostics.restore(*initial, island.resources));
    for (usize tick = 0; tick < expected.size(); ++tick) {
        CAPTURE(tick);
        combined.advance(island.frame());
        CHECK(diagnostics.checkpoint(island.resources) == expected[tick]);
    }
    CHECK(island.device.texturesCreated == uploads);
}

TEST_CASE("stale placement preparation rejects the whole opponent transaction",
          "[diagnostics][opponent-simulation][opponent-placements][assets]") {
    Island island(true);
    PlacedEnemies placements;
    placements.bind({ordinaryPlacement({0, 0, 20})});
    OpponentCheckpoint diagnostics{island.generators, island.enemies, island.missiles, &placements};
    const auto initial = diagnostics.checkpoint(island.resources);
    REQUIRE(initial);
    auto prepared = diagnostics.prepareRestore(*initial, island.resources);
    REQUIRE(prepared);
    island.native(island.frame());
    placements.observe(ViewVolume{}, {1, 0, 0});
    const auto current = diagnostics.checkpoint(island.resources);
    REQUIRE(current);
    CHECK_FALSE(diagnostics.canCommit(*prepared));
    CHECK_FALSE(diagnostics.commitRestore(std::move(*prepared)));
    CHECK(diagnostics.checkpoint(island.resources) == current);
    auto forged = *current;
    forged.placements->roster.front().spawn.position.x += 1;
    REQUIRE(forged.valid());
    CHECK_FALSE(diagnostics.prepareRestore(forged, island.resources));
    CHECK(diagnostics.checkpoint(island.resources) == current);
    forged = *current;
    forged.placements.reset();
    CHECK_FALSE(diagnostics.prepareRestore(forged, island.resources));
    CHECK_FALSE(island.diagnostics.prepareRestore(*current, island.resources));
}

} // namespace
