#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/DamageTypes.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PotionSimulation.h"
#include "game/screens/SessionInputs.h"
#include "game/world/PlayerFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Simulation = PotionSimulation;
constexpr Simulation::Binding kOwner{7, 3, 1};

TreeInfo potionTree() {
    TreeInfo tree;
    tree.name = "POTION_TEST";
    TreeNodeInfo node;
    node.name = "ROOT";
    node.type = 1;
    tree.nodes.push_back(node);
    for (const auto name : PlayerAnimator::kSequenceNames) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 11;
        sequence.frameRate = 30;
        TrackInfo track;
        track.flags = TrackInfo::channelBit(3);
        track.frames = {0, 10};
        const f32 start = static_cast<f32>(tree.sequences.size()) * 0.01f;
        track.values = {start, start + 0.4f};
        sequence.tracks.push_back(track);
        sequence.trackOfNode = {0};
        tree.sequences.push_back(sequence);
    }
    return tree;
}

Simulation::Context context() {
    Simulation::Context result;
    result.powers.fill(20);
    result.powers[2] = 20 * damage::kOwnColourBonus;
    result.bonuses[2] = damage::kOwnColourBonus;
    result.damageFlags = damage::kHeal;
    result.nextKind = 4;
    return result;
}

Simulation::Checkpoint capture(const PlayerAnimator& animator,
                               std::vector<s32> potions = {1, 2, 3, 4}, u64 tick = 10) {
    return {tick, animator.checkpoint(), animator.transitionPose(), std::move(potions), context()};
}

void sameAnimator(const PlayerAnimator& actual, const PlayerAnimator& expected) {
    REQUIRE(actual.checkpoint() == expected.checkpoint());
    CHECK(actual.transitionPose() == expected.transitionPose());
    REQUIRE(actual.pose().poses().size() == expected.pose().poses().size());
    for (usize i = 0; i < actual.pose().poses().size(); ++i) {
        CHECK(actual.pose().poses()[i] == expected.pose().poses()[i]);
        CHECK(actual.pose().matrices()[i] == expected.pose().matrices()[i]);
    }
}

void reachBlend(PlayerAnimator& animator) {
    for (s32 tick = 0; tick < 8; ++tick) {
        animator.update(PlayerMotion::Run, 1, Simulation::kStep);
    }
    // Starting a run or potion has no blend. The native return to Ready from
    // locomotion does; wait for that real boundary rather than manufacturing one.
    for (s32 tick = 0; tick < 120 && !animator.player().transitioning(); ++tick) {
        animator.update(PlayerMotion::Stand, 1, Simulation::kStep);
    }
    REQUIRE(animator.player().transitioning());
}

void checkRelease(const Simulation::Release& release, const PlayerAnimator& native,
                  const Simulation::Frame& frame, s32 stored, s32 resolved,
                  const Simulation::Context& power) {
    CHECK(release.origin.owner == kOwner);
    CHECK(release.origin.tick == frame.tick);
    CHECK(release.origin.generation == native.player().generation());
    CHECK(release.origin.kind ==
          (native.potionThrown() ? Simulation::Kind::Throw : Simulation::Kind::Use));
    CHECK(release.origin.valid());
    CHECK(release.storedKind == stored);
    CHECK(release.context.resolvedKind == resolved);
    CHECK(release.context.power == power.powers[static_cast<usize>(stored)]);
    CHECK(release.context.colourBonus == power.bonuses[static_cast<usize>(stored)]);
    CHECK(release.context.position == frame.position);
    CHECK(release.context.facing == frame.facing);
    if (native.potionThrown()) {
        REQUIRE(std::holds_alternative<MissileLaunch>(release.emission));
        const auto& launch = std::get<MissileLaunch>(release.emission);
        CHECK(launch.owner == kOwner.seat);
        CHECK(launch.position == frame.position + frame.facing * 2.0f + Vec3{0, 4, 0});
        CHECK(launch.direction == frame.facing);
        REQUIRE(launch.velocity);
        const f32 speed = 5 + 1.5f * static_cast<f32>(native.potionThrowTicks());
        const Vec3 velocity = (frame.facing + Vec3{0, 1, 0}) * (0.707f * speed);
        CHECK(launch.velocity->x == Approx(velocity.x));
        CHECK(launch.velocity->y == Approx(velocity.y));
        CHECK(launch.velocity->z == Approx(velocity.z));
        CHECK(launch.potency == 0.75f * release.context.power);
        CHECK(launch.damage == 40 * release.context.colourBonus);
        CHECK(launch.flags == damage::kHeal);
        CHECK(launch.model == nullptr);
        CHECK(launch.archive == nullptr);
        CHECK_FALSE(launch.release);
    } else {
        REQUIRE(std::holds_alternative<PotionEmission::Burst>(release.emission));
        const auto& burst = std::get<PotionEmission::Burst>(release.emission);
        CHECK(burst.impact.owner == kOwner.seat);
        CHECK(burst.impact.position == frame.position);
        CHECK(burst.impact.potion == resolved);
        CHECK(burst.impact.potency == release.context.power);
        CHECK(burst.impact.damage == 40 * release.context.colourBonus);
        CHECK(burst.impact.flags == damage::kHeal);
    }
}

void compareTimeline(const TreeInfo& tree, s32 character, bool thrown, bool accelerated) {
    PlayerAnimator native;
    REQUIRE(native.bind(tree, false));
    native.setCharacter(character);
    auto baseline = capture(native, {1, 0, 2});
    baseline.context.rapid = accelerated;
    baseline.context.speed = accelerated;
    Simulation simulation;
    REQUIRE(simulation.restore(tree, kOwner, baseline));
    auto stack = baseline.potions;
    auto nextKind = *baseline.context.nextKind;
    usize released = 0;
    constexpr std::array<s32, 3> renders{0, 1, 4};
    for (u64 step = 1; step <= Simulation::kMaxTicks; ++step) {
        Simulation::Frame frame;
        frame.tick = baseline.tick + step;
        frame.motion = step < 80 ? PlayerMotion::Run : PlayerMotion::Stand;
        frame.strafe = step < 35 ? StrafeWay::Left : StrafeWay::None;
        // The first release of the button latches charge; a second hold cannot
        // resume charging that same wind-up. Later presses can start a new cast.
        const bool held = (step <= 5 || (step >= 9 && step <= 12) || step == 65);
        if (held && !stack.empty()) {
            frame.deed = thrown ? PlayerDeed::ThrowPotion : PlayerDeed::UsePotion;
        }
        frame.position = {static_cast<f32>(step) * 0.01f, 2, 3};
        frame.facing = step < 40 ? Vec3{0.6f, 0, 0.8f} : Vec3{-1, 0, 0};
        native.setStrafe(frame.strafe);
        native.setAttackSpeed(baseline.context.rapid, baseline.context.speed);
        native.update(frame.motion, 1, Simulation::kStep, frame.deed);
        CAPTURE(character, thrown, accelerated, step, native.action());
        REQUIRE(simulation.step(frame));
        sameAnimator(simulation.animator(), native);
        if ((native.potionUsed() || native.potionThrown()) && !stack.empty()) {
            const s32 stored = stack.back();
            stack.pop_back();
            const s32 resolved = stored == 0 ? nextKind : stored;
            if (stored == 0) {
                nextKind = static_cast<u8>(nextKind % 4 + 1);
            }
            ++released;
            REQUIRE(simulation.releases().size() == released);
            checkRelease(simulation.releases().back(), native, frame, stored, resolved,
                         baseline.context);
        }
        CHECK(simulation.checkpoint().potions == stack);
        CHECK(simulation.checkpoint().context.nextKind == nextKind);
        CHECK(simulation.remaining() == Simulation::kMaxTicks - step);
        const auto beforeDraw = simulation.checkpoint();
        for (s32 draw = 0; draw < renders[static_cast<usize>(step % renders.size())]; ++draw) {
            TreePose pose;
            simulation.animator().evaluatePresentation(pose, 0.25f * draw);
        }
        CHECK(simulation.checkpoint() == beforeDraw);
    }
    REQUIRE(released >= 1);
    const auto final = simulation.checkpoint();
    CHECK_FALSE(simulation.step({final.tick + 1}));
    CHECK(simulation.checkpoint() == final);
}

TEST_CASE("private potion replay matches native held charge and exact changing poses",
          "[potion-simulation][rollback]") {
    const auto tree = potionTree();
    const bool thrown = GENERATE(false, true);
    const bool accelerated = GENERATE(false, true);
    compareTimeline(tree, 0, thrown, accelerated);
}

TEST_CASE("all native classes replay potion casts without mutating live resources or inventory",
          "[potion-simulation][rollback][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const bool thrown = GENERATE(false, true);
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    CharacterSave save;
    save.character = character;
    save.progress().inventory.potions = {1, 2, 3, 4};
    save.progress().health = 145;
    const auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    REQUIRE(figure->actionTree());
    const auto before = figure->animator().checkpoint();
    const auto textures = device.texturesCreated;
    const auto uploads = device.textureUpdates;
    compareTimeline(*figure->actionTree(), character, thrown, false);
    CHECK(figure->animator().checkpoint() == before);
    CHECK(save.progress().inventory.potions == std::vector<s32>{1, 2, 3, 4});
    CHECK(save.progress().health == 145);
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == uploads);
}

TEST_CASE("potion replay restores interrupted blends and stable release identity after correction",
          "[potion-simulation][rollback]") {
    const auto tree = potionTree();
    PlayerAnimator native;
    REQUIRE(native.bind(tree, false));
    reachBlend(native);
    auto baseline = capture(native, {4});
    REQUIRE(baseline.transitionPose);
    Simulation simulation;
    REQUIRE(simulation.restore(tree, kOwner, baseline));
    sameAnimator(simulation.animator(), native);
    Simulation::Identity first;
    Simulation::Checkpoint afterRelease;
    for (s32 pass = 0; pass < 2; ++pass) {
        REQUIRE(simulation.restore(tree, kOwner, baseline));
        REQUIRE(simulation.releases().empty());
        for (u64 tick = baseline.tick + 1; tick < baseline.tick + 80; ++tick) {
            const auto deed =
                tick == baseline.tick + 1 ? PlayerDeed::ThrowPotion : PlayerDeed::None;
            REQUIRE(simulation.step({tick, PlayerMotion::Stand, StrafeWay::None, deed}));
            if (!simulation.releases().empty()) {
                REQUIRE(simulation.releases().size() == 1);
                if (pass == 0) {
                    first = simulation.releases().front().origin;
                    afterRelease = simulation.checkpoint();
                } else {
                    CHECK(simulation.releases().front().origin == first);
                    CHECK(simulation.checkpoint() == afterRelease);
                }
                break;
            }
        }
        REQUIRE(first.valid());
    }
    REQUIRE(simulation.restore(tree, kOwner, afterRelease));
    REQUIRE(simulation.step({afterRelease.tick + 1}));
    CHECK(simulation.releases().empty()); // baseline's release latch is not a new event
    CHECK_FALSE(simulation.hasPotion());
}

TEST_CASE("only authoritative reactions can cancel an unspent potion windup",
          "[potion-simulation][rollback]") {
    const PlayerDeed reaction =
        GENERATE(PlayerDeed::Flinch, PlayerDeed::Reel, PlayerDeed::Spike, PlayerDeed::Die);
    const auto tree = potionTree();
    PlayerAnimator native;
    REQUIRE(native.bind(tree, false));
    native.update(PlayerMotion::Stand, 1, Simulation::kStep, PlayerDeed::ThrowPotion);
    auto baseline = capture(native, {0});
    baseline.context.nextKind.reset();
    Simulation simulation;
    REQUIRE(simulation.restore(tree, kOwner, baseline));
    const Simulation::Frame hurt{baseline.tick + 1, PlayerMotion::Stand, StrafeWay::None, reaction};
    CHECK_FALSE(simulation.step(hurt)); // no new speculative injury
    CHECK(simulation.checkpoint() == baseline);
    baseline.reaction = reaction;
    REQUIRE(simulation.restore(tree, kOwner, baseline));
    REQUIRE(simulation.step(hurt)); // known reaction can cancel unknown-colour windup
    native.update(PlayerMotion::Stand, 1, Simulation::kStep, reaction);
    sameAnimator(simulation.animator(), native);
    for (u64 tick = baseline.tick + 2; tick <= baseline.tick + 70; ++tick) {
        native.update(PlayerMotion::Stand, 1, Simulation::kStep);
        REQUIRE(simulation.step({tick}));
        sameAnimator(simulation.animator(), native);
    }
    CHECK(simulation.releases().empty());
    CHECK(simulation.checkpoint().potions == std::vector<s32>{0});
}

TEST_CASE("unknown shared potion colour fails atomically while ordinary motion remains usable",
          "[potion-simulation][rollback]") {
    const auto tree = potionTree();
    PlayerAnimator native;
    REQUIRE(native.bind(tree, false));
    auto baseline = capture(native, {0});
    baseline.context.nextKind.reset();
    Simulation simulation;
    REQUIRE(simulation.restore(tree, kOwner, baseline));
    REQUIRE(simulation.step({11, PlayerMotion::Run}));
    const auto before = simulation.checkpoint();
    CHECK_FALSE(simulation.step({12, PlayerMotion::Stand, StrafeWay::None, PlayerDeed::UsePotion}));
    CHECK(simulation.checkpoint() == before);
    baseline = before;
    baseline.context.nextKind = 2;
    REQUIRE(simulation.restore(tree, kOwner, baseline));
    REQUIRE(simulation.step({12, PlayerMotion::Stand, StrafeWay::None, PlayerDeed::UsePotion}));
    for (u64 tick = 13; simulation.releases().empty() && tick < 70; ++tick) {
        REQUIRE(simulation.step({tick}));
    }
    REQUIRE(simulation.releases().size() == 1);
    CHECK(simulation.releases().front().context.resolvedKind == 2);
    CHECK(simulation.releases().front().context.colourBonus == 1); // stored kind0, not resolved2
    CHECK(simulation.releases().front().context.power == 20);
    CHECK(simulation.checkpoint().context.nextKind == 3);
}

TEST_CASE("potion identities isolate seats grants epochs and clear observations on replacement",
          "[potion-simulation][rollback]") {
    const auto tree = potionTree();
    PlayerAnimator native;
    REQUIRE(native.bind(tree, false));
    const auto baseline = capture(native, {1});
    const std::array<Simulation::Binding, 4> owners{kOwner, Simulation::Binding{7, 3, 2},
                                                    Simulation::Binding{7, 4, 1},
                                                    Simulation::Binding{8, 3, 1}};
    std::vector<Simulation::Identity> identities;
    Simulation simulation;
    for (const auto& owner : owners) {
        REQUIRE(simulation.restore(tree, owner, baseline));
        REQUIRE(simulation.releases().empty());
        REQUIRE(simulation.step({11, PlayerMotion::Stand, StrafeWay::None, PlayerDeed::UsePotion}));
        for (u64 tick = 12; simulation.releases().empty() && tick < 70; ++tick) {
            REQUIRE(simulation.step({tick}));
        }
        REQUIRE(simulation.releases().size() == 1);
        const auto& identity = simulation.releases().front().origin;
        for (const auto& prior : identities) {
            CHECK(identity != prior);
            CHECK(identity.tick == prior.tick);
            CHECK(identity.generation == prior.generation);
        }
        identities.push_back(identity);
    }
    simulation.clear();
    CHECK_FALSE(simulation.hasPotion());
    CHECK(simulation.releases().empty());
    CHECK(simulation.remaining() == 0);
    CHECK_FALSE(simulation.step({1}));
}

TEST_CASE("potion checkpoint validation rejects malformed state without replacing the baseline",
          "[potion-simulation][rollback]") {
    const auto tree = potionTree();
    PlayerAnimator native;
    REQUIRE(native.bind(tree, false));
    const auto baseline = capture(native);
    Simulation simulation;
    REQUIRE(simulation.restore(tree, kOwner, baseline));
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    std::vector<Simulation::Checkpoint> invalid;
    auto add = [&](const auto& alter) {
        auto state = baseline;
        alter(state);
        invalid.push_back(std::move(state));
    };
    add([](auto& state) { state.potions.resize(1'000'000, 1); });
    add([](auto& state) { state.potions = {-1}; });
    add([](auto& state) { state.potions = {5}; });
    add([](auto& state) { state.remaining = Simulation::kMaxTicks + 1; });
    add([](auto& state) { state.context.nextKind = 0; });
    add([](auto& state) { state.context.nextKind = 5; });
    add([](auto& state) { state.context.damageFlags = ~damage::kHeal; });
    add([&](auto& state) { state.context.powers[0] = nan; });
    add([](auto& state) { state.context.powers.fill(33); });
    add([](auto& state) { state.context.powers[2] = 20; });
    add([](auto& state) {
        state.context.bonuses[3] = damage::kOwnColourBonus;
        state.context.powers[3] = 20 * damage::kOwnColourBonus;
    });
    add([&](auto& state) { state.context.bonuses[3] = nan; });
    add([](auto& state) { state.reaction = PlayerDeed::TurboFull; });
    add([](auto& state) { state.animation.shielded = true; });
    add([](auto& state) { state.animation.comboRide = true; });
    add([](auto& state) { state.animation.itemReleased = PlayerDeed::Hammer; });
    for (const auto& state : invalid) {
        CHECK_FALSE(state.valid());
        CHECK_FALSE(simulation.restore(tree, kOwner, state));
        CHECK(simulation.checkpoint() == baseline);
    }
    for (const auto owner : {Simulation::Binding{0, 1, 0}, Simulation::Binding{1, 0, 0},
                             Simulation::Binding{1, 1, 4}}) {
        CHECK_FALSE(simulation.restore(tree, owner, baseline));
        CHECK(simulation.checkpoint() == baseline);
    }
    reachBlend(native);
    auto blending = capture(native);
    REQUIRE(blending.transitionPose);
    const auto correct = blending;
    blending.transitionPose.reset();
    CHECK_FALSE(simulation.restore(tree, kOwner, blending));
    blending = correct;
    blending.transitionPose->previous.nodes.front().name = "UNRELATED";
    CHECK_FALSE(simulation.restore(tree, kOwner, blending));
    CHECK(simulation.checkpoint() == baseline);
}

TEST_CASE("potion frames reject gaps unsupported actions and nonfinite launch data atomically",
          "[potion-simulation][rollback]") {
    const auto tree = potionTree();
    PlayerAnimator native;
    REQUIRE(native.bind(tree, false));
    const auto baseline = capture(native);
    Simulation simulation;
    REQUIRE(simulation.restore(tree, kOwner, baseline));
    std::vector<Simulation::Frame> invalid{{10}, {12}, {0}};
    for (const auto deed :
         {PlayerDeed::Attack, PlayerDeed::StrongAttack, PlayerDeed::ShieldPotion, PlayerDeed::Combo,
          PlayerDeed::TurboFull, PlayerDeed::Shove, PlayerDeed::Die}) {
        invalid.push_back({11, PlayerMotion::Stand, StrafeWay::None, deed});
    }
    Simulation::Frame frame{11};
    frame.position.y = std::numeric_limits<f32>::infinity();
    invalid.push_back(frame);
    frame = {11};
    frame.facing = {0, 0, 0};
    invalid.push_back(frame);
    frame = {11};
    frame.facing.y = 0.1f;
    invalid.push_back(frame);
    frame = {11};
    frame.motion = static_cast<PlayerMotion>(255);
    invalid.push_back(frame);
    frame = {11};
    frame.strafe = static_cast<StrafeWay>(255);
    invalid.push_back(frame);
    for (const auto& value : invalid) {
        CHECK_FALSE(simulation.step(value));
        CHECK(simulation.checkpoint() == baseline);
        CHECK(simulation.releases().empty());
    }
    auto empty = baseline;
    empty.potions.clear();
    REQUIRE(simulation.restore(tree, kOwner, empty));
    CHECK_FALSE(simulation.step({11, PlayerMotion::Stand, StrafeWay::None, PlayerDeed::UsePotion}));
    CHECK(simulation.checkpoint() == empty);
    REQUIRE(simulation.step({11}));
}

struct MovementFixture {
    WorldCollision floor;
    MatchContext context;
    CombatSnapshot state;
    LocalMovementPrediction prediction;

    MovementFixture(const TreeInfo& tree, const PlayerRuntime& runtime) {
        floor.build(
            {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
             {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
        context.epoch = kOwner.epoch;
        context.scene = 1;
        context.owners = {1, 2, 0, 0};
        context.grants = {1, kOwner.grant, 0, 0};
        state.motion.epoch = context.epoch;
        state.motion.tick = 10;
        state.motion.cameraContinuity = 1;
        state.motion.players[0] = SeatMotion{1, 1, {-50, 0, 0}, 0};
        state.players[0].emplace().health = 100;
        state.motion.players[1] =
            SeatMotion{kOwner.grant, 1, runtime.actor.position(), runtime.actor.yaw()};
        auto& player = state.players[1].emplace();
        player.health = 100;
        player.movement.enabled = player.movement.current = true;
        player.movement.radius = runtime.actor.radius();
        player.movement.height = runtime.actor.height();
        player.movement.followHeight = runtime.actor.followPoint().y - runtime.actor.position().y;
        player.movement.speed = runtime.actor.speed();
        captureRuntime(runtime);
        prediction.begin(context, 2, floor);
        REQUIRE(prediction.setPlayer(1, tree));
        prediction.update(&state, {});
    }

    void captureRuntime(const PlayerRuntime& runtime) {
        auto& player = *state.players[1];
        const auto& animator = runtime.figure->animator();
        const auto& cursor = animator.player();
        player.animation = {static_cast<u32>(animator.action()), cursor.sequence(),
                            cursor.generation(), cursor.presentationFrame(), cursor.transition()};
        player.rollback = animator.checkpoint();
        player.transitionPose = animator.transitionPose();
        player.knockback = runtime.knockback.checkpoint();
        player.floor = PlayerFloorState{0, 4, std::nullopt};
        player.locomotion.emplace();
        player.locomotion->origin = runtime.actor.position();
        player.locomotion->radius = 8;
        player.locomotion->ticks = 12;
        player.potion.emplace();
        player.potion->potions = runtime.actor.save().progress().inventory.potions;
        player.potion->ticks = 12;
        state.motion.players[1]->position = runtime.actor.position();
        state.motion.players[1]->yaw = std::remainder(runtime.actor.yaw(), kTwoPi);
        REQUIRE(state.valid());
    }

    InputCommand input(u64 tick) const {
        InputCommand command;
        command.epoch = context.epoch;
        command.tick = tick;
        command.seat = 1;
        command.grant = kOwner.grant;
        return command;
    }

    CombatSnapshot shown() const {
        auto shown = state;
        prediction.present(shown, 1);
        REQUIRE(shown.valid());
        return shown;
    }
};

TEST_CASE(
    "local potion action replay follows real party motion and privately spends only at release",
    "[potion-simulation][potion-rollback][assets]") {
    const s32 character = GENERATE(0, 2, 6);
    const bool thrown = GENERATE(false, true);
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    std::array<PlayerRuntime, 1> party;
    auto& runtime = party.front();
    CharacterSave save;
    save.character = character;
    save.autoAttack = false;
    save.progress().inventory.potions = {1, 2};
    runtime.actor.spawn(1, save, nullptr, Vec3{0}, 0);
    runtime.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(runtime.figure);
    REQUIRE(runtime.figure->actionTree());
    MovementFixture fixture(*runtime.figure->actionTree(), runtime);
    FloorRiding::land(party, 0, runtime.actor.position(), fixture.floor);
    fixture.captureRuntime(runtime);
    std::vector<CombatSnapshot> history{fixture.state};
    struct NativeRelease {
        u64 tick;
        u64 generation;
        s32 kind;
        Vec3 position;
        s32 charge;
    };
    std::vector<NativeRelease> releases;
    u64 tick = 10;
    usize eventsSeen = 0;
    PartyMotion::Events events;
    events.select = [](usize, const SelectorInput&, s32) {};
    events.advanceTurbo = [](usize, s32, f32) {};
    events.perform = [&](usize, PartyMotion::Action action) {
        ++eventsSeen;
        if (action == PartyMotion::Action::UsePotion ||
            action == PartyMotion::Action::ThrowPotion) {
            auto& inventory = runtime.actor.save().progress().inventory;
            if (!inventory.potions.empty()) {
                releases.push_back({tick, runtime.figure->animator().player().generation(),
                                    inventory.takePotion(), runtime.actor.position(),
                                    runtime.figure->animator().potionThrowTicks()});
            }
        }
    };
    for (tick = 11; tick <= 100; ++tick) {
        auto command = fixture.input(tick);
        if (tick < 45) {
            command.direction = {0, 1};
            command.magnitude = 1;
        }
        if (tick == 11 || (thrown && tick <= 17) || tick == 65) {
            if (thrown) {
                command.heldButtons |= static_cast<u32>(CommandHeld::ThrowPotion);
            } else {
                command.pressedButtons |= static_cast<u32>(CommandPress::UsePotion);
            }
        }
        command.aimDirection = tick < 35 ? Vec3{0.6f, 0, 0.8f} : Vec3{-1, 0, 0};
        if (tick >= 22 && tick < 32) {
            command.heldButtons |= static_cast<u32>(CommandHeld::Strafe);
        }
        REQUIRE(command.valid());
        std::array<PlayInput, InputCommand::kSeats> inputs;
        inputs[1] = SessionInputs::playInput(command);
        PartyMotion::step(party, inputs, false, 0, 1, Simulation::kStep, fixture.floor, events);
        fixture.state.motion.tick = tick;
        fixture.state.players[1]->rollbackInput = command;
        fixture.captureRuntime(runtime);
        history.push_back(fixture.state);
        const auto& delayed = history[static_cast<usize>(tick > 16 ? tick - 16 : 0)];
        const auto inventory = runtime.actor.save().progress().inventory;
        const auto beforeEvents = eventsSeen;
        fixture.prediction.update(&delayed, std::array{command});
        CAPTURE(character, thrown, tick, runtime.figure->animator().action());
        const auto* replay = fixture.prediction.replayed(1);
        REQUIRE(replay);
        const auto shown = fixture.shown();
        CHECK(shown.players[1]->rollback == runtime.figure->animator().checkpoint());
        CHECK(shown.players[1]->transitionPose == runtime.figure->animator().transitionPose());
        CHECK(glm::distance(shown.motion.players[1]->position, runtime.actor.position()) ==
              Approx(0).margin(0.0001));
        CHECK(std::remainder(shown.motion.players[1]->yaw - runtime.actor.yaw(), kTwoPi) ==
              Approx(0).margin(0.0001));
        usize pending = 0;
        for (const auto& release : releases) {
            if (release.tick <= delayed.motion.tick) {
                continue;
            }
            REQUIRE(pending < replay->potions.size());
            const auto& predicted = replay->potions[pending++];
            CHECK(predicted.origin.tick == release.tick);
            CHECK(predicted.origin.generation == release.generation);
            CHECK(predicted.storedKind == release.kind);
            CHECK(glm::distance(predicted.context.position, release.position) ==
                  Approx(0).margin(0.0001));
        }
        CHECK(replay->potions.size() == pending);
        CHECK(replay->releases.empty());
        CHECK(shown.projectiles.empty());      // descriptors are not live damage-bearing missiles
        CHECK_FALSE(shown.players[1]->potion); // mixed-age drawing cannot recertify inventory
        for (usize render = 0; render < tick % 4; ++render) {
            CHECK(fixture.shown().players[1]->rollback == runtime.figure->animator().checkpoint());
        }
        CHECK(runtime.actor.save().progress().inventory == inventory);
        CHECK(eventsSeen == beforeEvents);
    }
    REQUIRE_FALSE(releases.empty());
}

TEST_CASE(
    "local potion prediction withdraws all observations when the certificate or controls change",
    "[potion-simulation][potion-rollback][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    PlayerRuntime runtime;
    CharacterSave save;
    save.progress().inventory.potions = {2};
    runtime.actor.spawn(1, save, nullptr, Vec3{0}, 0);
    runtime.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(runtime.figure);
    REQUIRE(runtime.figure->actionTree());
    MovementFixture fixture(*runtime.figure->actionTree(), runtime);
    auto command = fixture.input(11);
    command.heldButtons = static_cast<u32>(CommandHeld::ThrowPotion);
    fixture.prediction.update(&fixture.state, std::array{command});
    REQUIRE(fixture.prediction.replayed(1));
    auto& player = *fixture.state.players[1];
    SECTION("stable inventory prefix expires") {
        player.potion->ticks = 1;
    }
    SECTION("potion descriptor revoked") {
        player.potion.reset();
    }
    SECTION("weapon acquisition not certified") {
        command.heldButtons |= static_cast<u32>(CommandHeld::Attack);
    }
    SECTION("shield input has different native priority") {
        command.heldButtons |= static_cast<u32>(CommandHeld::ShieldPotion);
    }
    SECTION("selector would change native inventory") {
        command.pressedButtons = static_cast<u32>(CommandPress::SelectorRight);
    }
    // Resolve the first sampled command only when changing its checkpoint; the
    // host can reject it and retain Ready. The next command is genuinely future.
    fixture.state.motion.tick = 11;
    if (player.potion && player.potion->ticks == 1) {
        player.potion->ticks = 0;
    }
    command.tick = 12;
    fixture.prediction.update(&fixture.state, std::array{command});
    CHECK(fixture.prediction.replayed(1) == nullptr);
    CHECK(fixture.shown().players[1]->rollback == player.rollback);
}
} // namespace
