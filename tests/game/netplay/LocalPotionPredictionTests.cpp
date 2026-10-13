#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LocalPotionPrediction.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive weapons;
    ProjectileResources resources;
    WorldCollision floor;
    TreeInfo tree;
    MatchContext context;
    CombatSnapshot state;
    LocalMovementPrediction movement;
    LocalPotionPrediction bottles;
    u64 tick = 10;

    Fixture() {
        REQUIRE(weapons.load(test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path()));
        for (s32 kind = 1; kind <= 4; ++kind) {
            REQUIRE(resources.addTree(static_cast<u32>(100 + kind), weapons,
                                      PotionEmission::look(kind).bottle, device));
        }
        floor.build(std::vector<CollisionTriangle>{
            CollisionTriangle{
                {0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{100, 0, -100}}, 0},
            CollisionTriangle{
                {0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0}});
        tree.name = "WAR";
        TreeNodeInfo node;
        node.name = "ROOT";
        node.type = 1;
        tree.nodes.push_back(node);
        for (const auto name : PlayerAnimator::kSequenceNames) {
            TreeSequenceInfo sequence;
            sequence.name = name;
            sequence.frames = 4;
            sequence.frameRate = 30;
            sequence.trackOfNode = {-1};
            tree.sequences.push_back(sequence);
        }
        context.epoch = 7;
        context.scene = 1;
        context.owners = {1, 2, 0, 0};
        context.grants = {1, 5, 0, 0};
        state.motion.epoch = context.epoch;
        state.motion.tick = tick;
        state.motion.cameraContinuity = 1;
        state.motion.players[0] = SeatMotion{1, 1, {-50, 0, 0}, 0};
        state.players[0].emplace().health = 100;
        state.motion.players[1] = SeatMotion{5, 1, {}, 0};
        auto& player = state.players[1].emplace();
        player.health = 100;
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        player.rollback = animator.checkpoint();
        player.animation = {static_cast<u32>(animator.action()), animator.player().sequence(),
                            animator.player().generation(), animator.player().presentationFrame(),
                            animator.player().transition()};
        player.movement.enabled = player.movement.current = true;
        player.floor = PlayerFloorState{0, 4, std::nullopt};
        player.locomotion.emplace();
        player.locomotion->radius = 8;
        player.locomotion->ticks = 12;
        player.potion.emplace();
        player.potion->potions = {1, 3};
        player.potion->ticks = 12;
        player.potion->textureFrame = 24.0f;
        REQUIRE(state.valid());
        movement.begin(context, 2, floor);
        REQUIRE(movement.setPlayer(1, tree));
        bottles.begin(context, 2, floor, weapons, resources);
    }

    void step(bool held) {
        InputCommand input;
        input.epoch = context.epoch;
        input.grant = context.grants[1];
        input.seat = 1;
        input.tick = ++tick;
        input.heldButtons = held ? static_cast<u32>(CommandHeld::ThrowPotion) : 0;
        REQUIRE(input.valid());
        movement.update(&state, std::array{input});
        REQUIRE(movement.replayed(1));
        bottles.update(&state, movement, std::array{input});
    }

    CombatSnapshot shown() {
        auto shown = state;
        movement.present(shown, 1);
        bottles.present(shown, 1);
        REQUIRE(shown.valid());
        return shown;
    }

    ProjectileState confirmedBottle() const {
        PlayerMissiles native;
        REQUIRE(
            native.launch(PotionEmission::throwPotion({1, {0, 2, 0}, {0, 0, 1}, 3, 8, 1, 0}, 3)));
        for (s32 i = 0; i < 4; ++i) {
            native.update(PotionSimulation::kStep, nullptr);
        }
        REQUIRE(native.count() == 1);
        const auto& missile = native.missile(0);
        ProjectileState shot;
        shot.source = ProjectileSource::PlayerEffect;
        shot.instance = 51;
        shot.resource = 103;
        shot.placement = PlayerMissiles::transformOf(missile);
        shot.direction = missile.velocity;
        shot.age = missile.age;
        shot.potion =
            ProjectileState::PotionRollback{{context.epoch, state.motion.tick - 1, 7,
                                             context.grants[1], 1, PotionIdentity::Kind::Throw},
                                            3,
                                            PlayerMissiles::flightCheckpoint(missile),
                                            false};
        REQUIRE(native.visualHistory().size() == 1);
        shot.particles.emplace();
        shot.particles->potion = 3;
        shot.particles->potionOrigin = shot.potion->origin;
        shot.particles->flight = native.visualHistory().front().flight;
        REQUIRE(resources.sampleWeapon(shot, missile.age, 24));
        // A confirmed emitter's native generation is not a fresh replay cursor.
        shot.animation.generation = 19;
        REQUIRE(resources.accepts(shot));
        return shot;
    }
};

TEST_CASE("native potion bottle is shown at its local release without spending live inventory",
          "[game][local-potions][assets]") {
    Fixture f;
    const auto inventory = f.state.players[1]->potion->potions;
    std::optional<ProjectileState> previous;
    for (s32 i = 0; i < 11; ++i) {
        f.step(i < 3);
        const auto shown = f.shown();
        if (!shown.projectiles.empty()) {
            REQUIRE(shown.projectiles.size() == 1);
            const auto& shot = shown.projectiles.front();
            REQUIRE(shot.potion);
            CHECK(shot.potion->kind == 3);
            CHECK(shot.potion->origin.kind == PotionIdentity::Kind::Throw);
            CHECK(shot.potion->origin.seat == 1);
            CHECK(shot.potion->origin.tick > f.state.motion.tick);
            CHECK(shot.resource == 103);
            CHECK_FALSE(shot.rollback);
            CHECK_FALSE(shot.effectOrigin);
            REQUIRE(shot.particles);
            CHECK(shot.particles->potion == 3);
            CHECK(shot.particles->potionOrigin == shot.potion->origin);
            ReplicaProjectileParticles particles;
            REQUIRE(f.resources.prepareParticles(shot, particles));
            REQUIRE(f.resources.accepts(shot));
            if (previous) {
                CHECK(shot.instance == previous->instance);
                CHECK(shot.potion->origin == previous->potion->origin);
            }
            previous = shot;
            auto repeated = f.shown();
            CHECK(repeated.projectiles.front().placement == shot.placement);
        }
        CHECK(f.state.players[1]->potion->potions == inventory);
    }
    REQUIRE(previous);
    CHECK(previous->age > 0);
}

TEST_CASE("confirmed potion replaces speculation once and absent host bottles stay absent",
          "[game][local-potions][assets]") {
    Fixture f;
    for (s32 i = 0; i < 9; ++i) {
        f.step(i < 3);
    }
    auto displayed = f.shown();
    REQUIRE(displayed.projectiles.size() == 1);
    auto confirmed = displayed.projectiles.front();
    confirmed.instance = 51;
    f.state.projectiles = {confirmed};
    f.state.motion.tick = f.tick;
    REQUIRE(f.state.valid());
    f.movement.update(&f.state, {});
    f.bottles.update(&f.state, f.movement);
    f.bottles.present(displayed, 1);
    REQUIRE(displayed.projectiles.size() == 1);
    CHECK(displayed.projectiles.front().instance == 51);
    CHECK(displayed.projectiles.front().potion->origin == confirmed.potion->origin);

    ++f.state.motion.tick;
    f.state.projectiles.clear();
    f.movement.update(&f.state, {});
    f.bottles.update(&f.state, f.movement);
    displayed.projectiles = {confirmed}; // delayed render buffer still has the retired bottle
    f.bottles.present(displayed, 1);
    CHECK(displayed.projectiles.empty());
    f.bottles.present(displayed, 1);
    CHECK(displayed.projectiles.empty());
}

TEST_CASE("potion visual replay fails closed on missing clocks without hiding others",
          "[game][local-potions][assets]") {
    Fixture f;
    f.state.players[1]->potion->textureFrame.reset();
    for (s32 i = 0; i < 9; ++i) {
        f.step(i < 3);
    }
    CHECK(f.bottles.count() == 0);
    CHECK(f.shown().projectiles.empty());
    f.bottles.clear();
    auto shown = f.state;
    ProjectileState unrelated;
    unrelated.source = ProjectileSource::PlayerEffect;
    unrelated.instance = 55;
    unrelated.resource = 103;
    shown.projectiles = {unrelated};
    f.bottles.present(shown, 1);
    REQUIRE(shown.projectiles.size() == 1);
    CHECK(shown.projectiles.front().instance == 55);
}

TEST_CASE("held and zero-step potion confirmations preserve the complete authoritative pose",
          "[game][local-potions][assets]") {
    Fixture f;
    const bool held = GENERATE(false, true);
    auto source = f.confirmedBottle();
    source.potion->held = held;
    source.particles->held = held;
    f.state.projectiles = {source};
    const auto expected = CombatPacket::encode(f.state);
    REQUIRE(expected);
    std::vector<InputCommand> commands;
    if (held) {
        InputCommand input;
        input.epoch = f.context.epoch;
        input.grant = f.context.grants[1];
        input.seat = 1;
        input.tick = f.state.motion.tick + 2;
        REQUIRE(input.valid());
        commands.push_back(input);
    }
    f.bottles.update(&f.state, f.movement, commands);
    REQUIRE(f.bottles.count() == 1);
    auto shown = f.state;
    f.bottles.present(shown, 1);
    CHECK(CombatPacket::encode(shown) == expected);
    f.bottles.update(nullptr, f.movement);
    f.bottles.present(shown, 1);
    CHECK(CombatPacket::encode(shown) == expected);
}

TEST_CASE("potion confirmations reject invalid native resource poses before resampling",
          "[game][local-potions][assets]") {
    Fixture f;
    f.state.projectiles = {f.confirmedBottle()};
    f.bottles.update(&f.state, f.movement);
    REQUIRE(f.bottles.count() == 1);
    auto delayed = f.state;
    ++f.state.motion.tick;
    auto& malformed = f.state.projectiles.front();
    if (GENERATE(false, true)) {
        malformed.animation.sequence = 65535;
    } else {
        malformed.animation.frame = 1'000'000;
    }
    REQUIRE(f.state.valid());
    REQUIRE_FALSE(f.resources.accepts(malformed));
    InputCommand input;
    input.epoch = f.context.epoch;
    input.grant = f.context.grants[1];
    input.seat = 1;
    input.tick = f.state.motion.tick + 1;
    REQUIRE(input.valid());
    f.bottles.update(&f.state, f.movement, std::array{input});
    CHECK(f.bottles.count() == 0);
    const auto before = CombatPacket::encode(delayed);
    REQUIRE(before);
    f.bottles.present(delayed, 1);
    CHECK(CombatPacket::encode(delayed) == before);
}

TEST_CASE("potion prediction clears prior identities when its grant or epoch is reset",
          "[game][local-potions][assets]") {
    Fixture f;
    const bool newEpoch = GENERATE(false, true);
    for (s32 i = 0; i < 9; ++i) {
        f.step(i < 3);
    }
    REQUIRE(f.bottles.count() == 1);
    const auto oldContext = f.context;
    const auto oldAuthority = f.state;
    if (newEpoch) {
        ++f.context.epoch;
    } else {
        ++f.context.grants[1];
    }
    f.state.motion.epoch = f.context.epoch;
    f.state.motion.tick = ++f.tick;
    f.state.motion.players[1]->grant = f.context.grants[1];
    REQUIRE(f.state.valid());
    f.movement.begin(f.context, 2, f.floor);
    REQUIRE(f.movement.setPlayer(1, f.tree));
    f.bottles.begin(f.context, 2, f.floor, f.weapons, f.resources);
    CHECK(f.bottles.count() == 0);
    f.movement.update(&f.state, {});
    f.bottles.update(&f.state, f.movement);
    InputCommand previousOwner;
    previousOwner.epoch = oldContext.epoch;
    previousOwner.grant = oldContext.grants[1];
    previousOwner.seat = 1;
    previousOwner.tick = ++f.tick;
    previousOwner.heldButtons = static_cast<u32>(CommandHeld::ThrowPotion);
    REQUIRE(previousOwner.valid());
    f.movement.update(&f.state, std::array{previousOwner});
    f.bottles.update(&oldAuthority, f.movement, std::array{previousOwner});
    CHECK(f.bottles.count() == 0);
    auto shown = f.state;
    f.bottles.present(shown, 1);
    CHECK(shown.projectiles.empty());

    // A new valid owner/session can still create a fresh, separately identified bottle.
    for (s32 i = 0; i < 9; ++i) {
        f.step(i < 3);
    }
    const auto fresh = f.shown();
    REQUIRE(fresh.projectiles.size() == 1);
    REQUIRE(fresh.projectiles.front().potion);
    CHECK(fresh.projectiles.front().potion->origin.epoch == f.context.epoch);
    CHECK(fresh.projectiles.front().potion->origin.grant == f.context.grants[1]);
}

TEST_CASE("rejected or stale potion releases withdraw without inventing host bottles",
          "[game][local-potions][assets]") {
    Fixture f;
    for (s32 i = 0; i < 9; ++i) {
        f.step(i < 3);
    }
    auto delayed = f.shown();
    REQUIRE(delayed.projectiles.size() == 1);
    REQUIRE(f.bottles.count() == 1);
    SECTION("the host resolves the release deadline without executing the input") {
        f.state.motion.tick = f.tick;
        f.state.players[1]->movement.receivedInput = 0;
        REQUIRE(f.state.projectiles.empty());
        f.movement.update(&f.state, {});
        f.bottles.update(&f.state, f.movement);
    }
    SECTION("no new authoritative checkpoint arrives before the stale bound") {
        for (u64 i = 0; i <= LocalMovementPrediction::kStaleTicks; ++i) {
            f.bottles.update(nullptr, f.movement);
        }
    }
    CHECK(f.bottles.count() == 0);
    f.bottles.present(delayed, 1);
    CHECK(delayed.projectiles.empty());
    f.bottles.present(delayed, 1);
    CHECK(delayed.projectiles.empty());
    CHECK(f.state.players[1]->potion->potions == std::vector<s32>{1, 3});
}
} // namespace
