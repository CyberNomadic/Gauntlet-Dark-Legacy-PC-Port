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
        REQUIRE(simulation.checkpoint(resources));
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
          "[rollback][opponent-simulation][assets]") {
    Island island(true);
    island.spawn({-5, 0, -18}, kThrowWay, kArcherStrength);
    const auto initial = island.simulation.checkpoint(island.resources);
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
    std::vector<OpponentSimulation::Checkpoint> expected;
    for (s32 tick = 0; tick < 150; ++tick) {
        frame.timeStopped = tick >= 40 && tick < 50;
        frame.stops =
            tick % 2 == 0 ? std::span<const MissileStop>{stops} : std::span<const MissileStop>{};
        island.native(frame);
        const auto state = island.simulation.checkpoint(island.resources);
        REQUIRE(state);
        expected.push_back(*state);
    }
    REQUIRE(expected.back().enemies.nextInstance > initial->enemies.nextInstance);
    REQUIRE(expected.back().missiles.nextInstance > initial->missiles.nextInstance);
    REQUIRE(island.simulation.restore(*initial, island.resources));
    for (s32 tick = 0; tick < 150; ++tick) {
        CAPTURE(tick);
        frame.timeStopped = tick >= 40 && tick < 50;
        frame.stops =
            tick % 2 == 0 ? std::span<const MissileStop>{stops} : std::span<const MissileStop>{};
        island.simulation.advance(frame);
        CHECK(island.simulation.checkpoint(island.resources) == expected[static_cast<usize>(tick)]);
    }
    const auto roster = OpponentSimulation::targets(island.enemies, critters, bosses);
    REQUIRE(roster.size() >= 2);
    CHECK(roster[roster.size() - 2].id == 3002);
    CHECK(roster.back().id == 4003);
    CHECK(island.device.texturesCreated == uploads);
}

TEST_CASE("opponent bundle restores nothing if any one checkpoint or preparation is invalid",
          "[rollback][opponent-simulation][assets]") {
    Island island(true);
    island.spawn({0, 0, 15});
    const auto initial = island.simulation.checkpoint(island.resources);
    REQUIRE(initial);
    island.simulation.advance(island.frame());
    const auto current = island.simulation.checkpoint(island.resources);
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
    CHECK_FALSE(island.simulation.prepareRestore(invalid, island.resources));
    CHECK_FALSE(island.simulation.restore(invalid, island.resources));
    CHECK(island.simulation.checkpoint(island.resources) == current);

    auto prepared = island.simulation.prepareRestore(*initial, island.resources);
    REQUIRE(prepared);
    CHECK(island.simulation.checkpoint(island.resources) == current);
    // Invalidate only one component's token before the joint commit. The other
    // two must remain at their current state, not partially return to initial.
    if (pool == 0) {
        REQUIRE(island.generators.restore(current->generators));
    } else if (pool == 1) {
        REQUIRE(island.enemies.restore(current->enemies));
    } else {
        REQUIRE(island.missiles.restore(current->missiles, island.resources));
    }
    CHECK_FALSE(island.simulation.canCommit(*prepared));
    CHECK_FALSE(island.simulation.commitRestore(std::move(*prepared)));
    CHECK(island.simulation.checkpoint(island.resources) == current);

    prepared = island.simulation.prepareRestore(*initial, island.resources);
    REQUIRE(prepared);
    REQUIRE(island.simulation.commitRestore(std::move(*prepared)));
    CHECK_FALSE(island.simulation.commitRestore(std::move(*prepared)));
    CHECK(island.simulation.checkpoint(island.resources) == initial);
}

TEST_CASE("opponent bundle validates cross-pool generator references without rejecting orphans",
          "[rollback][opponent-simulation][assets]") {
    Island island(true);
    const auto slot = island.enemies.spawn(
        {.kind = kGruntKind, .position = {0, 0, 15}, .generator = 0, .placed = true},
        island.players);
    REQUIRE(slot);
    const auto initial = island.simulation.checkpoint(island.resources);
    REQUIRE(initial);
    REQUIRE(initial->generators.generators.size() == 1);
    REQUIRE_FALSE(initial->enemies.generatorEvents.empty());

    auto invalid = *initial;
    invalid.enemies.enemies[static_cast<usize>(*slot)].generator = 1;
    REQUIRE(invalid.enemies.valid());
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(island.simulation.prepareRestore(invalid, island.resources));
    CHECK_FALSE(island.simulation.restore(invalid, island.resources));
    CHECK(island.simulation.checkpoint(island.resources) == initial);

    invalid = *initial;
    invalid.enemies.generatorEvents.push_back({1, EnemyGeneratorEvent::Kind::Detached});
    REQUIRE(invalid.enemies.valid());
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(island.simulation.prepareRestore(invalid, island.resources));
    CHECK_FALSE(island.simulation.restore(invalid, island.resources));
    CHECK(island.simulation.checkpoint(island.resources) == initial);

    // Native destruction retains the generator slot. generatorGone detaches its
    // live children without erasing the earlier Born event waiting for do_items.
    const auto destroyed = island.generators.strike(0, 100000, 0);
    REQUIRE(destroyed);
    REQUIRE(destroyed->destroyed);
    island.enemies.generatorGone(0);
    const auto orphan = island.simulation.checkpoint(island.resources);
    REQUIRE(orphan);
    CHECK(orphan->enemies.enemies[static_cast<usize>(*slot)].generator == -1);
    CHECK(orphan->generators.generators.front().state == 0);
    CHECK(orphan->enemies.generatorEvents == initial->enemies.generatorEvents);
    REQUIRE(island.simulation.restore(*initial, island.resources));
    REQUIRE(island.simulation.restore(*orphan, island.resources));
    CHECK(island.simulation.checkpoint(island.resources) == orphan);
}

TEST_CASE("opponent transaction preserves the exhausted native missile identity sentinel",
          "[rollback][opponent-simulation][assets]") {
    Island island(true);
    auto exhausted = island.simulation.checkpoint(island.resources);
    REQUIRE(exhausted);
    exhausted->missiles.nextInstance = 0;
    REQUIRE(exhausted->valid());
    auto prepared = island.simulation.prepareRestore(*exhausted, island.resources);
    REQUIRE(prepared);
    REQUIRE(island.simulation.canCommit(*prepared));
    REQUIRE(island.simulation.commitRestore(std::move(*prepared)));
    CHECK(island.simulation.checkpoint(island.resources) == exhausted);
    island.missiles.launch(EnemyMissileKind::arrow(), {0, 3, 10}, {0, 3, 20}, 1, nullptr, -1);
    CHECK(island.missiles.count() == 0);
    CHECK(island.simulation.checkpoint(island.resources) == exhausted);
}

TEST_CASE("private opponent motion changes actual player collision and rewinds with its inputs",
          "[rollback][opponent-simulation][assets]") {
    Island island;
    island.players[0].position = {0, 0, -100};
    const s32 slot = island.spawn({0, 0, 10});
    for (s32 tick = 0; tick < 60; ++tick) {
        island.native(island.frame());
    }
    island.players[0].position = {0, 0, 0};
    const auto baseline = island.simulation.checkpoint(island.resources);
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
        const auto before = island.simulation.checkpoint(island.resources);
        REQUIRE(before);
        island.native(island.frame());
        const auto native = island.simulation.checkpoint(island.resources);
        REQUIRE(native);
        const auto nativeTargets = island.enemies.targets();
        const Vec3 expected = BodyCollision::resolveMovement(actor, actor.position(), desired,
                                                             nativeTargets, {}, &island.collision);
        REQUIRE(island.simulation.restore(*before, island.resources));
        const auto result = island.simulation.speculate(island.frame(), island.resources);
        if (result != OpponentSimulation::Result::Advanced) {
            CHECK(result == OpponentSimulation::Result::Interaction);
            CHECK(island.simulation.checkpoint(island.resources) == before);
            break;
        }
        CHECK(island.simulation.checkpoint(island.resources) == native);
        const Vec3 resolved = BodyCollision::resolveMovement(
            actor, actor.position(), desired, island.enemies.targets(), {}, &island.collision);
        CHECK(glm::distance(resolved, expected) < 0.00001f);
        collisionChanged = collisionChanged || glm::distance(resolved, stale) > 0.01f;
    }
    REQUIRE(collisionChanged);
    CHECK(island.enemies.positionOf(slot) !=
          baseline->enemies.enemies[static_cast<usize>(slot)].position);
    // Use the replayed body result as actual local motion, not a display-only
    // sample. The saved character remains outside the opponent island.
    const auto save = actor.save().toJson();
    actor.place(BodyCollision::resolveMovement(actor, actor.position(), desired,
                                               island.enemies.targets(), {}, &island.collision));
    CHECK(glm::distance(actor.position(), stale) > 0.01f);
    CHECK(actor.save().toJson() == save);
    REQUIRE(island.simulation.restore(*baseline, island.resources));
    CHECK(island.simulation.checkpoint(island.resources) == baseline);
}

TEST_CASE("clearing one pool invalidates an entire prepared opponent transaction",
          "[rollback][opponent-simulation][assets]") {
    Island island(true);
    island.spawn({0, 0, 15});
    island.missiles.launch(EnemyMissileKind::arrow(), {20, 4, 20}, {20, 4, 40}, 1, nullptr, -1);
    const auto initial = island.simulation.checkpoint(island.resources);
    REQUIRE(initial);
    auto prepared = island.simulation.prepareRestore(*initial, island.resources);
    REQUIRE(prepared);
    island.simulation.advance(island.frame());
    island.missiles.clear();
    const auto cleared = island.simulation.checkpoint(island.resources);
    REQUIRE(cleared);
    CHECK_FALSE(island.simulation.canCommit(*prepared));
    CHECK_FALSE(island.simulation.commitRestore(std::move(*prepared)));
    CHECK(island.simulation.checkpoint(island.resources) == cleared);
    CHECK(cleared->enemies.nextInstance > initial->enemies.nextInstance);
    CHECK(cleared->missiles.missiles.empty());
}

TEST_CASE("malformed opponent replay frame leaves the three pools untouched",
          "[rollback][opponent-simulation][assets]") {
    Island island(true);
    auto frame = island.frame();
    const s32 field = GENERATE(0, 1, 2, 3, 4, 5);
    if (field == 0) {
        frame.seconds = std::numeric_limits<f32>::quiet_NaN();
    } else if (field == 1) {
        frame.ticks = 60;
    } else if (field == 2) {
        frame.missileSpeed = -1;
    } else if (field == 3) {
        island.players[0].position.x = std::numeric_limits<f32>::infinity();
    } else if (field == 4) {
        frame.missileSpeed = std::numeric_limits<f32>::max();
    } else {
        frame.missileSpeed = std::numeric_limits<f32>::infinity();
    }
    const auto before = island.simulation.checkpoint(island.resources);
    REQUIRE(before);
    CHECK(island.simulation.speculate(frame, island.resources) ==
          OpponentSimulation::Result::Invalid);
    CHECK(island.simulation.checkpoint(island.resources) == before);
}

TEST_CASE("private opponent continuation bounds existing native missile work before updating",
          "[rollback][opponent-simulation][assets]") {
    Island island;
    island.missiles.launch(EnemyMissileKind::arrow(), {80, 10, 80}, {80, 10, 100}, 1, nullptr, -1);
    auto state = island.simulation.checkpoint(island.resources);
    REQUIRE(state);
    REQUIRE(state->missiles.missiles.size() == 1);
    SECTION("finite extreme velocity would exceed the spatial work budget") {
        state->missiles.missiles.front().velocity = Vec3{1000000};
        state->missiles.missiles.front().kind.radius = 0;
    }
    SECTION("finite extreme gravity would exceed the spatial work budget") {
        state->missiles.missiles.front().kind.weight = 1000000;
        state->missiles.missiles.front().kind.radius = 0;
    }
    SECTION("many individually ordinary flights exceed the aggregate work budget") {
        const auto missile = state->missiles.missiles.front();
        for (u64 instance = state->missiles.nextInstance; instance <= 1024; ++instance) {
            auto copy = missile;
            copy.instance = instance;
            state->missiles.missiles.push_back(copy);
        }
        state->missiles.nextInstance = 1025;
    }
    REQUIRE(state->valid());
    REQUIRE(island.simulation.restore(*state, island.resources));
    CHECK(island.simulation.speculate(island.frame(), island.resources) ==
          OpponentSimulation::Result::Unsupported);
    CHECK(island.simulation.checkpoint(island.resources) == state);
}

TEST_CASE("private opponent continuation still advances native flights and chasing bodies",
          "[rollback][opponent-simulation][assets]") {
    Island island;
    island.players[0].position = {0, 0, -100};
    const auto slot = island.spawn({0, 0, 20});
    for (s32 tick = 0; tick < 60; ++tick) {
        island.native(island.frame());
    }
    island.players[0].position = {0, 0, 0};
    island.missiles.launch(EnemyMissileKind::arrow(), {80, 10, 80}, {80, 10, 100}, 1, nullptr, -1);
    const auto initial = island.simulation.checkpoint(island.resources);
    REQUIRE(initial);
    REQUIRE(initial->missiles.missiles.size() == 1);
    for (s32 tick = 0; tick < 12; ++tick) {
        CAPTURE(tick);
        const auto before = island.simulation.checkpoint(island.resources);
        REQUIRE(before);
        island.native(island.frame());
        const auto expected = island.simulation.checkpoint(island.resources);
        REQUIRE(expected);
        REQUIRE(island.simulation.restore(*before, island.resources));
        REQUIRE(island.simulation.speculate(island.frame(), island.resources) ==
                OpponentSimulation::Result::Advanced);
        CHECK(island.simulation.checkpoint(island.resources) == expected);
    }
    const auto result = island.simulation.checkpoint(island.resources);
    REQUIRE(result);
    REQUIRE(result->missiles.missiles.size() == 1);
    CHECK(result->missiles.missiles.front().position !=
          initial->missiles.missiles.front().position);
    CHECK(result->enemies.enemies[static_cast<usize>(slot)].position !=
          initial->enemies.enemies[static_cast<usize>(slot)].position);
}

TEST_CASE("private opponent continuation admits native shooting minds and pending throws",
          "[rollback][opponent-simulation][assets]") {
    Island island(true);
    SECTION("shooting native mind") {
        const auto way = GENERATE(kThrowWay, kBombWay, kSkirmishWay, kSkirmishBombWay,
                                  kStandCastWay, kRangeCastWay, kCastWay);
        island.spawn({0, 0, 20}, way, kArcherStrength);
    }
    SECTION("previously requested throw survives a change to a chasing mind") {
        const auto slot = island.spawn({0, 0, 20});
        auto state = island.simulation.checkpoint(island.resources);
        REQUIRE(state);
        auto& animator = state->enemies.enemies[static_cast<usize>(slot)].animator;
        REQUIRE(animator);
        animator->requested = EnemyAction::Throw;
        REQUIRE(island.simulation.restore(*state, island.resources));
    }
    SECTION("brood can create a shooting mind in the same update") {
        auto state = island.simulation.checkpoint(island.resources);
        REQUIRE(state);
        REQUIRE(state->generators.generators.size() == 1);
        state->generators.generators.front().algorithm = kThrowWay;
        REQUIRE(island.simulation.restore(*state, island.resources));
    }
    const auto before = island.simulation.checkpoint(island.resources);
    REQUIRE(before);
    island.native(island.frame());
    const auto expected = island.simulation.checkpoint(island.resources);
    REQUIRE(expected);
    REQUIRE(island.simulation.restore(*before, island.resources));
    CHECK(island.simulation.speculate(island.frame(), island.resources) ==
          OpponentSimulation::Result::Advanced);
    CHECK(island.simulation.checkpoint(island.resources) == expected);
}

TEST_CASE("private archer and bomber releases match native continuation until the first impact",
          "[rollback][opponent-simulation][enemy-launch-budget][assets]") {
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
        auto state = island.simulation.checkpoint(island.resources);
        REQUIRE(state);
        state->generators.generators.front().algorithm = way;
        state->generators.generators.front().tier = strength;
        REQUIRE(island.simulation.restore(*state, island.resources));
    } else {
        island.spawn({0, 0, -20}, way, strength);
    }
    auto frame = island.frame();
    frame.ticks = ticks;
    frame.seconds = static_cast<f32>(ticks) / 60;
    const auto baseline = island.simulation.checkpoint(island.resources);
    REQUIRE(baseline);
    const auto uploads = island.device.texturesCreated;
    bool released = false;
    bool reachedBoundary = false;
    for (s32 tick = 0; tick < 600; tick += ticks) {
        CAPTURE(bred, bomb, tick, ticks);
        const auto before = island.simulation.checkpoint(island.resources);
        REQUIRE(before);
        island.native(frame);
        const auto expected = island.simulation.checkpoint(island.resources);
        REQUIRE(expected);
        REQUIRE(island.simulation.restore(*before, island.resources));
        const auto result = island.simulation.speculate(frame, island.resources);
        if (!expected->missiles.hits.empty() || !expected->enemies.blows.empty()) {
            CHECK(result == OpponentSimulation::Result::Interaction);
            CHECK(island.simulation.checkpoint(island.resources) == before);
            reachedBoundary = true;
            break;
        }
        REQUIRE(result == OpponentSimulation::Result::Advanced);
        CHECK(island.simulation.checkpoint(island.resources) == expected);
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

TEST_CASE("a later enemy launch exhausting work restores earlier shots cues and generator births",
          "[rollback][opponent-simulation][enemy-launch-budget][assets]") {
    Island island(true);
    const s32 archer = island.spawn({-6, 0, -20}, kThrowWay, kArcherStrength);
    std::optional<OpponentSimulation::Checkpoint> release;
    for (s32 tick = 0; tick < 360 && !release; ++tick) {
        const auto before = island.simulation.checkpoint(island.resources);
        REQUIRE(before);
        island.native(island.frame());
        const auto after = island.simulation.checkpoint(island.resources);
        REQUIRE(after);
        if (after->missiles.nextInstance > before->missiles.nextInstance) {
            release = *before;
        }
    }
    REQUIRE(release);
    auto& state = *release;
    // Start two independent native animator releases on the same AI step. The
    // second archer is across the player, with its own lifetime and floor point.
    const auto free = std::ranges::find_if(state.enemies.enemies, [](const auto& enemy) {
        return enemy.state == Enemies::State::Inactive;
    });
    REQUIRE(free != state.enemies.enemies.end());
    *free = state.enemies.enemies[static_cast<usize>(archer)];
    free->instance = state.enemies.nextInstance++;
    free->position.x += 12;
    free->birthPosition.x += 12;
    free->presentationPosition.x += 12;
    free->yaw = -free->yaw;
    free->presentationYaw = free->yaw;
    if (free->floor) {
        free->floor->local.x += 12;
    }
    REQUIRE(state.generators.generators.front().living < state.generators.generators.front().most);
    state.generators.generators.front().countdown = 0;
    REQUIRE(island.simulation.restore(state, island.resources));
    // These non-solid fixtures require only the native solid-bit check. The
    // first shot fits the shared scan allowance; the second cannot fit it.
    Obstacle empty;
    empty.solid = false;
    const std::vector<Obstacle> fixtures(2100, empty);
    auto frame = island.frame();
    frame.fixtures = fixtures;
    island.native(frame);
    const auto expected = island.simulation.checkpoint(island.resources);
    REQUIRE(expected);
    REQUIRE(expected->missiles.nextInstance >= state.missiles.nextInstance + 2);
    REQUIRE(expected->enemies.cues.size() >= state.enemies.cues.size() + 2);
    REQUIRE(expected->enemies.nextInstance > state.enemies.nextInstance);
    REQUIRE(expected->generators.generators.front().bred >
            state.generators.generators.front().bred);
    REQUIRE(island.simulation.restore(state, island.resources));
    CHECK(island.simulation.speculate(frame, island.resources) ==
          OpponentSimulation::Result::Unsupported);
    CHECK(island.simulation.checkpoint(island.resources) == release);
}

TEST_CASE("new enemy shots join the flight work check before any missile advances",
          "[rollback][opponent-simulation][enemy-launch-budget][assets]") {
    Island island;
    island.spawn({0, 0, -20}, kThrowWay, kArcherStrength);
    std::optional<OpponentSimulation::Checkpoint> release;
    for (s32 tick = 0; tick < 360 && !release; ++tick) {
        const auto before = island.simulation.checkpoint(island.resources);
        REQUIRE(before);
        island.native(island.frame());
        const auto after = island.simulation.checkpoint(island.resources);
        REQUIRE(after);
        if (after->missiles.nextInstance > before->missiles.nextInstance) {
            release = *before;
        }
    }
    REQUIRE(release);
    REQUIRE(release->missiles.missiles.empty());
    EnemyMissiles still;
    still.launch(EnemyMissileKind::arrow(), {80, 10, 80}, {80, 10, 100}, 1, nullptr, -1);
    const auto seed = still.checkpoint();
    REQUIRE(seed);
    auto flight = seed->missiles.front();
    flight.velocity = Vec3{0};
    for (u64 instance = 1; instance <= 511; ++instance) {
        flight.instance = instance;
        release->missiles.missiles.push_back(flight);
    }
    release->missiles.nextInstance = 512;
    REQUIRE(island.simulation.restore(*release, island.resources));
    island.native(island.frame());
    const auto expected = island.simulation.checkpoint(island.resources);
    REQUIRE(expected);
    REQUIRE(expected->missiles.nextInstance > release->missiles.nextInstance);
    REQUIRE(expected->missiles.missiles.front().position != flight.position);
    REQUIRE(island.simulation.restore(*release, island.resources));
    CHECK(island.simulation.speculate(island.frame(), island.resources) ==
          OpponentSimulation::Result::Unsupported);
    CHECK(island.simulation.checkpoint(island.resources) == release);
}

TEST_CASE("private opponent step rejects an impact and restores all pending queues and clocks",
          "[rollback][opponent-simulation][assets]") {
    Island island(true);
    island.missiles.launch(EnemyMissileKind::arrow(), {0, 2, -0.25f}, {0, 2, 10}, 1, nullptr, -1);
    auto before = island.simulation.checkpoint(island.resources);
    REQUIRE(before);
    before->enemies.cues.push_back({.kind = EnemyCue::Kind::Arrow, .enemyKind = kGruntKind});
    before->enemies.generatorEvents.push_back({0, EnemyGeneratorEvent::Kind::PatrolHit});
    REQUIRE(island.simulation.restore(*before, island.resources));
    CHECK(island.simulation.speculate(island.frame(), island.resources) ==
          OpponentSimulation::Result::Interaction);
    CHECK(island.simulation.checkpoint(island.resources) == before);
    CHECK(island.enemies.takeCues() == before->enemies.cues);
    CHECK(island.enemies.takeGeneratorEvents() == before->enemies.generatorEvents);
}

TEST_CASE("private opponent continuation refuses uncaptured contexts without invoking callbacks",
          "[rollback][opponent-simulation][assets]") {
    Island island;
    auto frame = island.frame();
    SECTION("active blast needs native scenery and hit effect state") {
        island.missiles.burst({0, 0, 0}, 10, 5, 0, 1, -1);
    }
    SECTION("returned damage needs player health and inventory") {
        island.players[0].meleeWard = EnemyMeleeWard::HealthVamp;
    }
    SECTION("death drain needs its external player state") {
        REQUIRE(island.enemies.loadKind(kDeathKind));
        REQUIRE(island.enemies.spawn({.kind = kDeathKind, .position = {0, 0, 10}, .placed = true},
                                     island.players));
    }
    SECTION("undelivered blows are not silently cleared") {
        auto state = island.simulation.checkpoint(island.resources);
        REQUIRE(state);
        state->enemies.blows.push_back({.player = 0, .kind = kGruntKind, .damage = 3});
        REQUIRE(island.simulation.restore(*state, island.resources));
    }
    const auto before = island.simulation.checkpoint(island.resources);
    REQUIRE(before);
    CHECK(island.simulation.speculate(frame, island.resources) ==
          OpponentSimulation::Result::Unsupported);
    CHECK(island.simulation.checkpoint(island.resources) == before);
}

TEST_CASE("native opponent phase still dispatches scenery in its missile phase",
          "[rollback][opponent-simulation][assets]") {
    Island island;
    island.missiles.burst({0, 0, 0}, 10, 5, 0, 1, -1);
    const auto before = island.simulation.checkpoint(island.resources);
    REQUIRE(before);
    usize called = 0;
    island.simulation.advance(island.frame(), [&](const auto&, auto& contacts) {
        ++called;
        contacts.push_back(12);
    });
    CHECK(called == 1);
    const auto state = island.simulation.checkpoint(island.resources);
    REQUIRE(state);
    REQUIRE(state->missiles.bursts.size() == 1);
    CHECK(state->missiles.bursts.front().scenery == std::vector<s32>{12});
    REQUIRE(island.simulation.restore(*before, island.resources));
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

TEST_CASE("ordinary placements replay their native watch phase before generators and swarm",
          "[rollback][opponent-simulation][opponent-placements][assets]") {
    Island island(true);
    PlacedEnemies placements;
    placements.bind({ordinaryPlacement({0, 0, 20}), ordinaryPlacement({8, 0, 25}),
                     ordinaryPlacement({0, 0, 80})});
    placements.observe(ViewVolume{}, {});
    OpponentSimulation combined{island.generators, island.enemies, island.missiles, &placements};
    const auto initial = combined.checkpoint(island.resources);
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
        const auto state = combined.checkpoint(island.resources);
        REQUIRE(state);
        expected.push_back(*state);
    }
    REQUIRE(expected.front().placements->pending == std::vector<usize>{2});
    REQUIRE(expected.front().enemies.nextInstance > initial->enemies.nextInstance);
    REQUIRE(combined.restore(*initial, island.resources));
    for (usize tick = 0; tick < expected.size(); ++tick) {
        CAPTURE(tick);
        REQUIRE(combined.speculate(island.frame(), island.resources) ==
                OpponentSimulation::Result::Advanced);
        CHECK(combined.checkpoint(island.resources) == expected[tick]);
    }
    CHECK(island.device.texturesCreated == uploads);
}

TEST_CASE("placement activation is restored with births RNG and impacts on failed continuation",
          "[rollback][opponent-simulation][opponent-placements][assets]") {
    Island island(true);
    PlacedEnemies placements;
    placements.bind({ordinaryPlacement({0, 0, 20}), ordinaryPlacement({8, 0, 25})});
    OpponentSimulation combined{island.generators, island.enemies, island.missiles, &placements};
    island.missiles.launch(EnemyMissileKind::arrow(), {0, 2, -0.25f}, {0, 2, 10}, 1, nullptr, -1);
    const auto initial = combined.checkpoint(island.resources);
    REQUIRE(initial);
    combined.advance(island.frame());
    const auto native = combined.checkpoint(island.resources);
    REQUIRE(native);
    REQUIRE(native->placements->pending.empty());
    REQUIRE(native->enemies.nextInstance > initial->enemies.nextInstance);
    REQUIRE_FALSE(native->missiles.hits.empty());
    REQUIRE(combined.restore(*initial, island.resources));
    CHECK(combined.speculate(island.frame(), island.resources) ==
          OpponentSimulation::Result::Interaction);
    CHECK(combined.checkpoint(island.resources) == initial);
    CHECK(std::ranges::equal(placements.pending(), initial->placements->pending));
}

TEST_CASE("stale placement preparation rejects the whole opponent transaction",
          "[rollback][opponent-simulation][opponent-placements][assets]") {
    Island island(true);
    PlacedEnemies placements;
    placements.bind({ordinaryPlacement({0, 0, 20})});
    OpponentSimulation combined{island.generators, island.enemies, island.missiles, &placements};
    const auto initial = combined.checkpoint(island.resources);
    REQUIRE(initial);
    auto prepared = combined.prepareRestore(*initial, island.resources);
    REQUIRE(prepared);
    island.native(island.frame());
    placements.observe(ViewVolume{}, {1, 0, 0});
    const auto current = combined.checkpoint(island.resources);
    REQUIRE(current);
    CHECK_FALSE(combined.canCommit(*prepared));
    CHECK_FALSE(combined.commitRestore(std::move(*prepared)));
    CHECK(combined.checkpoint(island.resources) == current);
    auto forged = *current;
    forged.placements->roster.front().spawn.position.x += 1;
    REQUIRE(forged.valid());
    CHECK_FALSE(combined.prepareRestore(forged, island.resources));
    CHECK(combined.checkpoint(island.resources) == current);
    forged = *current;
    forged.placements.reset();
    CHECK_FALSE(combined.prepareRestore(forged, island.resources));
    CHECK_FALSE(island.simulation.prepareRestore(*current, island.resources));
}

TEST_CASE("unsupported placed actors are not silently consumed by private replay",
          "[rollback][opponent-simulation][opponent-placements][assets]") {
    Island island;
    auto row = ordinaryPlacement({0, 0, 20});
    SECTION("great creature") {
        row.kind = row.spawn.kind = kGeneralEnemyKind;
    }
    SECTION("Death") {
        row.kind = row.spawn.kind = kDeathKind;
    }
    SECTION("IT") {
        row.kind = row.spawn.kind = kItKind;
    }
    SECTION("Garm brood") {
        row.kind = row.spawn.kind = kGarmBroodKind;
    }
    SECTION("unsupported mind") {
        row.spawn.algorithm = kSuicideWay;
    }
    SECTION("external carried item") {
        row.carried = 1;
    }
    PlacedEnemies placements;
    placements.bind({row});
    OpponentSimulation combined{island.generators, island.enemies, island.missiles, &placements};
    const auto initial = combined.checkpoint(island.resources);
    REQUIRE(initial);
    CHECK(combined.speculate(island.frame(), island.resources) ==
          OpponentSimulation::Result::Unsupported);
    CHECK(combined.checkpoint(island.resources) == initial);
}

} // namespace
