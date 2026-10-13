#include <array>
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ItemArchive.h"
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
constexpr f32 kStep = 1.0f / 60;

WorldCollision shieldFloor() {
    WorldCollision result;
    result.build({{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}}, 0},
                  {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}}, 0}});
    return result;
}

TreeInfo shieldTree() {
    TreeInfo result;
    result.name = "WAR";
    TreeNodeInfo root;
    root.name = "ROOT";
    root.type = 1;
    result.nodes.push_back(root);
    for (const auto name : PlayerAnimator::kSequenceNames) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 8;
        sequence.frameRate = 30;
        sequence.trackOfNode = {-1};
        result.sequences.push_back(sequence);
    }
    return result;
}

void captureAction(PlayerCombatState& target, const PlayerAnimator& animator) {
    target.rollback = animator.checkpoint();
    const auto& cursor = animator.player();
    target.animation = {static_cast<u32>(animator.action()), cursor.sequence(), cursor.generation(),
                        cursor.presentationFrame(), cursor.transition()};
}

void captureCompanion(PlayerCombatState& state, const PlayerRuntime& runtime) {
    const auto worn = PowerupEffects::of(runtime.actor.save().progress().inventory);
    const auto body =
        PlayerFigure::bodyPlacement(runtime.actor.transform(), runtime.actor.save(), worn);
    const auto visuals = runtime.figure->companionVisuals(body, 1);
    state.companions[1].reset();
    if (const auto& visual = visuals[1]) {
        state.companions[1] =
            CompanionState{visual->form,
                           visual->placement,
                           {0, visual->sequence, visual->generation, visual->frame, 1},
                           visual->textureClock,
                           visual->alpha,
                           runtime.figure->companion().animationCheckpoint()};
    }
}

void sameCompanion(const std::optional<CompanionState>& predicted,
                   const std::optional<CompanionState>& native) {
    REQUIRE(predicted.has_value() == native.has_value());
    if (!native) {
        return;
    }
    CHECK(predicted->form == native->form);
    CHECK(predicted->alpha == Approx(native->alpha).margin(0.00001));
    CHECK(predicted->textureClock == Approx(native->textureClock).margin(0.00001));
    CHECK(predicted->animation.sequence == native->animation.sequence);
    CHECK(predicted->animation.frame == Approx(native->animation.frame).margin(0.00001));
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            CHECK(predicted->placement[column][row] ==
                  Approx(native->placement[column][row]).margin(0.0001));
        }
    }
    REQUIRE(predicted->rollback);
    REQUIRE(native->rollback);
    // No visible companion means no animation seed in the checkpoint. New local
    // activations therefore have their own cosmetic generation, but every native
    // phase/loop/transition field must still match exactly.
    auto cursor = *predicted->rollback;
    cursor.generation = native->rollback->generation;
    CHECK(cursor == *native->rollback);
}

struct ShieldFixture {
    TreeInfo tree = shieldTree();
    TreeInfo fireTree = shieldTree();
    WorldCollision floor = shieldFloor();
    MatchContext context;
    CombatSnapshot state;
    LocalMovementPrediction prediction;

    ShieldFixture() {
        fireTree.name = "FW_SHLD_ACTIVE";
        fireTree.sequences.resize(1);
        context.epoch = 4;
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
            auto& player = state.players[seat].emplace();
            player.health = 100;
            player.movement.enabled = true;
            player.movement.current = true;
            player.modifiers =
                PlayerWeaponModifiers{0, 120, false, powerup::kFireShield, -1, kStep};
            captureAction(player, animator);
        }
    }

    void reset() {
        REQUIRE(state.valid());
        prediction.begin(context, 2, floor);
        REQUIRE(prediction.setPlayer(1, tree));
        REQUIRE(prediction.setPlayer(2, tree));
        REQUIRE(prediction.setFireShield(1, fireTree, 1));
        REQUIRE(prediction.setFireShield(2, fireTree, 1));
        prediction.update(&state, {});
    }

    InputCommand input(u64 tick, u8 seat = 1) const {
        InputCommand result;
        result.epoch = context.epoch;
        result.tick = tick;
        result.seat = seat;
        result.grant = context.grants[seat];
        result.direction = {0, 1};
        result.magnitude = 1;
        return result;
    }

    CombatSnapshot shown() const {
        auto result = state;
        prediction.present(result, 1);
        REQUIRE(result.valid());
        return result;
    }
};

TEST_CASE("shield locomotion replays native party movement with mixed equipment and late authority",
          "[netplay][shield-rollback][assets]") {
    const s32 character = GENERATE(0, 2, 6);
    const u32 shields = GENERATE(powerup::kReflectShield, powerup::kFireShield,
                                 powerup::kLightningShield, powerup::kShields);
    const bool mixed = GENERATE(false, true);
    CAPTURE(character, shields, mixed);
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    ItemArchive weapons;
    REQUIRE(powerups.load(root / "POWERUPS"));
    REQUIRE(weapons.load(root / "WEAPONS"));
    std::array<PlayerRuntime, 1> native;
    auto& runtime = native.front();
    CharacterSave save;
    save.character = character;
    save.progress().inventory.addPowerup(powerup::kArmor, shields, 0, 600);
    const u32 flags = mixed ? powerup::kRapidFire | powerup::kFiveWayShot | 4 : 0;
    if (mixed) {
        save.progress().inventory.addPowerup(powerup::kWeapon, flags, 0, 600);
        save.progress().inventory.addPowerup(powerup::kSpeed, 0, 3, 600);
    }
    runtime.actor.spawn(1, save, nullptr, Vec3{0}, 0.6f);
    runtime.actor.setPaceBonus(PowerupEffects::of(save.progress().inventory).paceAdd);
    runtime.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(runtime.figure);
    REQUIRE(runtime.figure->actionTree());
    ShieldFixture f;
    f.tree = *runtime.figure->actionTree();
    const auto fireIndex = weapons.trees.find("FW_SHLD_ACTIVE");
    REQUIRE(fireIndex);
    f.fireTree = weapons.trees.tree(*fireIndex);
    f.state.motion.players[1]->yaw = runtime.actor.yaw();
    auto& state = *f.state.players[1];
    state.movement.radius = runtime.actor.radius();
    state.movement.height = runtime.actor.height();
    state.movement.followHeight = runtime.actor.followPoint().y;
    state.movement.speed = runtime.actor.speed();
    state.movement.cameraYaw = 0.4f;
    const bool fiery = (shields & powerup::kFireShield) != 0;
    state.modifiers =
        PlayerWeaponModifiers{flags, 120, mixed, shields, fiery ? 600.0f : 0, fiery ? kStep : 0};
    state.knockback = runtime.knockback.checkpoint();
    captureAction(state, runtime.figure->animator());
    f.reset();
    REQUIRE(f.prediction.setFireShield(
        1, f.fireTree,
        PlayerFigure::bodyScale(save, PowerupEffects::of(save.progress().inventory))));
    std::vector<CombatSnapshot> history{f.state};
    usize eventsSeen = 0;
    PartyMotion::Events events;
    events.select = [](usize, const SelectorInput&, s32) {};
    events.advanceTurbo = [](usize, s32, f32) {};
    events.perform = [&](usize, PartyMotion::Action) { ++eventsSeen; };
    bool sawReady = false;
    bool sawRun = false;
    for (u64 tick = 11; tick <= 82; ++tick) {
        CAPTURE(tick);
        auto command = f.input(tick);
        const u64 phase = (tick - 11) % 24;
        if (phase < 4 || phase >= 20) {
            command.direction = Vec2{0};
            command.magnitude = 0;
        } else if (phase >= 12) {
            command.direction = {1, 0};
        }
        if (tick >= 35 && tick < 59) {
            command.heldButtons = static_cast<u32>(CommandHeld::Strafe);
        }
        if (tick >= 47) {
            command.aimDirection = tick % 8 == 0 ? Vec3{0} : glm::normalize(Vec3{0.3f, 0, -1});
        }
        REQUIRE(command.valid());
        std::array<PlayInput, InputCommand::kSeats> inputs;
        inputs[1] = SessionInputs::playInput(command);
        runtime.actor.save().progress().inventory.advance(kStep);
        runtime.figure->setCompanionPowerups(device, powerups,
                                             runtime.actor.save().progress().inventory, &weapons);
        PartyMotion::step(native, inputs, false, state.movement.cameraYaw, 1, kStep, f.floor,
                          events);
        const auto& animator = runtime.figure->animator();
        sawReady = sawReady || animator.action() == PlayerAnimator::Action::ShieldReady;
        sawRun = sawRun || animator.action() == PlayerAnimator::Action::ShieldRun;
        f.state.motion.tick = tick;
        auto& motion = *f.state.motion.players[1];
        motion.position = runtime.actor.position();
        motion.yaw = std::remainder(runtime.actor.yaw(), kTwoPi);
        captureAction(state, animator);
        captureCompanion(state, runtime);
        state.knockback = runtime.knockback.checkpoint();
        state.rollbackInput = command;
        if (fiery) {
            state.modifiers->fireSeconds =
                runtime.actor.save().progress().inventory.powerups[0].strength;
        }
        history.push_back(f.state);
        const auto& delayed = history[static_cast<usize>(tick > 16 ? tick - 16 : 0)];
        const auto inventory = runtime.actor.save().progress().inventory;
        const auto health = runtime.actor.save().health();
        const auto beforeEvents = eventsSeen;
        f.prediction.update(&delayed, std::array{command});
        const auto shown = f.shown();
        REQUIRE(f.prediction.replayed(1));
        CHECK(f.prediction.replayed(1)->releases.empty());
        CHECK(shown.players[1]->rollback == animator.checkpoint());
        CHECK(shown.players[1]->knockback == runtime.knockback.checkpoint());
        sameCompanion(shown.players[1]->companions[1], state.companions[1]);
        CHECK(glm::distance(shown.motion.players[1]->position, runtime.actor.position()) ==
              Approx(0).margin(0.0001));
        CHECK(std::remainder(shown.motion.players[1]->yaw - runtime.actor.yaw(), kTwoPi) ==
              Approx(0).margin(0.0001));
        for (usize render = 0; render < tick % 4; ++render) {
            CHECK(f.shown().players[1]->rollback == animator.checkpoint());
        }
        CHECK(runtime.actor.save().progress().inventory == inventory);
        CHECK(runtime.actor.save().health() == health);
        CHECK(eventsSeen == beforeEvents);
        CHECK(shown.projectiles.empty());
    }
    CHECK(sawReady);
    CHECK(sawRun);
}

TEST_CASE("shield certificates never authorize action or inventory input despite ranged claims",
          "[netplay][shield-rollback]") {
    constexpr std::array held{CommandHeld::Attack,       CommandHeld::StrongAttack,
                              CommandHeld::ShieldPotion, CommandHeld::Combo,
                              CommandHeld::Turbo,        CommandHeld::ThrowPotion};
    constexpr std::array pressed{
        CommandPress::Attack,       CommandPress::Defend,       CommandPress::TurboAttack,
        CommandPress::Charge,       CommandPress::UsePotion,    CommandPress::SelectorUp,
        CommandPress::SelectorDown, CommandPress::SelectorLeft, CommandPress::SelectorRight};
    for (usize index = 0; index < held.size() + pressed.size(); ++index) {
        CAPTURE(index);
        ShieldFixture f;
        f.state.players[1]->movement.ranged = true; // contradictory yet structurally valid
        f.reset();
        auto unsafe = f.input(11);
        if (index < held.size()) {
            unsafe.heldButtons = static_cast<u32>(held[index]);
        } else {
            unsafe.pressedButtons = static_cast<u32>(pressed[index - held.size()]);
        }
        // A second locally owned seat remains responsive when the first is unsafe.
        f.prediction.update(&f.state, std::array{unsafe, f.input(11, 2)});
        CHECK(f.prediction.replayed(1) == nullptr);
        REQUIRE(f.prediction.replayed(2));
        const auto shown = f.shown();
        CHECK(shown.motion.players[1]->position == f.state.motion.players[1]->position);
        CHECK(shown.players[1]->rollback == f.state.players[1]->rollback);
        CHECK(glm::distance(shown.motion.players[2]->position,
                            f.state.motion.players[2]->position) > 0);
        CHECK(f.prediction.replayed(2)->releases.empty());
    }
}

TEST_CASE("shield equipment rejects pending reactions and inherited attack actions",
          "[netplay][shield-rollback]") {
    ShieldFixture f;
    auto& state = *f.state.players[1];
    SECTION("queued native hit") {
        state.knockback = Knockback::Checkpoint{};
        state.reaction = PlayerDeed::Flinch;
        state.knockbackTicks = 1;
    }
    SECTION("continuing native slide") {
        state.knockback = Knockback::Checkpoint{};
        state.knockback->velocity = {1, 0, 0};
        state.knockbackTicks = 1;
    }
    SECTION("ordinary throw already in flight") {
        PlayerAnimator animator;
        REQUIRE(animator.bind(f.tree, false));
        animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Attack);
        REQUIRE_FALSE(PlayerAnimator::locomotion(animator.action()));
        captureAction(state, animator);
    }
    f.reset();
    f.prediction.update(&f.state, std::array{f.input(11)});
    CHECK(f.prediction.replayed(1) == nullptr);
    CHECK(f.shown().players[1]->rollback == state.rollback);
    CHECK(f.shown().motion.players[1]->position == f.state.motion.players[1]->position);
}

TEST_CASE("shield expiry preserves the preceding gait until native animation updates",
          "[netplay][shield-rollback]") {
    ShieldFixture f;
    auto& state = *f.state.players[1];
    state.modifiers->ticks = 3;
    f.reset();
    for (u64 tick = 11; tick <= 13; ++tick) {
        f.prediction.update(&f.state, std::array{f.input(tick)});
        REQUIRE(f.prediction.replayed(1));
        CHECK(f.shown().players[1]->rollback->shielded);
    }
    const auto lastSafe = f.shown();
    REQUIRE(lastSafe.players[1]->rollback);
    f.prediction.update(&f.state, std::array{f.input(14)});
    CHECK(f.prediction.replayed(1) == nullptr);
    CHECK(f.shown().motion.players[1]->position == f.state.motion.players[1]->position);

    // The inventory has expired at the next boundary, while the completed animator
    // still retains ShieldRun. Replay must consume that old action's pace first.
    f.state = lastSafe;
    f.state.motion.tick = 13;
    f.state.players[1]->modifiers.reset();
    PlayerActor native;
    const auto before = *f.state.motion.players[1];
    const auto& envelope = f.state.players[1]->movement;
    native.restoreMotion(before.position, before.yaw, envelope.speed, envelope.radius,
                         envelope.height, envelope.followHeight);
    PlayerAnimator animator;
    REQUIRE(animator.bind(f.tree, false));
    REQUIRE(animator.restore(*f.state.players[1]->rollback));
    const auto input = SessionInputs::playInput(f.input(14));
    const f32 pace = PartyMotion::actionPace(&animator, PlayerDeed::None);
    native.update(input.move, envelope.cameraYaw, kStep, &f.floor, pace, false);
    native.fall(kStep, f.floor);
    animator.setShielded(false);
    animator.update(PlayerAnimator::motionFor(input.move.magnitude), 1, kStep, PlayerDeed::None);
    f.prediction.update(&f.state, {});
    REQUIRE(f.prediction.replayed(1));
    CHECK(f.shown().players[1]->rollback == animator.checkpoint());
    CHECK(glm::distance(f.shown().motion.players[1]->position, native.position()) ==
          Approx(0).margin(0.0001));
}

TEST_CASE("shield movement obeys stale and ownership barriers", "[netplay][shield-rollback]") {
    ShieldFixture f;
    f.reset();
    f.prediction.update(&f.state, std::array{f.input(11)});
    REQUIRE(f.prediction.replayed(1));
    for (u64 tick = 12; tick <= 12 + LocalMovementPrediction::kStaleTicks; ++tick) {
        f.prediction.update(nullptr, std::array{f.input(tick)});
    }
    CHECK(f.prediction.replayed(1) == nullptr);
    auto refreshed = f.state;
    refreshed.motion.tick = 20;
    refreshed.motion.players[1]->grant = 5;
    f.prediction.update(&refreshed, std::array{f.input(44)});
    CHECK(f.prediction.replayed(1) == nullptr);
    f.prediction.clear();
    CHECK(f.prediction.replayed(1) == nullptr);
}

TEST_CASE("fire shield fade follows the captured native inventory clock and reconciles changes",
          "[netplay][shield-rollback][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    ItemArchive weapons;
    REQUIRE(powerups.load(root / "POWERUPS"));
    REQUIRE(weapons.load(root / "WEAPONS"));
    std::array<PlayerRuntime, 1> native;
    auto& runtime = native.front();
    CharacterSave save;
    save.character = 0;
    save.progress().inventory.addPowerup(powerup::kArmor, powerup::kFireShield, 0, 0.85f);
    runtime.actor.spawn(1, save, nullptr, Vec3{0}, 0);
    runtime.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(runtime.figure);
    runtime.figure->setShielded(true);
    runtime.figure->animate(1, 1, kStep);
    REQUIRE(runtime.figure->animator().action() == PlayerAnimator::Action::ShieldRun);
    runtime.figure->setCompanionPowerups(device, powerups,
                                         runtime.actor.save().progress().inventory, &weapons);
    runtime.figure->animate(1, 1, kStep);
    ShieldFixture f;
    f.tree = *runtime.figure->actionTree();
    const auto fireIndex = weapons.trees.find("FW_SHLD_ACTIVE");
    REQUIRE(fireIndex);
    f.fireTree = weapons.trees.tree(*fireIndex);
    auto& state = *f.state.players[1];
    state.movement.radius = runtime.actor.radius();
    state.movement.height = runtime.actor.height();
    state.movement.followHeight = runtime.actor.followPoint().y;
    state.movement.speed = runtime.actor.speed();
    state.knockback = runtime.knockback.checkpoint();
    captureAction(state, runtime.figure->animator());
    captureCompanion(state, runtime);
    state.modifiers->fireSeconds = 0.85f;
    state.modifiers->inventoryStep = 0;
    f.reset();
    PartyMotion::Events events;
    events.select = [](usize, const SelectorInput&, s32) {};
    events.advanceTurbo = [](usize, s32, f32) {};
    usize actions = 0;
    events.perform = [&](usize, PartyMotion::Action) { ++actions; };
    u64 tick = 10;
    // A sleeping boss holds inventory, an awake boss drains it at 3x, and its
    // defeat holds it again. An ordinary stage subsequently uses the level clock.
    for (const f32 inventoryStep : {0.0f, 3 * kStep, 0.0f, kStep}) {
        CAPTURE(inventoryStep);
        f.state.motion.tick = tick;
        f.state.motion.players[1]->position = runtime.actor.position();
        f.state.motion.players[1]->yaw = runtime.actor.yaw();
        captureAction(state, runtime.figure->animator());
        captureCompanion(state, runtime);
        state.knockback = runtime.knockback.checkpoint();
        state.modifiers->fireSeconds =
            runtime.actor.save().progress().inventory.powerups[0].strength;
        state.modifiers->inventoryStep = inventoryStep;
        f.prediction.update(&f.state, {});
        for (usize step = 0; step < 4; ++step) {
            const auto command = f.input(++tick);
            std::array<PlayInput, InputCommand::kSeats> inputs;
            inputs[1] = SessionInputs::playInput(command);
            auto& inventory = runtime.actor.save().progress().inventory;
            inventory.advance(inventoryStep);
            runtime.figure->setCompanionPowerups(device, powerups, inventory, &weapons);
            PartyMotion::step(native, inputs, false, 0, 1, kStep, f.floor, events);
            PlayerCombatState nativeState;
            captureCompanion(nativeState, runtime);
            const auto before = inventory;
            f.prediction.update(&f.state, std::array{command});
            REQUIRE(f.prediction.replayed(1));
            const auto shown = f.shown();
            sameCompanion(shown.players[1]->companions[1], nativeState.companions[1]);
            CHECK(shown.players[1]->rollback == runtime.figure->animator().checkpoint());
            CHECK(inventory == before);
            CHECK(actions == 0);
            CHECK(shown.projectiles.empty());
        }
    }
    CHECK(runtime.actor.save().progress().inventory.powerups[0].strength ==
          Approx(0.85f - 4 * (3 * kStep + kStep)).margin(0.00001));
}

TEST_CASE("fire shield replay needs its trusted asset and exact visible animation seed",
          "[netplay][shield-rollback]") {
    ShieldFixture f;
    SECTION("unbound native tree") {
        f.prediction.begin(f.context, 2, f.floor);
        REQUIRE(f.prediction.setPlayer(1, f.tree));
    }
    SECTION("visible fire without an exact animation seed") {
        f.state.players[1]->companions[1] = CompanionState{
            static_cast<u32>(PowerupCompanion::Kind::FireShield), Mat4{1}, {0, 0, 1, 0, 1}};
        f.reset();
    }
    f.prediction.update(&f.state, std::array{f.input(11)});
    CHECK(f.prediction.replayed(1) == nullptr);
    CHECK(f.shown().players[1]->rollback == f.state.players[1]->rollback);
}
} // namespace
