#include <array>
#include <cmath>
#include <format>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/SessionInputs.h"
#include "game/world/WorldMotion.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

enum class PlatformMotion { Lift, Rotation, Reverse, Rotator };

void sameMatrix(const Mat4& actual, const Mat4& expected) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            CHECK(actual[column][row] == Approx(expected[column][row]).margin(0.0001));
        }
    }
}

/** A native authored parent track or post-motion rotator carrying a child floor.
 * PartyMotion, not the replay implementation, drives the independent oracle. */
struct PlatformFixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    WorldCollision collision;
    WorldAnimator animator;
    LevelTriggers triggers;
    Rotators rotators;
    std::array<PlayerRuntime, 1> native;
    MatchContext context;
    CombatSnapshot state;
    LocalMovementPrediction prediction;
    PartyMotion::Events events;
    usize performed = 0;
    f32 seconds = 1.0f / 60;
    bool pauseLoops = false;

    explicit PlatformFixture(PlatformMotion mode, s32 character = 0, u32 tickRate = 60,
                             bool adjoiningFloor = false) {
        const auto directory = test::sampleLevel("local-platform-replay");
        const u32 flags =
            WorldObject::kAnimated | (mode == PlatformMotion::Reverse ? WorldObject::kReverse : 0);
        constexpr std::string_view distant = R"({"object":3,"frames":61,"state":257,"start":0,
                                                   "track":{"flags":32,"frames":[0,60],"values":[0,6]}})";
        const auto animation =
            mode == PlatformMotion::Rotator
                ? std::format("[{}]", distant)
                : std::format(R"([{{"object":0,"frames":31,"state":257,"start":0,
                                        "track":{{"flags":{},"frames":[0,30],"values":[0,2]}}}},{}])",
                              mode == PlatformMotion::Rotation ? 2 : 32, distant);
        const auto items = mode == PlatformMotion::Rotator
                               ? R"("itemInfos":[{"type":12,"subtype":0,"name":"SPIN"}],
                                    "itemInstances":[{"info":0,"position":[10,1,50],
                                        "params":[0,0,0,0,10,215,35,60,0,0,128,63]}],)"
                               : "";
        writeTextFile(directory / "world.json",
                      std::format(R"({{
            "objects":[
                {{"name":"GROUP","position":[10,1,50],"flags":{},"child":1,"next":2}},
                {{"name":"FLOOR","position":[0,0,0],"flags":4100}},
                {{"name":"FLOOR","position":[0,0,0],"flags":4,"next":3}},
                {{"name":"FLOOR","position":[{},1,50],"flags":4100}}],
            {} "animations":{}}})",
                                  flags, adjoiningFloor ? 20 : 100, items, animation));
        REQUIRE(models.load(directory));
        REQUIRE(textures.load(directory));
        REQUIRE(layout.load(directory));
        REQUIRE(scene.build(layout, models, textures, device));
        std::vector<CollisionTriangle> triangles{
            {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{100, 0, -100}}, 2, 4},
            {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 2, 4},
            {{0, 1, 0}, {Vec3{-5, 0, -5}, Vec3{5, 0, 5}, Vec3{5, 0, -5}}, 1, 4100},
            {{0, 1, 0}, {Vec3{-5, 0, -5}, Vec3{-5, 0, 5}, Vec3{5, 0, 5}}, 1, 4100},
            {{0, 1, 0}, {Vec3{-5, 0, -5}, Vec3{5, 0, 5}, Vec3{5, 0, -5}}, 3, 4100},
            {{0, 1, 0}, {Vec3{-5, 0, -5}, Vec3{-5, 0, 5}, Vec3{5, 0, 5}}, 3, 4100}};
        collision.build(std::move(triangles));
        collision.setMovingObjects(std::array<s32, 2>{1, 3});
        animator.bind(layout);
        triggers.bind(layout, animator, &collision);
        rotators.bind(layout);
        animator.apply(scene);
        sync();
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        CharacterSave save;
        save.character = character;
        const auto foot = Vec3{scene.worldTransform(1) * Vec4{1, 0, 0, 1}};
        native[0].actor.spawn(1, save, nullptr, foot, 0.35f);
        native[0].figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(native[0].figure);
        FloorRiding::land(native, 0, foot, collision);
        REQUIRE(native[0].floor.object == 1);
        REQUIRE(native[0].floor.placement);
        context.epoch = 3;
        context.scene = 1;
        REQUIRE(tickRate <= 120);
        context.tickRate = static_cast<u16>(tickRate);
        context.owners = {1, 2, 0, 0};
        context.grants = {1, 4, 0, 0};
        seconds = 1.0f / static_cast<f32>(tickRate);
        state.motion.epoch = context.epoch;
        state.motion.tick = 10;
        state.motion.cameraContinuity = 1;
        state.motion.players[0] = SeatMotion{1, 1, {-30, 0, 0}, 0};
        state.players[0].emplace().health = 100;
        state.motion.players[1] = SeatMotion{4, 1, foot, native[0].actor.yaw()};
        auto& player = state.players[1].emplace();
        player.health = 100;
        player.movement.enabled = player.movement.current = player.movement.platform = true;
        player.movement.radius = native[0].actor.radius();
        player.movement.height = native[0].actor.height();
        player.movement.followHeight = native[0].actor.followPoint().y - foot.y;
        player.movement.speed = native[0].actor.speed();
        player.movement.cameraYaw = 0.4f;
        capture();
        events.select = [](usize, const SelectorInput&, s32) {};
        events.advanceTurbo = [](usize, s32, f32) {};
        events.perform = [&](usize, PartyMotion::Action) { ++performed; };
        prediction.begin(context, 2, collision, &scene, &layout);
        REQUIRE(prediction.setPlayer(1, *native[0].figure->actionTree()));
        prediction.update(&state, {});
    }

    void sync() {
        collision.setObjectTransform(1, scene.worldTransform(1));
        collision.setObjectTransform(3, scene.worldTransform(3));
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
        player.movement.platform = runtime.floor.placement.has_value();
        player.locomotion.reset();
        if (!player.movement.platform) {
            // The native fixture has no creatures or blocking fixtures. Once on
            // its still ground, publish that explicit empty contact observation.
            auto& contacts = player.locomotion.emplace();
            contacts.origin = runtime.actor.position();
            contacts.radius = 8;
            contacts.ticks = PlayerLocomotionState::kMaxTicks;
        }
        state.geometry = scene.geometry();
        state.worldMotion = WorldMotion::capture(animator, triggers, rotators, collision,
                                                 layout.objects().size(), pauseLoops);
        state.worldMotionTicks = 12;
        REQUIRE(state.valid());
    }

    InputCommand input(u64 tick) const {
        InputCommand result;
        result.epoch = context.epoch;
        result.tick = tick;
        result.seat = 1;
        result.grant = 4;
        return result;
    }

    void step(const InputCommand& command) {
        scene.capturePresentation();
        animator.step(seconds, scene, pauseLoops);
        sync();
        std::array<PlayInput, InputCommand::kSeats> inputs;
        inputs[1] = SessionInputs::playInput(command);
        PartyMotion::step(native, inputs, false, state.players[1]->movement.cameraYaw, 1, seconds,
                          collision, events);
        triggers.update(seconds, {}, animator, scene, &collision);
        rotators.update(seconds, {}, scene);
        sync();
        state.motion.tick = command.tick;
        state.players[1]->rollbackInput = command;
        capture();
    }

    CombatSnapshot shown() const {
        auto result = state;
        prediction.present(result, 1);
        REQUIRE(result.valid());
        return result;
    }

    void compare(const CombatSnapshot& shown) const {
        REQUIRE(prediction.replayed(1));
        CHECK(prediction.replayed(1)->releases.empty());
        CHECK(shown.players[1]->rollback == native[0].figure->animator().checkpoint());
        CHECK(shown.players[1]->knockback == native[0].knockback.checkpoint());
        CHECK(glm::distance(shown.motion.players[1]->position, native[0].actor.position()) ==
              Approx(0).margin(0.0001));
        CHECK(std::remainder(shown.motion.players[1]->yaw - native[0].actor.yaw(), kTwoPi) ==
              Approx(0).margin(0.0001));
        REQUIRE(shown.players[1]->floor);
        CHECK(shown.players[1]->floor->object == native[0].floor.object);
        CHECK(shown.players[1]->floor->flags == native[0].floor.flags);
        REQUIRE(shown.players[1]->floor->placement.has_value() ==
                native[0].floor.placement.has_value());
        if (native[0].floor.placement) {
            sameMatrix(*shown.players[1]->floor->placement, *native[0].floor.placement);
        }
        REQUIRE(shown.geometry);
        const auto expected = scene.geometry();
        REQUIRE(shown.geometry->objects.size() == expected.objects.size());
        for (usize object = 0; object < expected.objects.size(); ++object) {
            if (expected.objects[object].index > 1) {
                continue;
            }
            CHECK(shown.geometry->objects[object].index == expected.objects[object].index);
            CHECK(shown.geometry->objects[object].continuity ==
                  expected.objects[object].continuity);
            sameMatrix(shown.geometry->objects[object].local, expected.objects[object].local);
        }
    }
};

TEST_CASE("local riders replay exact native authored and post-motion platform phases",
          "[netplay][platform-rollback][assets]") {
    const auto mode = GENERATE(PlatformMotion::Lift, PlatformMotion::Rotation,
                               PlatformMotion::Reverse, PlatformMotion::Rotator);
    const s32 character = GENERATE(0, 6);
    const u32 tickRate = GENERATE(30U, 60U);
    CAPTURE(mode, character, tickRate);
    PlatformFixture f(mode, character, tickRate);
    std::vector<CombatSnapshot> history{f.state};
    for (u64 tick = 11; tick <= 76; ++tick) {
        CAPTURE(tick);
        auto command = f.input(tick);
        if (tick >= 17 && tick % 12 < 6) {
            command.direction = {0, 1};
            command.magnitude = 0.4f;
        }
        if (tick >= 30) {
            command.heldButtons = static_cast<u32>(CommandHeld::Strafe);
        }
        if (tick >= 46) {
            command.aimDirection = tick % 5 == 0 ? Vec3{0} : glm::normalize(Vec3{-1, 0, 0.5f});
        }
        f.step(command);
        if (mode == PlatformMotion::Rotator) {
            REQUIRE(f.native[0].floor.placement);
            CHECK(*f.native[0].floor.placement != f.scene.worldTransform(1));
        }
        history.push_back(f.state);
        const auto& baseline = history[static_cast<usize>(tick > 16 ? tick - 16 : 0)];
        const auto inventory = f.native[0].actor.save().progress().inventory;
        const auto action = f.native[0].figure->animator().checkpoint();
        const auto floor = f.native[0].floor;
        const auto performed = f.performed;
        f.prediction.update(&baseline, std::array{command});
        f.compare(f.shown());
        for (usize render = 0; render < tick % 4; ++render) {
            f.compare(f.shown());
        }
        CHECK(f.native[0].actor.save().progress().inventory == inventory);
        CHECK(f.native[0].figure->animator().checkpoint() == action);
        CHECK(f.native[0].floor.placement == floor.placement);
        // Native footsteps are expected. Rewinding/presenting must not dispatch
        // any additional footsteps or gameplay events into that live scene.
        CHECK(f.performed == performed);
    }
}

TEST_CASE("platform prediction leaves unrelated world presentation on its delayed clock",
          "[netplay][platform-rollback][assets]") {
    PlatformFixture f(PlatformMotion::Lift);
    const auto baseline = f.state;
    for (u64 tick = 11; tick <= 16; ++tick) {
        const auto command = f.input(tick);
        f.step(command);
        f.prediction.update(&baseline, std::array{command});
    }
    auto shown = baseline;
    f.prediction.present(shown, 1);
    REQUIRE(shown.geometry);
    REQUIRE(baseline.geometry);
    REQUIRE(shown.geometry->objects.size() == 3);
    REQUIRE(shown.geometry->objects.back().index == 3);
    sameMatrix(shown.geometry->objects.back().local, baseline.geometry->objects.back().local);
    CHECK(shown.geometry->objects.back().local != f.state.geometry->objects.back().local);
    f.compare(shown);
}

TEST_CASE("predicted riders merge support geometry into older uncertified presentation",
          "[netplay][platform-rollback][assets]") {
    PlatformFixture f(PlatformMotion::Lift);
    const auto baseline = f.state;
    const auto command = f.input(11);
    f.step(command);
    f.prediction.update(&baseline, std::array{command});
    auto shown = baseline;
    shown.worldMotion.reset();
    shown.worldMotionTicks = 0;
    shown.players[1]->movement.platform = false;
    REQUIRE(shown.geometry);
    SECTION("support override absent in delayed sparse geometry") {
        shown.geometry->objects.erase(shown.geometry->objects.begin());
    }
    SECTION("support returns from a delayed visibility override") {
        shown.geometry->objects.front().visible = false;
        shown.geometry->objects.front().alpha = 0.25f;
    }
    REQUIRE(shown.valid());
    f.prediction.present(shown, 1);
    CHECK(shown.valid());
    CHECK_FALSE(shown.players[1]->movement.platform);
    CHECK_FALSE(shown.worldMotion);
    f.compare(shown);
    REQUIRE(shown.geometry->objects.front().index == 0);
    CHECK(shown.geometry->objects.front().visible);
    CHECK(shown.geometry->objects.front().alpha == 1);
}

TEST_CASE("rider rollback preserves native paused loops and one-shot motion",
          "[netplay][platform-rollback][assets]") {
    const auto mode = GENERATE(PlatformMotion::Lift, PlatformMotion::Reverse);
    PlatformFixture f(mode);
    f.pauseLoops = true;
    f.capture();
    f.prediction.begin(f.context, 2, f.collision, &f.scene, &f.layout);
    REQUIRE(f.prediction.setPlayer(1, *f.native[0].figure->actionTree()));
    const auto baseline = f.state;
    for (u64 tick = 11; tick <= 20; ++tick) {
        const auto command = f.input(tick);
        f.step(command);
        f.prediction.update(&baseline, std::array{command});
        f.compare(f.shown());
    }
    CHECK((f.native[0].actor.position() != baseline.motion.players[1]->position) ==
          (mode == PlatformMotion::Reverse));
}

TEST_CASE("riders can leave a predicted platform without retaining its carry ownership",
          "[netplay][platform-rollback][assets]") {
    PlatformFixture f(PlatformMotion::Lift);
    f.pauseLoops = true;
    auto& rider = f.native[0];
    rider.actor.place(Vec3{f.scene.worldTransform(1) * Vec4{4.9f, 0, 0, 1}});
    FloorRiding::land(f.native, 0, rider.actor.position(), f.collision);
    f.capture();
    f.state.players[1]->movement.cameraYaw = 0;
    f.prediction.begin(f.context, 2, f.collision, &f.scene, &f.layout);
    REQUIRE(f.prediction.setPlayer(1, *rider.figure->actionTree()));
    const auto baseline = f.state;
    for (u64 tick = 11; tick <= 22; ++tick) {
        auto command = f.input(tick);
        command.direction = {1, 0};
        command.magnitude = 1;
        f.step(command);
        f.prediction.update(&baseline, std::array{command});
        f.compare(f.shown());
    }
    CHECK(rider.floor.object == 2);
    CHECK_FALSE(f.shown().players[1]->floor->placement);
    CHECK_FALSE(f.shown().players[1]->movement.platform);
}

TEST_CASE("platform certificates reject combat requests and expire at their sampled horizon",
          "[netplay][platform-rollback][assets]") {
    PlatformFixture f(PlatformMotion::Lift);
    auto command = f.input(11);
    SECTION("ordinary attack") {
        command.heldButtons = static_cast<u32>(CommandHeld::Attack);
    }
    SECTION("strong attack") {
        command.heldButtons = static_cast<u32>(CommandHeld::StrongAttack);
    }
    SECTION("potion") {
        command.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    }
    SECTION("turbo") {
        command.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
    }
    SECTION("inventory selector") {
        command.pressedButtons = static_cast<u32>(CommandPress::SelectorUp);
    }
    SECTION("horizon exhausted") {
        for (u64 tick = 11; tick <= 22; ++tick) {
            f.prediction.update(&f.state, std::array{f.input(tick)});
            REQUIRE(f.prediction.replayed(1));
        }
        command = f.input(23);
    }
    f.prediction.update(&f.state, std::array{command});
    CHECK(f.prediction.replayed(1) == nullptr);
    CHECK(f.shown().motion.players[1]->position == f.state.motion.players[1]->position);
    CHECK(f.shown().players[1]->rollback == f.state.players[1]->rollback);
    CHECK(f.performed == 0);
}

TEST_CASE("rider prediction refuses transfer outside the certified carrier subtree",
          "[netplay][platform-rollback][assets]") {
    PlatformFixture f(PlatformMotion::Lift, 0, 60, true);
    f.pauseLoops = true;
    auto& rider = f.native[0];
    rider.actor.place(Vec3{f.scene.worldTransform(1) * Vec4{4.9f, 0, 0, 1}});
    FloorRiding::land(f.native, 0, rider.actor.position(), f.collision);
    REQUIRE(rider.floor.object == 1);
    f.capture();
    f.state.players[1]->movement.cameraYaw = 0;
    f.prediction.begin(f.context, 2, f.collision, &f.scene, &f.layout);
    REQUIRE(f.prediction.setPlayer(1, *rider.figure->actionTree()));
    const auto baseline = f.state;
    bool crossed = false;
    for (u64 tick = 11; tick <= 22; ++tick) {
        auto command = f.input(tick);
        command.direction = {1, 0};
        command.magnitude = 1;
        f.step(command);
        f.prediction.update(&baseline, std::array{command});
        if (rider.floor.object == 3) {
            crossed = true;
            CHECK(f.prediction.replayed(1) == nullptr);
            auto shown = baseline;
            // The renderer can still be showing an older place on the deck.
            // Rejected transfer must not substitute latest-authority feet while
            // leaving the delayed support and its other occupants behind.
            shown.motion.tick = baseline.motion.tick - 1;
            shown.motion.players[1]->position.x -= 0.25f;
            shown.worldMotion.reset();
            shown.worldMotionTicks = 0;
            shown.players[1]->movement.platform = false;
            const auto delayedPosition = shown.motion.players[1]->position;
            f.prediction.present(shown, 1);
            CHECK(shown.valid());
            CHECK(shown.motion.players[1]->position == delayedPosition);
            break;
        }
        f.compare(f.shown());
    }
    CHECK(crossed);
}

TEST_CASE("unsupported and stale platform replay keeps delayed riders with delayed floors",
          "[netplay][platform-rollback][assets]") {
    PlatformFixture f(PlatformMotion::Lift);
    auto delayed = f.state;
    delayed.worldMotion.reset();
    delayed.worldMotionTicks = 0;
    delayed.players[1]->movement.platform = false;
    REQUIRE(delayed.valid());
    for (u64 tick = 11; tick <= 16; ++tick) {
        f.step(f.input(tick));
    }
    REQUIRE(f.state.motion.players[1]->position.y > delayed.motion.players[1]->position.y);
    auto command = f.input(17);
    SECTION("unsupported attack has no geometry endpoint") {
        command.heldButtons = static_cast<u32>(CommandHeld::Attack);
        f.prediction.update(&f.state, std::array{command});
    }
    SECTION("expired platform horizon") {
        auto authority = f.state;
        authority.worldMotionTicks = 1;
        f.prediction.update(&authority, std::array{command});
        f.prediction.update(&authority, std::array{f.input(18)});
    }
    SECTION("stale host checkpoint") {
        f.prediction.update(&f.state, std::array{command});
        for (u64 tick = 0; tick <= LocalMovementPrediction::kStaleTicks; ++tick) {
            f.prediction.update(nullptr, {});
        }
    }
    REQUIRE_FALSE(f.prediction.replayed(1));
    const auto oldPlayer = delayed.players[1];
    const auto oldMotion = delayed.motion.players[1];
    REQUIRE(delayed.geometry);
    const auto oldGeometry = delayed.geometry->objects;
    f.prediction.present(delayed, 1);
    CHECK(delayed.valid());
    CHECK_FALSE(delayed.worldMotion);
    CHECK_FALSE(delayed.players[1]->movement.platform);
    CHECK(delayed.motion.players[1]->position == oldMotion->position);
    CHECK(delayed.motion.players[1]->yaw == oldMotion->yaw);
    CHECK(delayed.players[1]->animation.frame == oldPlayer->animation.frame);
    CHECK(delayed.players[1]->rollback == oldPlayer->rollback);
    CHECK(delayed.players[1]->floor->placement == oldPlayer->floor->placement);
    REQUIRE(delayed.geometry->objects.size() == oldGeometry.size());
    for (usize object = 0; object < oldGeometry.size(); ++object) {
        CHECK(delayed.geometry->objects[object].local == oldGeometry[object].local);
    }
}

TEST_CASE("owned platform seats cannot publish conflicting world clock endpoints",
          "[netplay][platform-rollback][assets]") {
    PlatformFixture f(PlatformMotion::Lift);
    f.context.owners[2] = 2;
    f.context.grants[2] = 7;
    f.state.players[2] = f.state.players[1];
    f.state.motion.players[2] =
        SeatMotion{7, 1, Vec3{f.scene.worldTransform(1) * Vec4{-3, 0, 0, 1}}, 0};
    REQUIRE(f.state.valid());
    f.prediction.begin(f.context, 2, f.collision, &f.scene, &f.layout);
    REQUIRE(f.prediction.setPlayer(1, *f.native[0].figure->actionTree()));
    REQUIRE(f.prediction.setPlayer(2, *f.native[0].figure->actionTree()));
    auto second = f.input(11);
    second.seat = 2;
    second.grant = 7;
    f.prediction.update(&f.state, std::array{f.input(12), second});
    CHECK(f.prediction.replayed(1) == nullptr);
    CHECK(f.prediction.replayed(2) == nullptr);
    const auto unaligned = f.shown();
    CHECK(unaligned.motion.players[1]->position == f.state.motion.players[1]->position);
    CHECK(unaligned.motion.players[2]->position == f.state.motion.players[2]->position);
    second.tick = 12;
    f.prediction.update(&f.state, std::array{second});
    REQUIRE(f.prediction.replayed(1));
    REQUIRE(f.prediction.replayed(2));
    CHECK(f.prediction.replayed(1)->endTick == f.prediction.replayed(2)->endTick);
    const auto aligned = f.shown();
    CHECK(aligned.motion.players[1]->position.y > f.state.motion.players[1]->position.y);
    CHECK(aligned.motion.players[2]->position.y > f.state.motion.players[2]->position.y);
}

TEST_CASE("ordinary replay restores visible gate solidity and contact-only corrections",
          "[netplay][platform-rollback][collision-checkpoint][assets]") {
    const bool contactOnly = GENERATE(false, true);
    PlatformFixture f(PlatformMotion::Lift);
    constexpr f32 kGateZ = 2;
    const std::array gate{
        CollisionTriangle{
            {0, 0, -1}, {Vec3{-10, 0, kGateZ}, Vec3{10, 8, kGateZ}, Vec3{10, 0, kGateZ}}, 0, 2},
        CollisionTriangle{
            {0, 0, -1}, {Vec3{-10, 0, kGateZ}, Vec3{-10, 8, kGateZ}, Vec3{10, 8, kGateZ}}, 0, 2}};
    f.collision.append(gate);
    auto& runtime = f.native[0];
    runtime.actor.place(Vec3{0});
    runtime.floor = {};
    FloorRiding::land(f.native, 0, Vec3{0}, f.collision);
    REQUIRE(runtime.floor.object == 2);
    REQUIRE_FALSE(runtime.floor.placement);
    f.state.players[1]->movement.cameraYaw = 0;
    f.capture();
    f.state.worldMotionTicks = 0;
    REQUIRE(f.scene.objectVisible(0));
    // Bind the guest with the gate initially solid, then change only its
    // authoritative collision flags. Geometry remains visible in both states.
    f.prediction.begin(f.context, 2, f.collision, &f.scene, &f.layout);
    REQUIRE(f.prediction.setPlayer(1, *runtime.figure->actionTree()));
    f.prediction.update(&f.state, {});
    f.collision.setSolid(0, contactOnly);
    f.collision.setContactOnly(0, contactOnly);
    f.state.motion.tick = 11;
    f.capture();
    f.state.worldMotionTicks = 0;
    auto authority = f.state;
    REQUIRE_FALSE(authority.players[1]->movement.platform);
    REQUIRE(authority.worldMotion);
    CHECK(authority.worldMotion->collision[0] ==
          (contactOnly ? WorldMotion::kSolid | WorldMotion::kContactOnly : 0));
    f.prediction.update(&authority, {});
    for (u64 tick = 12; tick <= 51; ++tick) {
        CAPTURE(contactOnly, tick);
        auto command = f.input(tick);
        command.direction = {0, 1};
        command.magnitude = 1;
        f.step(command);
        f.prediction.update(&authority, std::array{command});
        REQUIRE(f.prediction.replayed(1));
        const auto shown = f.shown();
        CHECK(glm::distance(shown.motion.players[1]->position, runtime.actor.position()) < 1e-4f);
        CHECK(shown.players[1]->rollback == runtime.figure->animator().checkpoint());
        if (tick % 8 == 3) {
            authority = f.state;
            authority.worldMotionTicks = 0;
        }
    }
    REQUIRE(runtime.actor.position().z > kGateZ + runtime.actor.radius());

    // A newer host correction closes the same visible gate and relocates the
    // player before it. The old pass-through flags must not survive the rewind.
    f.collision.setSolid(0, true);
    f.collision.setContactOnly(0, false);
    runtime.actor.place(Vec3{0});
    FloorRiding::land(f.native, 0, Vec3{0}, f.collision);
    ++f.state.motion.players[1]->continuity;
    f.capture();
    f.state.worldMotionTicks = 0;
    authority = f.state;
    CHECK(authority.worldMotion->collision[0] == WorldMotion::kSolid);
    f.prediction.update(&authority, {});
    for (u64 tick = 52; tick <= 91; ++tick) {
        CAPTURE(contactOnly, tick);
        auto command = f.input(tick);
        command.direction = {0, 1};
        command.magnitude = 1;
        f.step(command);
        f.prediction.update(&authority, std::array{command});
        REQUIRE(f.prediction.replayed(1));
        const auto shown = f.shown();
        CHECK(glm::distance(shown.motion.players[1]->position, runtime.actor.position()) < 1e-4f);
        CHECK(shown.players[1]->rollback == runtime.figure->animator().checkpoint());
        if (tick % 8 == 3) {
            authority = f.state;
            authority.worldMotionTicks = 0;
        }
    }
    CHECK(runtime.actor.position().z <= kGateZ - runtime.actor.radius() + 1e-4f);
}
} // namespace
