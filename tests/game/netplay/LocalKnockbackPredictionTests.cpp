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
#include "game/screens/PartyMotion.h"
#include "game/screens/SessionInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct KnockbackFixture {
    test::FakeRenderDevice device;
    std::array<PlayerRuntime, 1> native;
    WorldCollision floor;
    MatchContext context;
    CombatSnapshot authority;
    LocalMovementPrediction prediction;
    PartyMotion::Events events;
    usize performed = 0;

    KnockbackFixture() {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        device.recordDraws = false;
        floor.build({{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}}, 0},
                     {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}}, 0}});
        auto& runtime = native.front();
        CharacterSave save;
        save.progress().health = 100;
        runtime.actor.spawn(1, save, nullptr, Vec3{0}, 0);
        runtime.figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(runtime.figure);
        REQUIRE(runtime.figure->actionTree());
        context.epoch = 4;
        context.scene = 1;
        context.owners = {1, 2, 2, 0};
        context.grants = {1, 3, 7, 0};
        authority.motion.epoch = context.epoch;
        authority.motion.tick = 10;
        authority.motion.cameraContinuity = 1;
        for (usize seat = 0; seat < 3; ++seat) {
            authority.motion.players[seat] =
                SeatMotion{context.grants[seat], 1, Vec3{static_cast<f32>(seat) * 20, 0, 0}, 0};
            auto& player = authority.players[seat].emplace();
            player.health = 100;
            player.movement.enabled = player.movement.current = true;
            player.movement.speed = runtime.actor.speed();
            player.movement.radius = runtime.actor.radius();
            player.movement.height = runtime.actor.height();
            player.movement.followHeight = runtime.actor.followPoint().y;
            captureAction(player, runtime.figure->animator());
        }
        EnemyCombatState enemy;
        enemy.instance = 42;
        enemy.kind = 1;
        enemy.health = enemy.fullHealth = 100;
        authority.enemies.push_back(enemy);
        events.perform = [&](usize, PartyMotion::Action) { ++performed; };
        events.select = [](usize, const SelectorInput&, s32) {};
        events.advanceTurbo = [](usize, s32, f32) {};
    }
    static void captureAction(PlayerCombatState& state, const PlayerAnimator& animator) {
        state.rollback = animator.checkpoint();
        const auto& cursor = animator.player();
        state.animation = {static_cast<u32>(animator.action()), cursor.sequence(),
                           cursor.generation(), cursor.presentationFrame(), cursor.transition()};
    }
    CombatSnapshot capture(u64 tick) const {
        auto host = authority;
        host.motion.tick = tick;
        const auto& runtime = native.front();
        host.motion.players[1]->position = runtime.actor.position();
        host.motion.players[1]->yaw = std::remainder(runtime.actor.yaw(), kTwoPi);
        auto& player = *host.players[1];
        captureAction(player, runtime.figure->animator());
        player.knockback = runtime.knockback.checkpoint();
        player.reaction = runtime.reaction;
        player.knockbackTicks = PlayerCombatState::kKnockbackTicks;
        return host;
    }
    void begin() {
        authority = capture(authority.motion.tick);
        prediction.begin(context, 2, floor);
        REQUIRE(prediction.setPlayer(1, *native.front().figure->actionTree()));
        REQUIRE(prediction.setPlayer(2, *native.front().figure->actionTree()));
        accept(authority);
    }
    void accept(const CombatSnapshot& host, std::span<const InputCommand> input = {}) {
        const auto encoded = CombatPacket::encode(host);
        REQUIRE(encoded);
        const auto decoded = CombatPacket::decode(*encoded);
        REQUIRE(decoded);
        authority = *decoded;
        prediction.update(&authority, input);
    }
    InputCommand command(u64 tick, u8 seat = 1) const {
        InputCommand input;
        input.epoch = context.epoch;
        input.tick = tick;
        input.seat = seat;
        input.grant = context.grants[seat];
        return input;
    }
    void step(const InputCommand& input) {
        std::array<PlayInput, InputCommand::kSeats> controls;
        controls[1] = SessionInputs::playInput(input);
        PartyMotion::step(native, controls, false, 0, 1, 1.0f / 60, floor, events);
    }
    CombatSnapshot shown(f32 blend = 1) const {
        auto displayed = authority;
        prediction.present(displayed, blend);
        REQUIRE(displayed.valid());
        return displayed;
    }
};

TEST_CASE("confirmed queued reactions and partial decay rewind against native party motion",
          "[netplay][rollback][knockback-prediction][assets]") {
    const auto reaction = GENERATE(PlayerDeed::Flinch, PlayerDeed::Spike, PlayerDeed::Reel);
    const bool consumed = GENERATE(false, true);
    const s32 renders = GENERATE(0, 1, 4);
    KnockbackFixture fixture;
    auto& runtime = fixture.native.front();
    runtime.reaction = reaction;
    runtime.knockback.queue({1, 0, 0}, Knockback::kKnockBack, 10);
    if (consumed) {
        fixture.step(fixture.command(10));
        REQUIRE(runtime.reaction == PlayerDeed::None);
        REQUIRE(runtime.figure->animator().reacting());
        REQUIRE(runtime.knockback.checkpoint().frameSeconds > 0);
    }
    fixture.begin();
    const auto initial = fixture.authority;
    std::vector<CombatSnapshot> history{initial};
    bool moved = false;
    bool recovered = false;
    for (u64 tick = 11; tick <= 95; ++tick) {
        auto input = fixture.command(tick);
        // Native reactions ignore both movement and manual aiming. Only supply
        // this intent on the first step so no post-recovery command is guessed.
        if (tick == 11) {
            input.direction = {0, 1};
            input.magnitude = 1;
            input.aimDirection = Vec3{-1, 0, 0};
            input.heldButtons = static_cast<u32>(CommandHeld::Attack);
        }
        fixture.step(input);
        auto checkpoint = fixture.capture(tick);
        checkpoint.players[1]->rollbackInput = input;
        history.push_back(checkpoint);
        const usize eventsBeforeReplay = fixture.performed;
        if (tick >= 16 && tick % 4 == 0) {
            fixture.accept(history[static_cast<usize>(tick - 4 - 10)], std::array{input});
        } else {
            fixture.prediction.update(&fixture.authority, std::array{input});
        }
        CAPTURE(reaction, consumed, renders, tick);
        const auto displayed = fixture.shown();
        REQUIRE(displayed.players[1]->rollback);
        CHECK(*displayed.players[1]->rollback == runtime.figure->animator().checkpoint());
        REQUIRE(displayed.players[1]->knockback);
        CHECK(*displayed.players[1]->knockback == runtime.knockback.checkpoint());
        CHECK(displayed.players[1]->reaction == runtime.reaction);
        CHECK(displayed.motion.players[1]->position == runtime.actor.position());
        CHECK(displayed.motion.players[1]->yaw == Approx(runtime.actor.yaw()).margin(1e-6f));
        CHECK(displayed.players[1]->health == 100);
        CHECK(displayed.enemies.front().health == 100);
        CHECK(displayed.projectiles.empty());
        REQUIRE(fixture.prediction.replayed(1));
        CHECK(fixture.prediction.replayed(1)->releases.empty());
        CHECK(fixture.performed == eventsBeforeReplay);
        for (s32 render = 0; render < renders; ++render) {
            const auto frame =
                fixture.shown(static_cast<f32>(render + 1) / static_cast<f32>(renders));
            CHECK(frame.players[1]->health == 100);
            CHECK(frame.projectiles.empty());
        }
        moved = moved || runtime.actor.position() != initial.motion.players[1]->position;
        recovered = recovered || !runtime.figure->animator().reacting();
    }
    CHECK(moved);
    CHECK(recovered);
    CHECK_FALSE(runtime.knockback.sliding());
}

TEST_CASE("a newer confirmed impulse replaces the old replay without moving another local seat",
          "[netplay][rollback][knockback-prediction][assets]") {
    KnockbackFixture fixture;
    auto& runtime = fixture.native.front();
    runtime.reaction = PlayerDeed::Flinch;
    runtime.knockback.queue({1, 0, 0}, Knockback::kKnockBack, 10);
    fixture.begin();
    for (u64 tick = 11; tick <= 14; ++tick) {
        const auto input = fixture.command(tick);
        fixture.step(input);
        fixture.prediction.update(&fixture.authority, std::array{input});
    }
    const auto previous = fixture.authority;
    runtime.knockback.queue({0, 0, -1}, Knockback::kKnockBack, 10);
    auto host = fixture.capture(14);
    host.players[1]->health = 91;
    fixture.accept(host);
    const auto other = fixture.shown().motion.players[2]->position;
    for (u64 tick = 15; tick <= 22; ++tick) {
        const auto input = fixture.command(tick);
        fixture.step(input);
        fixture.prediction.update(&fixture.authority, std::array{input, fixture.command(tick, 2)});
        const auto shown = fixture.shown();
        REQUIRE(shown.players[1]->rollback);
        CHECK(*shown.players[1]->rollback == runtime.figure->animator().checkpoint());
        REQUIRE(shown.players[1]->knockback);
        CHECK(*shown.players[1]->knockback == runtime.knockback.checkpoint());
        CHECK(shown.motion.players[1]->position == runtime.actor.position());
        CHECK(shown.motion.players[2]->position == other);
        CHECK(shown.players[1]->health == 91);
        CHECK(shown.players[2]->health == 100);
        CHECK(shown.enemies.front().health == 100);
        CHECK(shown.projectiles.empty());
    }
    const auto corrected = fixture.shown();
    fixture.prediction.update(&previous, {});
    CHECK(fixture.shown().players[1]->rollback == corrected.players[1]->rollback);
    CHECK(fixture.shown().players[1]->health == 91);
}

TEST_CASE("reaction prediction does not exceed the certified corridor or invent a recovery attack",
          "[netplay][rollback][knockback-prediction][assets]") {
    enum class Limit : u8 { Prefix, Movement, Attack, Defend, Shove, Fall };
    const auto limit = GENERATE(Limit::Prefix, Limit::Movement, Limit::Attack, Limit::Defend,
                                Limit::Shove, Limit::Fall);
    KnockbackFixture fixture;
    auto& runtime = fixture.native.front();
    if (limit == Limit::Shove) {
        runtime.knockback.shove({1, 0, 0}, 1.0f / 60);
    } else if (limit == Limit::Fall) {
        runtime.knockback.queue({1, 0, 0}, Knockback::kKnockDown, 10);
    } else {
        runtime.knockback.addVelocity({1, 0, 0});
    }
    fixture.begin();
    auto input = fixture.command(11);
    if (limit == Limit::Prefix) {
        input.tick = fixture.authority.motion.tick + PlayerCombatState::kKnockbackTicks + 1;
    } else if (limit == Limit::Movement) {
        input.direction = {0, 1};
        input.magnitude = 1;
    } else if (limit == Limit::Attack) {
        input.heldButtons = static_cast<u32>(CommandHeld::Attack);
    } else if (limit == Limit::Defend) {
        input.pressedButtons = static_cast<u32>(CommandPress::Defend);
    }
    const auto checkpoint = CombatPacket::encode(fixture.authority);
    REQUIRE(checkpoint);
    fixture.prediction.update(&fixture.authority, std::array{input});
    CHECK(fixture.prediction.replayed(1) == nullptr);
    const auto shown = fixture.shown();
    CHECK(shown.motion.players[1]->position == fixture.authority.motion.players[1]->position);
    CHECK(shown.players[1]->rollback == fixture.authority.players[1]->rollback);
    CHECK(shown.players[1]->health == 100);
    CHECK(shown.projectiles.empty());
    CHECK(CombatPacket::encode(fixture.authority) == checkpoint);
}
} // namespace
