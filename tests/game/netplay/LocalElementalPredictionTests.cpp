#include <array>
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/SessionInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

WorldCollision floorPlane() {
    WorldCollision result;
    result.build({{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}}, 0},
                  {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}}, 0}});
    return result;
}

TreeInfo actionTree() {
    TreeInfo tree;
    tree.name = "WAR";
    TreeNodeInfo root;
    root.name = "ROOT";
    root.type = 1;
    tree.nodes.push_back(root);
    for (const auto name : PlayerAnimator::kSequenceNames) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 8;
        sequence.frameRate = 30;
        sequence.trackOfNode = {-1};
        tree.sequences.push_back(sequence);
    }
    return tree;
}

void actionState(PlayerCombatState& state, const PlayerAnimator& animator) {
    state.rollback = animator.checkpoint();
    state.transitionPose = animator.transitionPose();
    const auto& cursor = animator.player();
    state.animation = {static_cast<u32>(animator.action()), cursor.sequence(), cursor.generation(),
                       cursor.presentationFrame(), cursor.transition()};
}

struct Fixture {
    TreeInfo tree = actionTree();
    WorldCollision floor = floorPlane();
    MatchContext context;
    CombatSnapshot state;
    LocalMovementPrediction prediction;

    Fixture() {
        context.epoch = 3;
        context.scene = 1;
        context.owners = {1, 2, 2, 0};
        context.grants = {1, 4, 2, 0};
        state.motion.epoch = context.epoch;
        state.motion.tick = 10;
        state.motion.cameraContinuity = 1;
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        for (usize seat = 0; seat < 3; ++seat) {
            state.motion.players[seat] = SeatMotion{context.grants[seat], 1, Vec3{0}, 0};
            state.players[seat] = PlayerCombatState{};
            auto& player = *state.players[seat];
            player.health = 100;
            player.movement.enabled = true;
            player.movement.current = true;
            player.movement.ranged = true;
            actionState(player, animator);
        }
    }

    void reset() {
        REQUIRE(state.valid());
        prediction.begin(context, 2, floor);
        REQUIRE(prediction.setPlayer(1, tree));
        REQUIRE(prediction.setPlayer(2, tree));
        prediction.update(&state, {});
    }

    InputCommand command(u64 tick, u8 seat = 1) const {
        InputCommand result;
        result.epoch = context.epoch;
        result.tick = tick;
        result.seat = seat;
        result.grant = context.grants[seat];
        result.heldButtons = static_cast<u32>(CommandHeld::Attack);
        return result;
    }

    CombatSnapshot shown() const {
        auto result = state;
        prediction.present(result, 1);
        REQUIRE(result.valid());
        return result;
    }
};

TEST_CASE("elemental ordinary actions match native party motion and complete release metadata",
          "[netplay][elemental-rollback][assets]") {
    const s32 character = GENERATE(0, 2, 6);
    const u32 element = GENERATE(1U, 2U, 3U, 4U);
    const bool strong = GENERATE(false, true);
    const bool mixed = GENERATE(false, true);
    CAPTURE(character, element, strong, mixed);
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    std::array<PlayerRuntime, 1> native;
    auto& runtime = native.front();
    CharacterSave save;
    save.character = character;
    const u32 flags = element | (mixed ? powerup::kRapidFire | powerup::kFiveWayShot : 0U);
    save.progress().inventory.addPowerup(powerup::kWeapon, flags, 0, 600);
    if (mixed) {
        save.progress().inventory.addPowerup(powerup::kSpeed, 0, 3, 600);
    }
    runtime.actor.spawn(1, save, nullptr, Vec3{0}, 0);
    runtime.actor.setPaceBonus(PowerupEffects::of(save.progress().inventory).paceAdd);
    runtime.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(runtime.figure);
    REQUIRE(runtime.figure->actionTree());
    Fixture fixture;
    fixture.tree = *runtime.figure->actionTree();
    auto& state = *fixture.state.players[1];
    state.movement.radius = runtime.actor.radius();
    state.movement.height = runtime.actor.height();
    state.movement.followHeight = runtime.actor.followPoint().y;
    state.movement.speed = runtime.actor.speed();
    state.modifiers = PlayerWeaponModifiers{flags, 120, mixed};
    state.knockback = runtime.knockback.checkpoint();
    actionState(state, runtime.figure->animator());
    fixture.reset();
    std::vector<CombatSnapshot> history{fixture.state};
    std::vector<LocalMovementPrediction::Release> releases;
    usize eventsSeen = 0;
    PartyMotion::Events events;
    events.select = [](usize, const SelectorInput&, s32) {};
    events.advanceTurbo = [](usize, s32, f32) {};
    events.perform = [&](usize, PartyMotion::Action) { ++eventsSeen; };
    for (u64 tick = 11; tick <= 90; ++tick) {
        CAPTURE(character, element, strong, mixed, tick);
        auto command = fixture.command(tick);
        if (strong) {
            command.heldButtons = static_cast<u32>(CommandHeld::StrongAttack);
        }
        if ((tick - 11) % 32 >= 16) {
            command.heldButtons = 0; // cover wind-up, released input and recovery
        }
        if (tick < 46) {
            command.direction = {0, 1};
            command.magnitude = 1;
        }
        if (mixed) {
            command.aimDirection = glm::normalize(Vec3{0.3f, 0, 1});
            command.heldButtons |= static_cast<u32>(CommandHeld::Strafe);
        }
        REQUIRE(command.valid());
        std::array<PlayInput, InputCommand::kSeats> inputs;
        inputs[1] = SessionInputs::playInput(command);
        runtime.actor.save().progress().inventory.advance(1.0f / 60);
        PartyMotion::step(native, inputs, false, 0, 1, 1.0f / 60, fixture.floor, events);
        const auto& animator = runtime.figure->animator();
        fixture.state.motion.tick = tick;
        auto& motion = *fixture.state.motion.players[1];
        motion.position = runtime.actor.position();
        motion.yaw = std::remainder(runtime.actor.yaw(), kTwoPi);
        actionState(state, animator);
        state.knockback = runtime.knockback.checkpoint();
        state.rollbackInput = command;
        history.push_back(fixture.state);
        if (animator.released() || animator.strongReleased()) {
            releases.push_back({tick, animator.player().generation(), motion,
                                animator.strongReleased(), command.aimDirection.has_value(),
                                animator.attackSeconds(), static_cast<u8>(mixed ? 5 : 1), flags});
        }
        const auto& delayed = history[static_cast<usize>(tick > 16 ? tick - 16 : 0)];
        const auto inventory = runtime.actor.save().progress().inventory;
        const auto beforeEvents = eventsSeen;
        fixture.prediction.update(&delayed, std::array{command});
        const auto shown = fixture.shown();
        REQUIRE(fixture.prediction.replayed(1));
        CHECK(shown.players[1]->rollback == animator.checkpoint());
        CHECK(shown.players[1]->knockback == runtime.knockback.checkpoint());
        CHECK(glm::distance(shown.motion.players[1]->position, runtime.actor.position()) ==
              Approx(0).margin(0.0001));
        CHECK(std::remainder(shown.motion.players[1]->yaw - runtime.actor.yaw(), kTwoPi) ==
              Approx(0).margin(0.0001));
        const auto* replay = fixture.prediction.replayed(1);
        usize pending = 0;
        for (const auto& release : releases) {
            if (release.tick <= delayed.motion.tick) {
                continue;
            }
            REQUIRE(pending < replay->releases.size());
            const auto& actual = replay->releases[pending++];
            CHECK(actual.tick == release.tick);
            CHECK(actual.generation == release.generation);
            CHECK(actual.strong == release.strong);
            CHECK(actual.weaponFlags == flags);
            CHECK(actual.shots == release.shots);
        }
        CHECK(replay->releases.size() == pending);
        // Extra render submissions must not advance the action or execute the
        // native callback sinks, nor mutate the real inventory or health.
        for (usize render = 0; render < tick % 4; ++render) {
            CHECK(fixture.shown().players[1]->rollback == animator.checkpoint());
        }
        CHECK(runtime.actor.save().progress().inventory == inventory);
        CHECK(eventsSeen == beforeEvents);
        CHECK(shown.players[1]->health == 100);
        CHECK(shown.projectiles.empty());
    }
    if (character == 6 && strong) {
        // The native Sorceress heavy action has its own class effect, not an
        // ordinary thrown weapon (PlayerAnimator::play's strong-release gate).
        CHECK(releases.empty());
    } else {
        CHECK_FALSE(releases.empty());
    }
}

TEST_CASE("elemental expiry and unsupported controls withdraw the whole speculative prefix",
          "[netplay][elemental-rollback]") {
    Fixture fixture;
    auto& player = *fixture.state.players[1];
    player.modifiers = PlayerWeaponModifiers{4U | powerup::kRapidFire | powerup::kFiveWayShot, 24};
    fixture.reset();
    // A throw releases after both its wind-up and release sequence. Sixteen
    // application ticks need not finish both eight-frame native phases.
    for (u64 tick = 11; tick <= 34; ++tick) {
        fixture.prediction.update(&fixture.state, std::array{fixture.command(tick)});
    }
    REQUIRE(fixture.prediction.replayed(1));
    REQUIRE_FALSE(fixture.prediction.replayed(1)->releases.empty());
    auto input = fixture.command(35);
    SECTION("expiration budget") {}
    SECTION("inventory selection") {
        input.pressedButtons = static_cast<u32>(CommandPress::SelectorRight);
        player.modifiers->ticks = 120;
        ++fixture.state.motion.tick;
    }
    fixture.prediction.update(&fixture.state, std::array{input});
    CHECK(fixture.prediction.replayed(1) == nullptr);
    CHECK(fixture.shown().players[1]->rollback == player.rollback);
    fixture.state.motion.tick = 35;
    player.modifiers.reset();
    player.rollbackInput = input;
    player.rollbackInput->heldButtons = 0;
    player.rollbackInput->pressedButtons = 0;
    fixture.prediction.update(&fixture.state, {});
    for (u64 tick = 36; tick <= 59; ++tick) {
        fixture.prediction.update(&fixture.state, std::array{fixture.command(tick)});
    }
    const auto* replay = fixture.prediction.replayed(1);
    REQUIRE(replay);
    REQUIRE_FALSE(replay->releases.empty());
    for (const auto& release : replay->releases) {
        CHECK(release.weaponFlags == 0);
        CHECK(release.shots == 1);
    }
}

TEST_CASE("element expiry preserves native latched attack speed until the next sequence",
          "[netplay][elemental-rollback]") {
    const bool speed = GENERATE(false, true);
    Fixture fixture;
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    native.setAttackSpeed(!speed, speed);
    native.update(PlayerMotion::Stand, 1, 1.0f / 60, PlayerDeed::Attack);
    auto& player = *fixture.state.players[1];
    actionState(player, native);
    player.modifiers = PlayerWeaponModifiers{4, 120}; // Rapid/Speed has just expired
    REQUIRE(player.rollback->rapid == !speed);
    REQUIRE(player.rollback->speed == speed);
    fixture.reset();
    for (u64 tick = 11; tick <= 34; ++tick) {
        auto input = fixture.command(tick);
        native.setAttackSpeed(false, false);
        native.update(PlayerMotion::Stand, 1, 1.0f / 60, PlayerDeed::Attack);
        fixture.prediction.update(&fixture.state, std::array{input});
        CHECK(fixture.shown().players[1]->rollback == native.checkpoint());
        const auto* replay = fixture.prediction.replayed(1);
        REQUIRE(replay);
        for (const auto& release : replay->releases) {
            CHECK(release.weaponFlags == 4);
            CHECK(release.shots == 1);
        }
    }
    CHECK(player.rollback->rapid == !speed);
    CHECK(player.rollback->speed == speed);
}

TEST_CASE("elemental replay isolates seats and does not admit melee or turbo side effects",
          "[netplay][elemental-rollback]") {
    Fixture fixture;
    fixture.state.players[1]->modifiers = PlayerWeaponModifiers{1, 120};
    fixture.state.players[2]->modifiers = PlayerWeaponModifiers{4 | powerup::kThreeWayShot, 120};
    fixture.reset();
    for (u64 tick = 11; tick <= 34; ++tick) {
        fixture.prediction.update(&fixture.state,
                                  std::array{fixture.command(tick, 1), fixture.command(tick, 2)});
    }
    for (const u8 seat : {u8{1}, u8{2}}) {
        const auto* replay = fixture.prediction.replayed(seat);
        REQUIRE(replay);
        REQUIRE_FALSE(replay->releases.empty());
        for (const auto& release : replay->releases) {
            CHECK(release.motion.grant == fixture.context.grants[seat]);
            CHECK(release.weaponFlags == fixture.state.players[seat]->modifiers->flags);
        }
    }
    auto& player = *fixture.state.players[1];
    player.movement.ranged = false;
    player.melee = PlayerMeleeState{};
    player.melee->selection.reach = 2;
    player.melee->selection.range = 30;
    player.melee->selection.facingDot = 0.707f;
    player.melee->selection.targets.push_back({{0, 0, 1}, {0, 0, 1}, 1, 1, false});
    fixture.reset();
    auto command = fixture.command(11);
    SECTION("stationary melee") {}
    SECTION("turbo request without a certified ordinary region") {
        command.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
    }
    fixture.prediction.update(&fixture.state, std::array{command});
    CHECK(fixture.prediction.replayed(1) == nullptr);
    CHECK(fixture.shown().players[1]->rollback == player.rollback);
    CHECK(fixture.shown().projectiles.empty());
}
} // namespace
