#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/SessionInputs.h"
#include "game/world/BodyCollision.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

void terrainFloor(std::vector<CollisionTriangle>& triangles, s32 object, f32 first, f32 last,
                  f32 bottom, f32 top, u32 flags = 4) {
    const Vec3 normal = glm::normalize(Vec3{bottom - top, last - first, 0});
    const Vec3 a{first, bottom, -10};
    const Vec3 b{last, top, -10};
    const Vec3 c{last, top, 10};
    const Vec3 d{first, bottom, 10};
    triangles.push_back({normal, {a, c, b}, object, flags});
    triangles.push_back({normal, {a, d, c}, object, flags});
}

/** Exact PartyMotion is the oracle, including its floor-owner and impulse phases.
 * The scene is static; no asset exporter or predictor helper supplies the result. */
struct TerrainFixture {
    test::FakeRenderDevice device;
    WorldCollision collision;
    std::array<PlayerRuntime, 1> native;
    MatchContext context;
    CombatSnapshot state;
    LocalMovementPrediction prediction;
    PartyMotion::Events events;
    std::vector<PlayerLocomotionState::Body> bodies;
    std::vector<Obstacle> generators;
    std::vector<Obstacle> fixtures;
    usize performed = 0;
    f32 seconds = 1.0f / 60;

    TerrainFixture(std::vector<CollisionTriangle> triangles, const Vec3& position,
                   s32 character = 0, u16 tickRate = 60) {
        collision.build(std::move(triangles));
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        CharacterSave save;
        save.character = character;
        save.autoAttack = false;
        native[0].actor.spawn(1, save, nullptr, position, 0.75f);
        native[0].figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(native[0].figure);
        FloorRiding::land(native, 0, position, collision);
        REQUIRE(native[0].floor.object >= 0);
        context.epoch = 8;
        context.scene = 1;
        context.tickRate = tickRate;
        context.owners = {1, 2, 0, 0};
        context.grants = {1, 4, 0, 0};
        seconds = 1.0f / static_cast<f32>(tickRate);
        state.motion.epoch = context.epoch;
        state.motion.tick = 10;
        state.motion.cameraContinuity = 1;
        state.motion.players[0] = SeatMotion{1, 1, {-30, 0, 0}, 0};
        state.players[0].emplace().health = 100;
        state.motion.players[1] = SeatMotion{4, 1, position, native[0].actor.yaw()};
        auto& player = state.players[1].emplace();
        player.health = 100;
        player.movement.enabled = player.movement.current = true;
        player.movement.radius = native[0].actor.radius();
        player.movement.height = native[0].actor.height();
        player.movement.followHeight = native[0].actor.followPoint().y - position.y;
        player.movement.speed = native[0].actor.speed();
        events.select = [](usize, const SelectorInput&, s32) {};
        events.advanceTurbo = [](usize, s32, f32) {};
        events.perform = [&](usize, PartyMotion::Action) { ++performed; };
        events.resolveMovement = [&](usize, const Vec3& from, const Vec3& to) {
            std::vector<MissileTarget> targets;
            for (const auto& body : bodies) {
                targets.push_back(body.target());
            }
            return BodyCollision::resolveMovement(native[0].actor, from, to, targets, generators,
                                                  &collision);
        };
        capture();
        begin();
    }

    void begin() {
        prediction.begin(context, 2, collision);
        REQUIRE(prediction.setPlayer(1, *native[0].figure->actionTree()));
        prediction.update(&state, {});
    }

    void capture() {
        const auto& runtime = native[0];
        state.motion.players[1]->position = runtime.actor.position();
        state.motion.players[1]->yaw = std::remainder(runtime.actor.yaw(), kTwoPi);
        auto& player = *state.players[1];
        const auto& action = runtime.figure->animator();
        const auto& cursor = action.player();
        player.animation = {static_cast<u32>(action.action()), cursor.sequence(),
                            cursor.generation(), cursor.presentationFrame(), cursor.transition()};
        player.rollback = action.checkpoint();
        player.transitionPose = action.transitionPose();
        player.knockback = runtime.knockback.checkpoint();
        player.floor =
            PlayerFloorState{runtime.floor.object, runtime.floor.flags, runtime.floor.placement};
        player.locomotion.emplace();
        player.locomotion->origin = runtime.actor.position();
        player.locomotion->radius = 8;
        player.locomotion->ticks = 12;
        player.locomotion->bodies = bodies;
        player.locomotion->generators = generators;
        player.locomotion->fixtures = fixtures;
        REQUIRE(state.valid());
    }

    InputCommand input(u64 tick, f32 direction = 1) const {
        InputCommand result;
        result.epoch = context.epoch;
        result.tick = tick;
        result.seat = 1;
        result.grant = 4;
        result.direction = {direction, 0};
        result.magnitude = 1;
        return result;
    }

    void step(const InputCommand& command) {
        std::array<PlayInput, InputCommand::kSeats> inputs;
        inputs[1] = SessionInputs::playInput(command);
        PartyMotion::step(native, inputs, false, state.players[1]->movement.cameraYaw, 1, seconds,
                          collision, events);
        for (const auto& box : fixtures) {
            auto& actor = native[0].actor;
            actor.slide(box.pushOut(actor.position(), actor.radius()) - actor.position(),
                        &collision);
        }
        state.motion.tick = command.tick;
        state.players[1]->rollbackInput = command;
        capture();
    }

    CombatSnapshot shown(f32 blend = 1) const {
        auto result = state;
        prediction.present(result, blend);
        REQUIRE(result.valid());
        return result;
    }

    void compare() const {
        REQUIRE(prediction.replayed(1));
        CHECK(prediction.replayed(1)->releases.empty());
        const auto result = shown();
        CHECK(glm::distance(result.motion.players[1]->position, native[0].actor.position()) ==
              Approx(0).margin(0.0001));
        CHECK(std::remainder(result.motion.players[1]->yaw - native[0].actor.yaw(), kTwoPi) ==
              Approx(0).margin(0.0001));
        CHECK(result.players[1]->rollback == native[0].figure->animator().checkpoint());
        CHECK(result.players[1]->transitionPose == native[0].figure->animator().transitionPose());
        CHECK(result.players[1]->knockback == native[0].knockback.checkpoint());
        REQUIRE(result.players[1]->floor);
        CHECK(result.players[1]->floor->object == native[0].floor.object);
        CHECK(result.players[1]->floor->flags == native[0].floor.flags);
        CHECK_FALSE(result.players[1]->floor->placement);
    }
};

TEST_CASE("local movement replays native static slopes seams and stair floor transitions",
          "[netplay][terrain-rollback][assets]") {
    const bool stairs = GENERATE(false, true);
    const f32 direction = GENERATE(-1.0f, 1.0f);
    const s32 character = GENERATE(0, 6);
    const u16 tickRate = GENERATE(u16{30}, u16{60});
    CAPTURE(stairs, direction, character, tickRate);
    std::vector<CollisionTriangle> triangles;
    if (stairs) {
        terrainFloor(triangles, 0, -10, -2, 0, 0);
        terrainFloor(triangles, 1, -2, 0, 0.75f, 0.75f);
        terrainFloor(triangles, 2, 0, 2, 1.5f, 1.5f);
        terrainFloor(triangles, 3, 2, 10, 2.25f, 2.25f);
    } else {
        terrainFloor(triangles, 0, -10, 0, -5, 0);
        terrainFloor(triangles, 1, 0.02f, 10, 0.01f, 5);
    }
    const f32 startX = -3 * direction;
    const f32 startY = stairs ? (direction > 0 ? 0 : 2.25f) : startX * 0.5f;
    TerrainFixture f(std::move(triangles), {startX, startY, 0}, character, tickRate);
    auto authority = f.state;
    const auto initialFloor = f.native[0].floor.object;
    for (u64 tick = 11; tick <= static_cast<u64>(tickRate) * 2 + 10; ++tick) {
        CAPTURE(tick);
        auto command = f.input(tick, direction);
        if (tick % 12 >= 6) {
            command.heldButtons = static_cast<u32>(CommandHeld::Strafe);
        }
        f.step(command);
        const auto nativeEvents = f.performed;
        const auto nativeActor = f.native[0].actor.checkpoint();
        f.prediction.update(&authority, std::array{command});
        f.compare();
        for (const f32 blend : {0.0f, 0.5f, 1.0f}) {
            const auto presented = f.shown(blend);
            PlayerActor contact;
            const auto& pose = *presented.motion.players[1];
            const auto& envelope = presented.players[1]->movement;
            contact.restoreMotion(pose.position, pose.yaw, envelope.speed, envelope.radius,
                                  envelope.height, envelope.followHeight);
            const auto ground = FloorRiding::support(contact, f.collision);
            REQUIRE(ground);
            CHECK(pose.position.y == Approx(ground->y).margin(0.0001));
        }
        CHECK(f.performed == nativeEvents);
        CHECK(f.native[0].actor.checkpoint() == nativeActor);
        if (tick % 5 == 0) {
            authority = f.state;
        }
    }
    CHECK(f.native[0].floor.object != initialFloor);
    CHECK((f.native[0].actor.position().x - startX) * direction > 5);
    CHECK(std::abs(f.native[0].actor.position().y - startY) > 2);
}

TEST_CASE("terrain replay refuses uncertified floor ownership and fall paths",
          "[netplay][terrain-rollback][assets]") {
    std::vector<CollisionTriangle> triangles;
    terrainFloor(triangles, 0, -10, 0, -5, 0);
    terrainFloor(triangles, 1, 0, 10, 0, 5);
    TerrainFixture f(std::move(triangles), {-0.02f, -0.01f, 0});
    SECTION("moving support reached after the local input") {
        f.collision.setMovingObjects(std::array<s32, 1>{1});
    }
    SECTION("floor exit lock") {
        f.collision.setFloorExitBlocked(0, true);
    }
    SECTION("absent support would require native falling or rescue") {
        f.collision.clear();
    }
    SECTION("last floor belongs to a kept-apart support") {
        f.state.players[1]->floor->flags |= FloorRiding::kKeptApart;
    }
    SECTION("last floor does not exist") {
        f.state.players[1]->floor = PlayerFloorState{-1, 0, std::nullopt};
    }
    SECTION("legacy snapshot omits floor ownership") {
        f.state.players[1]->floor.reset();
    }
    SECTION("automatic acquisition lacks a vertical certificate") {
        auto& contacts = *f.state.players[1]->locomotion;
        contacts.automatic = true;
        // Strafe bypasses acquisition this tick, but must still not extend the
        // frozen context onto another elevation for a later ordinary step.
    }
    f.begin();
    const auto baseline = f.state;
    auto command = f.input(11);
    command.heldButtons = static_cast<u32>(CommandHeld::Strafe);
    f.prediction.update(&baseline, std::array{command});
    CHECK_FALSE(f.prediction.replayed(1));
    const auto result = f.shown();
    CHECK(result.motion.players[1]->position == baseline.motion.players[1]->position);
    CHECK(result.motion.players[1]->yaw == baseline.motion.players[1]->yaw);
    CHECK(result.players[1]->rollback == baseline.players[1]->rollback);
}

TEST_CASE("static slope rollback preserves ordered creature generator and fixture contacts",
          "[netplay][terrain-rollback][locomotion-contact][assets]") {
    const auto kind = GENERATE(0, 1, 2);
    CAPTURE(kind);
    std::vector<CollisionTriangle> triangles;
    terrainFloor(triangles, 0, -10, 0, -5, 0);
    terrainFloor(triangles, 1, 0, 10, 0, 5);
    TerrainFixture f(std::move(triangles), {-2, -1, 0});
    if (kind == 0) {
        f.bodies.push_back({{1, 0.5f, 0}, 0.5f, 5});
    } else {
        Obstacle box;
        box.centre = {1, 0.5f, 0};
        box.halfAcross = box.halfAlong = 0.5f;
        box.height = 5;
        (kind == 1 ? f.generators : f.fixtures).push_back(box);
    }
    f.capture();
    f.begin();
    auto authority = f.state;
    for (u64 tick = 11; tick <= 70; ++tick) {
        CAPTURE(tick);
        const auto command = f.input(tick);
        f.step(command);
        f.prediction.update(&authority, std::array{command});
        f.compare();
        if (tick % 5 == 0) {
            authority = f.state;
        }
    }
    CHECK(f.native[0].actor.position().x > -1);
    CHECK(f.native[0].actor.position().x < 0.1f);
    CHECK(f.native[0].actor.position().y > -0.5f);
}

TEST_CASE("an unrelated attack certificate cannot replace missing native locomotion contacts",
          "[netplay][terrain-rollback][locomotion-contact][assets]") {
    std::vector<CollisionTriangle> triangles;
    terrainFloor(triangles, 0, -10, 10, 0, 0);
    TerrainFixture f(std::move(triangles), {0, 0, 0});
    auto& player = *f.state.players[1];
    player.locomotion.reset();
    SECTION("retained idle turbo certificate") {
        player.turbo =
            PlayerTurboState{f.native[0].turbo.checkpoint(), f.native[0].move.checkpoint()};
        auto& geometry = player.turboGeometry.emplace();
        geometry.selection.feet = f.native[0].actor.position();
        geometry.selection.reach = f.native[0].actor.reach();
        auto& moving = geometry.moving.emplace();
        moving.origin = geometry.selection.feet;
        moving.radius = 8;
        moving.ticks = 12;
    }
    SECTION("retained stationary melee certificate") {
        auto& melee = player.melee.emplace();
        melee.selection.feet = f.native[0].actor.position();
        melee.selection.reach = f.native[0].actor.reach();
    }
    REQUIRE(f.state.valid());
    f.begin();
    const auto baseline = f.state;
    f.prediction.update(&baseline, std::array{f.input(11)});
    CHECK_FALSE(f.prediction.replayed(1));
    CHECK(f.shown().motion.players[1]->position == baseline.motion.players[1]->position);
}

TEST_CASE("missing generic contacts do not revoke an independently bounded turbo start",
          "[netplay][terrain-rollback][locomotion-contact][turbo-rollback][assets]") {
    std::vector<CollisionTriangle> triangles;
    terrainFloor(triangles, 0, -10, 10, 0, 0);
    TerrainFixture f(std::move(triangles), {0, 0, 0});
    ClassDataSet classes;
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path();
    REQUIRE(classes.load(root));
    REQUIRE(classes.stats(0));
    auto& player = *f.state.players[1];
    player.locomotion.reset();
    f.native[0].turbo.add(40);
    player.turbo = PlayerTurboState{f.native[0].turbo.checkpoint(), f.native[0].move.checkpoint()};
    auto& geometry = player.turboGeometry.emplace();
    geometry.selection.feet = f.native[0].actor.position();
    geometry.selection.reach = f.native[0].actor.reach();
    auto& moving = geometry.moving.emplace();
    moving.origin = geometry.selection.feet;
    moving.radius = 8;
    moving.ticks = 12;
    REQUIRE(f.state.valid());
    f.begin();
    REQUIRE(f.prediction.setPlayer(1, *f.native[0].figure->actionTree(), classes.stats(0), 0));
    auto command = f.input(11);
    command.direction = Vec2{0};
    command.magnitude = 0;
    command.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
    f.prediction.update(&f.state, std::array{command});
    REQUIRE(f.prediction.replayed(1));
    CHECK(f.shown().players[1]->rollback->action == PlayerAnimator::Action::TurboStrong);
}
} // namespace
