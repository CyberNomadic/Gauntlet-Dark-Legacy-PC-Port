#include <array>
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/LocalProjectilePrediction.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PlayerAttacks.h"
#include "game/screens/SessionInputs.h"
#include "game/world/CameraMovementLimit.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

/** Native targeting and party motion run against real, fixed enemy cylinders.
 * Only the host calls PartyMotion; replay receives encoded geometry and actions. */
struct MovingMeleeFixture {
    test::FakeRenderDevice device;
    ClassDataSet classes;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    LevelOpponents opponents;
    LevelFixtures fixtures;
    PlayerAttacks attacks;
    std::array<PlayerRuntime, 2> native;
    PlayerAttacks::Targets targets{opponents, fixtures, {}};
    PartyMotion::Events events;
    MatchContext context;
    CombatSnapshot authority;
    LocalMovementPrediction prediction;
    std::array<s32, 2> enemyIds{};
    usize members = 1;
    usize performed = 0;
    usize blows = 0;
    usize throws = 0;

    explicit MovingMeleeFixture(s32 character = 0, bool stepTarget = true, bool second = false,
                                f32 slope = 0) {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        device.recordDraws = false;
        REQUIRE(classes.load(root / "PDATA"));
        // The world is owned by this fixture; its synthetic ground lets the native
        // target and motion queries see exactly the same independently built mesh.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned world fixture
        auto& floor = const_cast<WorldCollision&>(world.collision());
        const Vec3 normal = glm::normalize(Vec3{0, 1, -slope});
        const Vec3 a{-100, -100 * slope, -100};
        const Vec3 b{100, -100 * slope, -100};
        const Vec3 c{100, 100 * slope, 100};
        const Vec3 d{-100, 100 * slope, 100};
        floor.build({{normal, {a, b, c}, 0}, {normal, {a, c, d}, 0}});
        arsenal.bind({device, classes, weapons, floor, effects, audio, nullptr, {}, false, false});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        auto& enemies = opponents.enemies();
        enemies.open(device, root, &floor, 4, {}, 1);
        REQUIRE(enemies.loadKind(kGruntKind));
        members = second ? 2 : 1;
        context.epoch = 6;
        context.scene = 1;
        context.owners = {1, 2, second ? u8{2} : u8{0}, 0};
        context.grants = {1, 3, second ? u32{7} : u32{0}, 0};
        authority.motion.epoch = context.epoch;
        authority.motion.tick = 10;
        authority.motion.cameraContinuity = 1;
        authority.motion.camera = {{0, 12, -18}, 0.5f, 0, 0};
        authority.motion.players[0] = SeatMotion{1, 1, Vec3{-30, 0, 0}, 0};
        authority.players[0].emplace().health = 100;
        for (usize index = 0; index < members; ++index) {
            auto& member = native[index];
            CharacterSave save;
            save.character = character;
            save.progress().health = 100;
            const auto seat = static_cast<s32>(index + 1);
            const Vec3 feet{static_cast<f32>(index) * 30, 0, 0};
            member.actor.spawn(seat, save, classes.stats(character), feet, 0);
            member.figure = PlayerFigure::load(device, root, save, false);
            REQUIRE(member.figure);
            REQUIRE(member.figure->actionTree());
            const f32 gap = stepTarget ? 2.5f : 0.6f;
            Vec3 enemyPosition =
                feet + Vec3{0, 0, member.actor.reach() + gap + enemyKind(kGruntKind).radius};
            enemyPosition.y = enemyPosition.z * slope;
            const auto id =
                enemies.spawn({.kind = kGruntKind, .position = enemyPosition, .placed = true}, {});
            REQUIRE(id);
            enemyIds[index] = *id;
            authority.motion.players[index + 1] = SeatMotion{context.grants[index + 1], 1, feet, 0};
            auto& player = authority.players[index + 1].emplace();
            player.health = 100;
            player.movement.enabled = player.movement.current = true;
            player.movement.speed = member.actor.speed();
            player.movement.radius = member.actor.radius();
            player.movement.height = member.actor.height();
            player.movement.followHeight = member.actor.followPoint().y - feet.y;
        }
        targets.players = std::span{native.data(), members};
        events.select = [](usize, const SelectorInput&, s32) {};
        events.advanceTurbo = [](usize, s32, f32) {};
        events.perform = [&](usize, PartyMotion::Action action) {
            ++performed;
            blows += action == PartyMotion::Action::Melee ? 1 : 0;
            throws += action == PartyMotion::Action::ThrowWeapon ||
                              action == PartyMotion::Action::StrongThrow
                          ? 1
                          : 0;
        };
        events.attackDeed = [&](usize index, bool strong, bool moved, const Vec3& facing) {
            const auto& member = native[index];
            return attacks.attackDeed(member.actor, strong, targets, moved,
                                      member.figure->animator().meleeChain(), facing);
        };
        events.meleeSense = [&](usize index, bool held, const Vec3& facing) {
            return attacks.meleeSense(native[index].actor, held, targets, facing);
        };
        events.aim = [&](usize index) {
            return attacks.aim(native[index].actor, native[index].actor.facing(), targets);
        };
        events.resolveMovement = [&](usize index, const Vec3& from, const Vec3& to) {
            return opponents.resolveMovement(native[index].actor, from, to);
        };
        events.limitMovement = [&](usize index, const Vec3& from, const Vec3& to) {
            const auto& movement = authority.players[index + 1]->movement;
            return movement.cameraLimited
                       ? CameraMovementLimit::constrain(
                             from, to, movement.attention, authority.motion.camera,
                             {authority.motion.horizontalFov, authority.motion.aspect},
                             Vec3{0, movement.followHeight, 0})
                       : to;
        };
    }
    CombatSnapshot capture(u64 tick) const {
        auto host = authority;
        host.motion.tick = tick;
        for (usize index = 0; index < members; ++index) {
            const auto& member = native[index];
            const auto& actor = member.actor;
            auto& player = *host.players[index + 1];
            host.motion.players[index + 1]->position = actor.position();
            host.motion.players[index + 1]->yaw = std::remainder(actor.yaw(), kTwoPi);
            const auto& animator = member.figure->animator();
            const auto& cursor = animator.player();
            player.rollback = animator.checkpoint();
            player.transitionPose = animator.transitionPose();
            player.knockback = member.knockback.checkpoint();
            player.animation = {static_cast<u32>(animator.action()), cursor.sequence(),
                                cursor.generation(), cursor.presentationFrame(),
                                cursor.transition()};
            const auto selection = attacks.meleeAcquisition(actor, targets);
            REQUIRE(selection);
            auto& melee = player.melee.emplace();
            melee.selection = *selection;
            melee.captureYaw = host.motion.players[index + 1]->yaw;
            melee.assistedAim = attacks.aim(actor, actor.facing(), targets);
            auto& moving = melee.moving.emplace();
            moving.origin = actor.position();
            moving.radius = PlayerMeleeState::kMaxRadius;
            moving.ticks = PlayerMeleeState::kMaxTicks;
            moving.targets = opponents.enemies().meleeGeometry();
            if (player.weapon) {
                player.weapon->target = attacks.aim(actor, actor.facing(), targets);
            }
        }
        return host;
    }
    void begin() {
        authority = capture(authority.motion.tick);
        prediction.begin(context, 2, world.collision());
        for (usize index = 0; index < members; ++index) {
            REQUIRE(prediction.setPlayer(static_cast<u8>(index + 1),
                                         *native[index].figure->actionTree()));
        }
        accept(authority);
    }
    void accept(const CombatSnapshot& host, std::span<const InputCommand> commands = {}) {
        const auto bytes = CombatPacket::encode(host);
        REQUIRE(bytes);
        const auto decoded = CombatPacket::decode(*bytes);
        REQUIRE(decoded);
        authority = *decoded;
        prediction.update(&authority, commands);
    }
    InputCommand command(u64 tick, u8 seat = 1) const {
        InputCommand input;
        input.epoch = context.epoch;
        input.tick = tick;
        input.grant = context.grants[seat];
        input.seat = seat;
        return input;
    }
    void step(std::span<const InputCommand> commands) {
        std::array<PlayInput, InputCommand::kSeats> controls;
        for (const auto& command : commands) {
            REQUIRE(command.valid());
            controls[command.seat] = SessionInputs::playInput(command);
        }
        PartyMotion::step(std::span{native.data(), members}, controls, false,
                          authority.players[1]->movement.cameraYaw, 1,
                          1.0f / static_cast<f32>(context.tickRate), world.collision(), events);
    }
    CombatSnapshot shown(f32 blend = 1) const {
        auto state = authority;
        prediction.present(state, blend);
        REQUIRE(state.valid());
        return state;
    }
    void checkNative(usize index = 0) const {
        const usize seat = index + 1;
        const auto state = shown();
        REQUIRE(state.players[seat]->rollback);
        CHECK(*state.players[seat]->rollback == native[index].figure->animator().checkpoint());
        CHECK(state.players[seat]->knockback == native[index].knockback.checkpoint());
        CHECK(glm::distance(state.motion.players[seat]->position, native[index].actor.position()) <
              1e-5f);
        CHECK(state.motion.players[seat]->yaw == Approx(native[index].actor.yaw()).margin(1e-5f));
        CHECK(state.players[seat]->health == 100);
        CHECK(state.projectiles.empty());
        const auto* replay = prediction.replayed(static_cast<u8>(seat));
        REQUIRE(replay != nullptr);
        CHECK(replay->releases.empty());
    }
};

TEST_CASE("bounded native melee acquisition can release a weapon with release-local aim",
          "[netplay][rollback][moving-melee][bounded-throw][assets]") {
    const bool strong = GENERATE(false, true);
    const bool cursor = GENERATE(false, true);
    MovingMeleeFixture fixture(0, false);
    auto& runtime = fixture.native[0];
    const auto distant = fixture.opponents.enemies().spawn(
        {.kind = kGruntKind, .position = {0.5f, 0, -12}, .placed = true}, {});
    REQUIRE(distant);
    REQUIRE_FALSE(fixture.attacks.rangedOnly(runtime.actor, fixture.targets));
    const auto tuning = fixture.arsenal.weaponThrow(runtime.actor, 0);
    REQUIRE(tuning);
    auto& descriptor = fixture.authority.players[1]->weapon.emplace();
    descriptor.character = static_cast<u32>(tuning->character);
    descriptor.hand = tuning->hand;
    descriptor.speed = tuning->speed;
    descriptor.resource = 7;
    auto* archive = runtime.figure->missileArchive();
    REQUIRE(archive != nullptr);
    ProjectileResources resources;
    REQUIRE(resources.addTree(7, *archive, runtime.figure->missileTree(), fixture.device));
    LocalProjectilePrediction projectiles;
    projectiles.begin(fixture.context, 2, fixture.world.collision());
    PlayerMissiles nativeFlight;
    std::optional<Vec3> nativeAim;
    u64 nativeGeneration = 0;
    usize nativeReleases = 0;
    fixture.events.perform = [&](usize index, PartyMotion::Action action) {
        if (action != PartyMotion::Action::ThrowWeapon &&
            action != PartyMotion::Action::StrongThrow) {
            return;
        }
        auto& member = fixture.native[index];
        nativeAim = member.cursorAiming
                        ? std::nullopt
                        : fixture.attacks.aim(member.actor, member.actor.facing(), fixture.targets);
        const auto prepared = fixture.arsenal.prepareWeapon(
            member.actor, *member.figure, member.actor.facing(),
            action == PartyMotion::Action::StrongThrow ? 2.0f : 1.0f, true, nativeAim);
        REQUIRE(prepared);
        REQUIRE(prepared->missiles.size() == 1);
        REQUIRE(nativeFlight.launch(prepared->missiles.front()));
        nativeGeneration = member.figure->animator().player().generation();
        ++nativeReleases;
    };
    fixture.begin();
    const auto originalAim = fixture.authority.players[1]->weapon->target;
    REQUIRE(originalAim);
    REQUIRE(originalAim->z > 0);
    bool observed = false;
    for (u64 tick = 11; tick <= 70 && !observed; ++tick) {
        auto input = fixture.command(tick);
        // The near enemy invalidates all-bearing ranged permission, but walking
        // away selects a real throw. Its target is across the opposite bearing.
        input.direction = {0, -1};
        input.magnitude = 0.15f;
        if (cursor) {
            input.aimDirection = Vec3{0, 0, -1};
        }
        if (tick == 11) {
            input.heldButtons =
                static_cast<u32>(strong ? CommandHeld::StrongAttack : CommandHeld::Attack);
        }
        const std::array commands{input};
        fixture.step(commands);
        nativeFlight.update(1.0f / 60, &fixture.world.collision());
        fixture.prediction.update(&fixture.authority, commands);
        projectiles.update(&fixture.authority, fixture.prediction, resources, commands);
        const auto* replay = fixture.prediction.replayed(1);
        REQUIRE(replay != nullptr);
        CHECK(*fixture.shown().players[1]->rollback == runtime.figure->animator().checkpoint());
        if (nativeReleases != 0) {
            REQUIRE(replay->releases.size() == 1);
            const auto& release = replay->releases.front();
            CHECK(release.tick == tick);
            CHECK(release.generation == nativeGeneration);
            CHECK(release.strong == strong);
            CHECK(release.targetResolved);
            CHECK(release.target == nativeAim);
            CHECK(release.target != originalAim);
            if (!cursor) {
                REQUIRE(nativeAim);
                CHECK(nativeAim->z < 0);
            }
            auto shown = fixture.shown();
            projectiles.present(shown, 1);
            REQUIRE(shown.projectiles.size() == 1);
            REQUIRE(nativeFlight.count() == 1);
            const auto& component = shown.projectiles.front();
            REQUIRE(component.rollback);
            CHECK(component.rollback->flight.position == nativeFlight.missile(0).position);
            CHECK(component.rollback->flight.velocity == nativeFlight.missile(0).velocity);
            CHECK(component.rollback->flight.age == nativeFlight.missile(0).age);
            CHECK(projectiles.soundReleases().size() == 1);
            observed = true;
        } else if (tick % 4 == 0) {
            fixture.accept(fixture.capture(tick));
        }
    }
    REQUIRE(observed);
}

TEST_CASE("moving melee rewinds native class actions and released-stick lunges across delayed "
          "checkpoints",
          "[netplay][rollback][moving-melee][assets]") {
    const s32 character = GENERATE(0, 2, 5, 6);
    const bool lunge = GENERATE(false, true);
    MovingMeleeFixture fixture(character, lunge);
    fixture.begin();
    std::vector<CombatSnapshot> history{fixture.authority};
    bool lunged = false;
    bool recovered = false;
    bool continuedWithoutStick = false;
    for (u64 tick = 11; tick <= 140; ++tick) {
        auto input = fixture.command(tick);
        if (tick < 76 && tick % 19 != 0) {
            input.heldButtons = static_cast<u32>(CommandHeld::Attack);
            if (tick % 19 == 1) {
                input.pressedButtons = static_cast<u32>(CommandPress::Attack);
            }
        }
        if (tick == 11 || (!lunge && tick < 32)) {
            input.direction = glm::normalize(Vec2{tick > 22 ? 0.2f : 0, 1});
            input.magnitude = lunge ? 1 : 0.3f;
        }
        const auto before = fixture.native[0].actor.position();
        fixture.step(std::array{input});
        const auto& animator = fixture.native[0].figure->animator();
        lunged = lunged || animator.lunging();
        continuedWithoutStick = continuedWithoutStick ||
                                (input.magnitude == 0 && animator.lunging() &&
                                 glm::distance(before, fixture.native[0].actor.position()) > 0);
        recovered = recovered || (tick > 76 && !animator.meleeing());
        auto host = fixture.capture(tick);
        host.players[1]->rollbackInput = input;
        history.push_back(host);
        const usize emitted = fixture.performed;
        if (tick >= 16 && tick % 3 == 1) {
            fixture.accept(history[static_cast<usize>(tick - 4 - 10)], std::array{input});
        } else {
            fixture.prediction.update(&fixture.authority, std::array{input});
        }
        CAPTURE(character, lunge, tick, animator.action());
        fixture.checkNative();
        CHECK(fixture.performed == emitted);
        CHECK(fixture.opponents.enemies().healthOf(fixture.enemyIds[0]) ==
              enemyKind(kGruntKind).healthAtTier(1));
        CHECK(fixture.shown().motion.players[0]->position == Vec3{-30, 0, 0});
        for (s32 frame = 0; frame < 4; ++frame) {
            fixture.shown(static_cast<f32>(frame) / 3);
        }
        fixture.checkNative();
    }
    CHECK(fixture.blows > 0);
    CHECK(fixture.throws == 0);
    CHECK(recovered);
    CHECK(glm::distance(fixture.native[0].actor.position(), Vec3{0}) > 0);
    if (lunge) {
        CHECK(lunged);
        CHECK(continuedWithoutStick);
    }
}

TEST_CASE("moving melee rejects exhausted geometry or unsupported fixture and body corrections",
          "[netplay][rollback][moving-melee][assets]") {
    enum class Unsafe : u8 { Deadline, Region, Body, Item, Disabled };
    const auto unsafe =
        GENERATE(Unsafe::Deadline, Unsafe::Region, Unsafe::Body, Unsafe::Item, Unsafe::Disabled);
    MovingMeleeFixture fixture;
    fixture.begin();
    auto host = fixture.authority;
    auto& moving = *host.players[1]->melee->moving;
    if (unsafe == Unsafe::Deadline) {
        moving.ticks = 1;
    } else if (unsafe == Unsafe::Region) {
        moving.radius = 0.01f;
    } else if (unsafe == Unsafe::Body) {
        moving.targets.front().base = Vec3{0, 0, fixture.native[0].actor.radius()};
        moving.targets.front().acquisition.point = moving.targets.front().base + Vec3{0, 3, 0};
    } else if (unsafe == Unsafe::Item) {
        Obstacle blocker;
        blocker.centre = {0, 0, 0.5f};
        blocker.halfAcross = blocker.halfAlong = 1;
        blocker.height = 10;
        moving.obstacles.push_back(blocker);
    } else {
        host.players[1]->melee->moving.reset();
    }
    // A newer host tick replaces all raw geometry as one transaction.
    host.motion.tick = 11;
    fixture.accept(host);
    auto input = fixture.command(unsafe == Unsafe::Deadline ? 13 : 12);
    input.direction = {0, 1};
    input.magnitude = 1;
    input.heldButtons = static_cast<u32>(CommandHeld::Attack);
    const auto unchanged = CombatPacket::encode(fixture.authority);
    fixture.prediction.update(&fixture.authority, std::array{input});
    const auto shown = fixture.shown();
    CHECK(fixture.prediction.replayed(1) == nullptr);
    CHECK(shown.motion.players[1]->position == fixture.authority.motion.players[1]->position);
    CHECK(shown.players[1]->rollback == fixture.authority.players[1]->rollback);
    CHECK(shown.projectiles.empty());
    CHECK(CombatPacket::encode(fixture.authority) == unchanged);
}

TEST_CASE("a rejected moving swing corrects one local seat without rewinding the other",
          "[netplay][rollback][moving-melee][assets]") {
    MovingMeleeFixture fixture(0, true, true);
    fixture.begin();
    const auto old = fixture.authority;
    std::array<InputCommand, 2> input{fixture.command(11), fixture.command(11, 2)};
    for (auto& command : input) {
        command.direction = {0, 1};
        command.magnitude = 1;
        command.heldButtons = static_cast<u32>(CommandHeld::Attack);
    }
    fixture.prediction.update(&fixture.authority, input);
    const auto predicted = fixture.shown();
    REQUIRE(predicted.players[1]->rollback);
    REQUIRE(predicted.players[2]->rollback);
    CHECK(predicted.players[1]->animation.action ==
          static_cast<u32>(PlayerAnimator::Action::Step1));
    CHECK(predicted.players[2]->animation.action ==
          static_cast<u32>(PlayerAnimator::Action::Step1));

    // The host resolved the first command as neutral, while the second command
    // reached its deadline normally. No speculative swing may survive that fact.
    input[0].heldButtons = 0;
    input[0].direction = Vec2{0};
    input[0].magnitude = 0;
    fixture.step(input);
    auto host = fixture.capture(11);
    host.players[1]->rollbackInput = input[0];
    host.players[2]->rollbackInput = input[1];
    fixture.accept(host);
    fixture.checkNative(1);
    CHECK(fixture.shown().players[1]->rollback ==
          fixture.native[0].figure->animator().checkpoint());
    CHECK(fixture.shown().players[1]->knockback == fixture.native[0].knockback.checkpoint());
    CHECK_FALSE(PlayerAnimator::isMelee(
        static_cast<PlayerAnimator::Action>(fixture.shown().players[1]->animation.action)));
    CHECK(fixture.shown().players[2]->rollback == predicted.players[2]->rollback);
    // Small horizontal corrections deliberately ease instead of snapping. The
    // corrected action is already exact; reject old authority and require the
    // visual offset to converge without applying the first seat's error to P2.
    f32 error = glm::distance(fixture.shown().motion.players[1]->position,
                              fixture.native[0].actor.position());
    REQUIRE(error > 0);
    CHECK(error <=
          glm::distance(predicted.motion.players[1]->position, fixture.native[0].actor.position()));
    for (s32 update = 0; update < 29; ++update) {
        fixture.prediction.update(&old, {});
        const auto shown = fixture.shown();
        const f32 next =
            glm::distance(shown.motion.players[1]->position, fixture.native[0].actor.position());
        CHECK(next <= error);
        CHECK(shown.players[1]->rollback == fixture.native[0].figure->animator().checkpoint());
        fixture.checkNative(1);
        error = next;
    }
    fixture.checkNative(0);
    fixture.checkNative(1);
    CHECK(fixture.shown().projectiles.empty());
}

TEST_CASE("moving melee preserves native class power paces and fresh strong buffering",
          "[netplay][rollback][moving-melee][assets]") {
    const s32 character = GENERATE(0, 2, 5, 6);
    MovingMeleeFixture fixture(character, false);
    fixture.begin();
    bool power = false;
    bool movingPower = false;
    bool strongStarted = false;
    std::vector<CombatSnapshot> history{fixture.authority};
    for (u64 tick = 11; tick <= 100; ++tick) {
        auto input = fixture.command(tick);
        input.direction = {0, 1};
        input.magnitude = 0.1f;
        const auto& before = fixture.native[0].figure->animator();
        if (!strongStarted && before.meleeChain() > 0) {
            strongStarted = true;
        }
        input.heldButtons =
            static_cast<u32>(strongStarted ? CommandHeld::StrongAttack : CommandHeld::Attack);
        const auto position = fixture.native[0].actor.position();
        fixture.step(std::array{input});
        const auto action = fixture.native[0].figure->animator().action();
        const bool powerNow = action == PlayerAnimator::Action::PowerClose ||
                              action == PlayerAnimator::Action::PowerMed ||
                              action == PlayerAnimator::Action::Spin;
        power = power || powerNow;
        movingPower = movingPower || (powerNow && fixture.native[0].actor.position() != position);
        auto host = fixture.capture(tick);
        host.players[1]->rollbackInput = input;
        history.push_back(host);
        if (tick >= 16 && tick % 3 == 1) {
            fixture.accept(history[static_cast<usize>(tick - 4 - 10)], std::array{input});
        } else {
            fixture.prediction.update(&fixture.authority, std::array{input});
        }
        CAPTURE(character, tick, action);
        fixture.checkNative();
    }
    CHECK(strongStarted);
    CHECK(power);
    CHECK(movingPower);
    CHECK(fixture.throws == 0);
}

TEST_CASE("moving melee replays the native camera boundary rewrite before target refinement",
          "[netplay][rollback][moving-melee][assets]") {
    MovingMeleeFixture fixture(0, false);
    fixture.authority.motion.camera = {{-30, 15, -25}, std::atan2(15.0f, 25.0f), 0, 0};
    fixture.authority.players[1]->movement.cameraLimited = true;
    fixture.begin();
    const auto before = fixture.native[0].actor.position();
    const Vec3 desired = before + Vec3{1, 0, 1};
    const auto constrained = CameraMovementLimit::constrain(
        before, desired, fixture.authority.players[1]->movement.attention,
        fixture.authority.motion.camera,
        {fixture.authority.motion.horizontalFov, fixture.authority.motion.aspect},
        Vec3{0, fixture.authority.players[1]->movement.followHeight, 0});
    REQUIRE(glm::distance(constrained, desired) > 0.01f);
    for (u64 tick = 11; tick < 18; ++tick) {
        auto input = fixture.command(tick);
        input.direction = glm::normalize(Vec2{0.2f, 1});
        input.magnitude = 0.2f;
        input.heldButtons = static_cast<u32>(CommandHeld::Attack);
        fixture.step(std::array{input});
        fixture.prediction.update(&fixture.authority, std::array{input});
        CAPTURE(tick);
        fixture.checkNative();
    }
    CHECK(fixture.native[0].actor.position() != before);
}

TEST_CASE("moving melee preserves cursor deadzones and explicit strafe during native lunges",
          "[netplay][rollback][moving-melee][assets]") {
    enum class Aim : u8 { CursorTurn, CursorDeadzone, Strafe };
    const auto aim = GENERATE(Aim::CursorTurn, Aim::CursorDeadzone, Aim::Strafe);
    MovingMeleeFixture fixture;
    fixture.begin();
    bool lunged = false;
    bool constrainedTurn = false;
    for (u64 tick = 11; tick < 21; ++tick) {
        auto input = fixture.command(tick);
        input.direction = glm::normalize(Vec2{tick < 14 ? 0 : 0.2f, 1});
        input.magnitude = 1;
        input.heldButtons = static_cast<u32>(CommandHeld::Attack);
        if (aim == Aim::Strafe) {
            input.heldButtons |= static_cast<u32>(CommandHeld::Strafe);
        } else if (aim == Aim::CursorDeadzone && tick >= 14) {
            input.aimDirection = Vec3{0};
        } else {
            input.aimDirection = glm::normalize(Vec3{tick < 14 ? 0 : 0.25f, 0, 1});
        }
        const auto& animator = fixture.native[0].figure->animator();
        const f32 yaw = fixture.native[0].actor.yaw();
        const bool constrained = animator.lunging() && animator.turnScale() < 1;
        fixture.step(std::array{input});
        fixture.prediction.update(&fixture.authority, std::array{input});
        CAPTURE(aim, tick);
        fixture.checkNative();
        lunged = lunged || animator.lunging();
        if (constrained && tick >= 14) {
            constrainedTurn = true;
            // Cursor changes must not bypass the native attack's locked turn.
            CHECK(fixture.native[0].actor.yaw() == Approx(yaw).margin(1e-5f));
        }
    }
    CHECK(lunged);
    CHECK(constrainedTurn);
    CHECK(fixture.throws == 0);
}
TEST_CASE("moving melee follows native static ramp support throughout its lunge",
          "[netplay][rollback][moving-melee][assets]") {
    constexpr f32 kSlope = 0.1f;
    MovingMeleeFixture fixture(0, true, false, kSlope);
    fixture.begin();
    bool lunged = false;
    for (u64 tick = 11; tick < 19; ++tick) {
        auto input = fixture.command(tick);
        input.direction = {0, 1};
        input.magnitude = 1;
        input.heldButtons = static_cast<u32>(CommandHeld::Attack);
        fixture.step(std::array{input});
        fixture.prediction.update(&fixture.authority, std::array{input});
        CAPTURE(tick);
        fixture.checkNative();
        lunged = lunged || fixture.native[0].figure->animator().lunging();
        const auto feet = fixture.native[0].actor.position();
        CHECK(feet.y == Approx(feet.z * kSlope).margin(1e-5f));
    }
    CHECK(lunged);
    CHECK(fixture.native[0].actor.position().y > 0);
    CHECK(fixture.throws == 0);
}
} // namespace
