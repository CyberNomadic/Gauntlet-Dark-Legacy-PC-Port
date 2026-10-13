#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/MeleeSelection.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/SessionInputs.h"
#include "game/world/BodyCollision.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TreeInfo locomotionTree() {
    TreeInfo tree;
    tree.name = "WAR";
    TreeNodeInfo node;
    node.name = "ROOT";
    node.type = 1;
    tree.nodes.push_back(node);
    for (const auto name : PlayerAnimator::kSequenceNames) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 60;
        sequence.frameRate = 30;
        sequence.trackOfNode = {-1};
        tree.sequences.push_back(sequence);
    }
    return tree;
}
WorldCollision flatFloor() {
    WorldCollision floor;
    floor.build({{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}}, 0},
                 {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}}, 0}});
    return floor;
}
void captureAction(PlayerCombatState& state, const PlayerAnimator& animator) {
    state.rollback = animator.checkpoint();
    state.transitionPose = animator.transitionPose();
    const auto& cursor = animator.player();
    state.animation = {static_cast<u32>(animator.action()), cursor.sequence(), cursor.generation(),
                       cursor.presentationFrame(), cursor.transition()};
}
struct Prediction {
    TreeInfo tree = locomotionTree();
    WorldCollision floor = flatFloor();
    MatchContext context;
    CombatSnapshot state;
    LocalMovementPrediction client;
    explicit Prediction(s32 character = -1) {
        context.epoch = 3;
        context.scene = 1;
        context.owners = {1, 2, 2, 0};
        context.grants = {1, 4, 2, 0};
        state.motion.epoch = context.epoch;
        state.motion.tick = 10;
        state.motion.cameraContinuity = 1;
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.setCharacter(character);
        for (usize seat = 0; seat < 3; ++seat) {
            state.motion.players[seat] = SeatMotion{context.grants[seat], 1, Vec3{0}, 0};
            state.players[seat] = PlayerCombatState{};
            state.players[seat]->health = 100;
            captureAction(*state.players[seat], animator);
            state.players[seat]->movement.enabled = true;
            state.players[seat]->movement.current = true;
            state.players[seat]->movement.ranged = true;
            state.players[seat]->shadow = PlayerShadowState{};
        }
        reset();
    }
    void reset() {
        client.begin(context, 2, floor);
        REQUIRE(client.setPlayer(1, tree));
        REQUIRE(client.setPlayer(2, tree));
        client.update(&state, {});
    }
    InputCommand command(u64 tick, u8 seat = 1) const {
        InputCommand result;
        result.epoch = context.epoch;
        result.tick = tick;
        result.grant = context.grants[seat];
        result.seat = seat;
        result.direction = {0, 1};
        result.magnitude = 1;
        return result;
    }
    CombatSnapshot shown(f32 blend = 1) const {
        auto result = state;
        client.present(result, blend);
        REQUIRE(result.valid());
        return result;
    }
};

TEST_CASE("local movement responds before a round trip and replay resolves each input once",
          "[netplay][prediction]") {
    Prediction fixture;
    const auto command = fixture.command(11);
    fixture.client.update(&fixture.state, std::array{command});
    const auto immediate = fixture.shown();
    CHECK(immediate.motion.players[1]->position.z == Approx(5.0f / 60));
    CHECK(immediate.motion.players[0]->position == Vec3{0});
    CHECK(immediate.motion.players[2]->position == Vec3{0});
    CHECK(immediate.players[1]->animation.action == static_cast<u32>(PlayerAnimator::Action::Run1));
    CHECK(immediate.players[1]->shadow->ground.z == immediate.motion.players[1]->position.z);
    CHECK(fixture.state.motion.players[1]->position == Vec3{0});
    CHECK(fixture.state.players[1]->health == 100);
    // Duplicate packet/sample has no second step; acknowledging the step has no
    // second step either. The baseline renderer would still be at the old checkpoint.
    fixture.client.update(&fixture.state, std::array{command});
    CHECK(fixture.shown().motion.players[1]->position == immediate.motion.players[1]->position);
    fixture.state = immediate;
    fixture.state.motion.tick = 11;
    fixture.client.update(&fixture.state, {});
    CHECK(fixture.shown().motion.players[1]->position == immediate.motion.players[1]->position);
    CHECK(fixture.client.metrics().pending == 0);
    CHECK(fixture.client.metrics().resolvedDeadlineMs > 0);
}

TEST_CASE("prediction retains ownership, cutscene locks, teleports and bounded loss recovery",
          "[netplay][prediction]") {
    Prediction fixture;
    SECTION("remote and revoked input cannot predict") {
        auto remote = fixture.command(11, 0);
        auto revoked = fixture.command(11);
        ++revoked.grant;
        fixture.client.update(&fixture.state, std::array{remote, revoked});
        CHECK(fixture.client.metrics().pending == 0);
    }
    SECTION("knockdown or cutscene revokes pending movement") {
        fixture.client.update(&fixture.state, std::array{fixture.command(15)});
        ++fixture.state.motion.tick;
        fixture.state.players[1]->movement.enabled = false;
        fixture.state.players[1]->movement.current = false;
        fixture.state.players[1]->movement.ranged = false;
        fixture.client.update(&fixture.state, {});
        CHECK(fixture.shown().motion.players[1]->position == Vec3{0});
        CHECK(fixture.client.metrics().pending == 0);
        CHECK(fixture.client.metrics().cuts > 0);
    }
    SECTION("teleport cannot interpolate across the map") {
        fixture.client.update(&fixture.state, std::array{fixture.command(15)});
        ++fixture.state.motion.tick;
        ++fixture.state.motion.players[1]->continuity;
        fixture.state.motion.players[1]->position = {60, 0, 0};
        fixture.client.update(&fixture.state, {});
        CHECK(fixture.shown(0).motion.players[1]->position == Vec3{60, 0, 0});
        CHECK(fixture.client.metrics().pending == 0);
    }
    SECTION("host silence bounds history and stops extrapolation") {
        for (u64 tick = 11; tick < 200; ++tick) {
            fixture.client.update(&fixture.state, std::array{fixture.command(tick)});
        }
        CHECK(fixture.client.metrics().pending <= LocalMovementPrediction::kHistory);
        CHECK(fixture.client.metrics().stalled > 0);
        CHECK(fixture.shown().motion.players[1]->position == Vec3{0});
    }
    SECTION("unapproved melee or item attack requests cannot predict an ordinary throw") {
        fixture.state.players[1]->movement.ranged = false;
        fixture.reset();
        auto attack = fixture.command(11);
        attack.heldButtons = static_cast<u32>(CommandHeld::Attack);
        fixture.client.update(&fixture.state, std::array{attack});
        CHECK(fixture.shown().players[1]->animation.action ==
              fixture.state.players[1]->animation.action);
        CHECK(fixture.shown().projectiles.empty());
    }
}

TEST_CASE("selective player rollback accepts ordinary attacks and guard in the sampled tick",
          "[netplay][prediction][rollback]") {
    const auto deed = GENERATE(PlayerDeed::Attack, PlayerDeed::StrongAttack, PlayerDeed::Defend);
    Prediction fixture;
    auto input = fixture.command(11);
    if (deed == PlayerDeed::Defend) {
        input.pressedButtons = static_cast<u32>(CommandPress::Defend);
        input.heldButtons = static_cast<u32>(CommandHeld::Turbo);
    } else {
        input.heldButtons = static_cast<u32>(
            deed == PlayerDeed::Attack ? CommandHeld::Attack : CommandHeld::StrongAttack);
    }
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*fixture.state.players[1]->rollback));
    const f32 firstPace = deed == PlayerDeed::StrongAttack
                              ? std::min(native.moveScale(), PlayerAnimator::kStrongThrowPace)
                              : native.moveScale();
    native.update(PlayerMotion::Run, 1, 1.0f / 60, deed);
    fixture.client.update(&fixture.state, std::array{input});
    const auto shown = fixture.shown();
    REQUIRE(shown.players[1]->rollback);
    CHECK(*shown.players[1]->rollback == native.checkpoint());
    CHECK(shown.players[1]->animation.action != fixture.state.players[1]->animation.action);
    CHECK(shown.motion.players[1]->position.z == Approx(5.0f / 60 * firstPace));
    CHECK(shown.projectiles.empty());
    CHECK(shown.players[1]->health == fixture.state.players[1]->health);
    CHECK(fixture.client.metrics().samples == 1);
}

TEST_CASE("stationary melee rollback never approves uncommanded stepping actions",
          "[netplay][prediction][rollback][melee]") {
    using Action = PlayerAnimator::Action;
    for (usize index = 0; index < PlayerAnimator::kActionCount; ++index) {
        const auto action = static_cast<Action>(index);
        INFO(index);
        const bool steps = action >= Action::Step1 && action <= Action::WalkStrikeRecover;
        CHECK(PlayerAnimator::rollbackMeleeAction(action) ==
              (PlayerAnimator::isMelee(action) && !steps));
        CHECK(PlayerAnimator::rollbackMovingMeleeAction(action) == PlayerAnimator::isMelee(action));
        CHECK(PlayerAnimator::rollbackTurboAction(action) ==
              (action == Action::TurboStrong || action == Action::TurboFull));
        if (PlayerAnimator::rollbackMeleeAction(action)) {
            CHECK_FALSE(PlayerAnimator::rollbackAction(action));
        }
    }
}

TEST_CASE("shared party planning retains first-swing pace turn locks and released-stick lunges",
          "[netplay][prediction][rollback][moving-melee]") {
    const TreeInfo tree = locomotionTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    PlayerActor actor;
    actor.restoreMotion({0, 0, 0}, 0.4f, 5, 0.75f, 5, 2.5f);
    constexpr f32 kCameraYaw = 0.2f;
    constexpr f32 kSeconds = 1.0f / 60;
    const MoveInput right{{1, 0}, 1};
    SECTION("starting quick and slow swings use their first-frame policy") {
        CHECK(PartyMotion::actionPace(&animator, PlayerDeed::Melee) == Approx(0.25f));
        CHECK(PartyMotion::actionPace(&animator, PlayerDeed::MeleeSlow) == 0);
        CHECK(PartyMotion::actionPace(&animator, PlayerDeed::MeleeLow) == 0);
        CHECK(PartyMotion::actionPace(&animator, PlayerDeed::StrongAttack) == Approx(0.25f));
        CHECK(PartyMotion::actionPace(&animator, PlayerDeed::MeleeSlow, true) == Approx(0.4f));
        const auto quick = PartyMotion::attackMove(actor, &animator, right, PlayerDeed::Melee,
                                                   kCameraYaw, kSeconds, true);
        CHECK(PlayerActor::headingOf(quick, kCameraYaw) == Approx(actor.yaw()));
        const auto slow = PartyMotion::attackMove(actor, &animator, right, PlayerDeed::MeleeSlow,
                                                  kCameraYaw, kSeconds, true);
        CHECK(slow.direction == right.direction);
        CHECK(slow.magnitude == right.magnitude);
    }
    SECTION("a held lunge turns by its native fractional 30 Hz share") {
        animator.setMelee({MeleeRange::Step, false, 0});
        animator.update(PlayerMotion::Walk, 1, kSeconds, PlayerDeed::Melee);
        REQUIRE(animator.lunging());
        CHECK(PartyMotion::actionPace(&animator, PlayerDeed::Melee) == Approx(1));
        const auto step = PartyMotion::attackMove(actor, &animator, right, PlayerDeed::Melee,
                                                  kCameraYaw, kSeconds, true);
        const f32 wanted = PlayerActor::headingOf(right, kCameraYaw);
        const f32 expected = actor.yaw() + (wanted - actor.yaw()) * (1 - std::sqrt(0.75f));
        CHECK(PlayerActor::headingOf(step, kCameraYaw) == Approx(expected));
    }
    SECTION("releasing the stick preserves the already started lunge but held bodies cannot") {
        animator.setMelee({MeleeRange::Step, false, 0});
        animator.update(PlayerMotion::Walk, 1, kSeconds, PlayerDeed::Melee);
        animator.update(PlayerMotion::Stand, 1, kSeconds, PlayerDeed::None);
        REQUIRE(animator.lunging());
        CHECK(PartyMotion::actionPace(&animator, PlayerDeed::None) == Approx(0.5f));
        const auto step = PartyMotion::attackMove(actor, &animator, {}, PlayerDeed::None,
                                                  kCameraYaw, kSeconds, true);
        CHECK(step.magnitude == 1);
        CHECK(PlayerActor::headingOf(step, kCameraYaw) == Approx(actor.yaw()));
        CHECK_FALSE(PartyMotion::attackMove(actor, &animator, {}, PlayerDeed::None, kCameraYaw,
                                            kSeconds, false)
                        .any());
    }
    SECTION("missing figures and reaction locks keep their original behavior") {
        CHECK(PartyMotion::actionPace(nullptr, PlayerDeed::None) == 1);
        CHECK(PartyMotion::actionPace(nullptr, PlayerDeed::Melee) == Approx(0.25f));
        CHECK(PartyMotion::actionPace(nullptr, PlayerDeed::MeleeSlow) == 0);
        CHECK(PartyMotion::actionPace(nullptr, PlayerDeed::StrongAttack) == 1);
        CHECK_FALSE(PartyMotion::attackMove(actor, nullptr, {}, PlayerDeed::None, kCameraYaw,
                                            kSeconds, true)
                        .any());
        animator.update(PlayerMotion::Stand, 1, kSeconds, PlayerDeed::Flinch);
        CHECK(PartyMotion::actionPace(&animator, PlayerDeed::Melee) == 0);
    }
}

TEST_CASE("shared turbo planning retains meter thresholds button priority and angular speed",
          "[netplay][prediction][rollback][turbo-rollback]") {
    const auto tree = locomotionTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, false));
    PlayInput input;
    input.turboAttackPressed = true;
    input.chargePressed = true;
    CHECK(PartyMotion::turboDeed(animator, 39.999f, input) == PlayerDeed::None);
    CHECK(PartyMotion::turboDeed(animator, 40, input) == PlayerDeed::TurboStrong);
    CHECK(PartyMotion::turboDeed(animator, 99.999f, input) == PlayerDeed::TurboStrong);
    CHECK(PartyMotion::turboDeed(animator, 100, input) == PlayerDeed::TurboFull);
    CHECK(PartyMotion::turboDeed(animator, 100, input, true) == PlayerDeed::Breathe);
    input.turboAttackPressed = false;
    CHECK(PartyMotion::turboDeed(animator, 4.999f, input) == PlayerDeed::None);
    CHECK(PartyMotion::turboDeed(animator, 5, input) == PlayerDeed::Shove);
    animator.update(PlayerMotion::Stand, 1, 1.0f / 60, PlayerDeed::TurboFull);
    input.turboAttackPressed = true;
    CHECK(PartyMotion::turboDeed(animator, 100, input) == PlayerDeed::None);
    CHECK(PartyMotion::turboHeading(0, kPi * 0.5f, 1.0f / 60, 0.25f) == Approx(kPi / 48));
    CHECK(PartyMotion::turboHeading(0, 0.01f, 1.0f / 60, 0.25f) == Approx(0.01f));
    CHECK(PartyMotion::turboHeading(kPi - 0.01f, -kPi + 0.01f, 1.0f / 60, 1) ==
          Approx(kPi + 0.01f));
    CHECK(PartyMotion::turboHeading(0.2f, 1, 1.0f / 60, 0) == Approx(0.2f));
}

TEST_CASE(
    "turbo rollback checkpoints native delayed payment without releasing live strikes",
    "[netplay][prediction][rollback][turbo-rollback][weapon-modifier-rollback][speed-rollback]") {
    const f32 held = GENERATE(40.0f, 100.0f);
    const bool rapid = GENERATE(false, true);
    const bool speed = GENERATE(false, true);
    Prediction fixture{0};
    ClassStats stats;
    MoveStrike strike;
    strike.amount = 10;
    strike.startFrame = 2;
    strike.flags = MoveStrike::kHidesWeapon;
    strike.help = 1;
    stats.moveStrikes.push_back(strike);
    stats.moves.turboB = 0;
    stats.moves.turboC1 = 0;
    auto& state = *fixture.state.players[1];
    if (rapid || speed) {
        state.modifiers = PlayerWeaponModifiers{rapid ? powerup::kRapidFire : 0U, 120, speed};
    }
    TurboMeter meter;
    meter.add(held);
    TurboMove move;
    state.turbo = PlayerTurboState{meter.checkpoint(), move.checkpoint()};
    state.turboGeometry.emplace();
    state.turboGeometry->selection.reach = state.movement.radius * 2;
    auto& region = state.turboGeometry->moving.emplace();
    region.radius = 2;
    region.ticks = PlayerMeleeState::kMaxTicks;
    REQUIRE(fixture.state.valid());
    fixture.reset();
    REQUIRE(fixture.client.setPlayer(1, fixture.tree, &stats, 0));
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*state.rollback));
    s32 nativeStrikes = 0;
    for (u64 tick = 11; tick <= 20; ++tick) {
        auto command = fixture.command(tick);
        command.direction = {};
        command.magnitude = 0;
        if (tick == 11) {
            command.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
        }
        const auto input = SessionInputs::playInput(command);
        native.setAttackSpeed(rapid, speed);
        native.update(PlayerMotion::Stand, 1, 1.0f / 60,
                      PartyMotion::turboDeed(native, meter.held(), input));
        if (native.turboBegan()) {
            move.begin(native.action(), &stats, meter);
        }
        move.advance(native.action(), native.player().frame(), Vec3{0, 0, 1}, &stats, meter,
                     {.announce = [](s32) {},
                      .dim = [](f32) {},
                      .volley = [](const Vec3&) {},
                      .strike = [&](s32) { ++nativeStrikes; }});
        if (!native.turboing()) {
            meter.fill(1.0f / 60);
        }
        meter.step(1);
        fixture.client.update(&fixture.state, std::array{command});
        const auto shown = fixture.shown();
        REQUIRE(fixture.client.replayed(1));
        CHECK(fixture.client.replayed(1)->releases.empty());
        CHECK(shown.players[1]->rollback == native.checkpoint());
        REQUIRE(shown.players[1]->turbo);
        CHECK(shown.players[1]->turbo->meter == meter.checkpoint());
        CHECK(shown.players[1]->turbo->move == move.checkpoint());
        CHECK(shown.motion.players[1]->position == Vec3{0});
        CHECK(shown.projectiles.empty());
        CHECK(shown.players[1]->health == state.health);
        if (nativeStrikes == 0) {
            CHECK(meter.held() == held);
        }
    }
    CHECK(nativeStrikes == 1);
    CHECK(meter.held() == 0);
    CHECK(state.turbo->meter.held == held);
    CHECK(state.turbo->move.all.empty());
}

TEST_CASE("turbo rollback keeps native guard priority and fails closed without trusted context",
          "[netplay][prediction][rollback][turbo-rollback]") {
    Prediction fixture{0};
    const ClassStats stats;
    auto& state = *fixture.state.players[1];
    TurboMeter meter;
    meter.add(39);
    state.turbo = PlayerTurboState{meter.checkpoint(), {}};
    state.turboGeometry.emplace();
    state.turboGeometry->selection.reach = state.movement.radius * 2;
    auto& region = state.turboGeometry->moving.emplace();
    region.radius = 2;
    region.ticks = PlayerMeleeState::kMaxTicks;
    fixture.reset();
    REQUIRE(fixture.client.setPlayer(1, fixture.tree, &stats, 0));
    auto command = fixture.command(11);
    command.direction = {};
    command.magnitude = 0;
    command.pressedButtons =
        static_cast<u32>(CommandPress::TurboAttack) | static_cast<u32>(CommandPress::Defend);
    command.heldButtons = static_cast<u32>(CommandHeld::Attack);
    bool predicts = false;
    SECTION("underfunded turbo retains the guard edge") {
        predicts = true;
    }
    SECTION("missing class data never invents a timeline") {
        REQUIRE(fixture.client.setPlayer(1, fixture.tree));
    }
    SECTION("different trusted class cannot restore this actor") {
        REQUIRE(fixture.client.setPlayer(1, fixture.tree, &stats, 1));
    }
    SECTION("missing turbo admission leaves the host in control") {
        state.turbo.reset();
        state.turboGeometry.reset();
        fixture.reset();
        REQUIRE(fixture.client.setPlayer(1, fixture.tree, &stats, 0));
    }
    SECTION("charge without its separate ram certificate remains authoritative") {
        command.pressedButtons |= static_cast<u32>(CommandPress::Charge);
    }
    fixture.client.update(&fixture.state, std::array{command});
    const auto shown = fixture.shown();
    if (predicts) {
        REQUIRE(fixture.client.replayed(1));
        CHECK(shown.players[1]->animation.action ==
              static_cast<u32>(PlayerAnimator::Action::DefendRaise));
        REQUIRE(shown.players[1]->turbo);
        CHECK(shown.players[1]->turbo->meter.held == Approx(39 + 2.0f / 60));
    } else {
        CHECK(fixture.client.replayed(1) == nullptr);
        CHECK(shown.players[1]->rollback == state.rollback);
        CHECK(shown.players[1]->turbo == state.turbo);
    }
    CHECK(shown.motion.players[1]->position == Vec3{0});
    CHECK(shown.projectiles.empty());
}

TEST_CASE("available turbo metadata does not change an ordinary ranged replay",
          "[netplay][prediction][rollback][turbo-rollback]") {
    Prediction fixture;
    auto& state = *fixture.state.players[1];
    state.turbo.emplace();
    state.turboGeometry.emplace();
    state.turboGeometry->selection.reach = state.movement.radius * 2;
    auto& region = state.turboGeometry->moving.emplace();
    region.radius = 2;
    region.ticks = PlayerMeleeState::kMaxTicks;
    REQUIRE(fixture.state.valid());
    fixture.reset();
    // No trusted turbo binding is needed until a turbo command actually occurs.
    auto command = fixture.command(11);
    command.heldButtons = static_cast<u32>(CommandHeld::Attack);
    fixture.client.update(&fixture.state, std::array{command});
    const auto shown = fixture.shown();
    REQUIRE(fixture.client.replayed(1));
    CHECK(shown.players[1]->animation.action != state.animation.action);
    CHECK(shown.motion.players[1]->position.z == Approx(state.movement.speed / 60));
}

TEST_CASE("confirmed turbo recovery keeps refilling without retriggering turbo replay",
          "[netplay][prediction][rollback][turbo-rollback]") {
    Prediction fixture{0};
    const ClassStats stats;
    TurboMeter meter;
    meter.add(7);
    meter.step(1);
    const TurboMove move;
    auto& state = *fixture.state.players[1];
    state.turbo = PlayerTurboState{meter.checkpoint(), move.checkpoint()};
    state.turboGeometry.emplace();
    state.turboGeometry->selection.reach = state.movement.radius * 2;
    auto& region = state.turboGeometry->moving.emplace();
    region.radius = 2;
    region.ticks = PlayerMeleeState::kMaxTicks;
    REQUIRE(fixture.state.valid());
    fixture.reset();
    REQUIRE(fixture.client.setPlayer(1, fixture.tree, &stats, 0));
    for (u64 tick = 11; tick < 30; ++tick) {
        auto input = fixture.command(tick);
        input.direction = {};
        input.magnitude = 0;
        meter.fill(1.0f / 60);
        meter.step(1);
        fixture.client.update(&fixture.state, std::array{input});
        const auto shown = fixture.shown();
        REQUIRE(fixture.client.replayed(1));
        REQUIRE(shown.players[1]->turbo);
        CHECK(shown.players[1]->turbo->meter == meter.checkpoint());
        CHECK(shown.players[1]->turbo->move == move.checkpoint());
        CHECK(shown.players[1]->health == state.health);
        CHECK_FALSE(shown.players[1]->weaponHidden);
    }
}

TEST_CASE("ordinary weapon modifiers replay native speed and retain each release fan",
          "[netplay][prediction][rollback][weapon-modifier-rollback][speed-rollback]") {
    const u32 flags = GENERATE(powerup::kRapidFire, powerup::kThreeWayShot, powerup::kFiveWayShot,
                               powerup::kRapidFire | powerup::kThreeWayShot,
                               powerup::kRapidFire | powerup::kFiveWayShot,
                               powerup::kThreeWayShot | powerup::kFiveWayShot);
    const bool strong = GENERATE(false, true);
    const bool strafe = GENERATE(false, true);
    const bool speed = GENERATE(false, true);
    Prediction fixture{0};
    for (auto& sequence : fixture.tree.sequences) {
        sequence.frames = 3;
    }
    auto& state = *fixture.state.players[1];
    state.modifiers = PlayerWeaponModifiers{flags, PlayerWeaponModifiers::kMaxTicks, speed};
    REQUIRE_FALSE(state.rollback->rapid); // pickup after the preceding native motion step
    REQUIRE(fixture.state.valid());
    fixture.reset();
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*state.rollback));
    PowerupEffects worn;
    worn.weapon = flags;
    std::vector<u64> releases;
    for (u64 tick = 11; tick <= 34; ++tick) {
        auto input = fixture.command(tick);
        input.direction = strafe ? Vec2{1, 0} : Vec2{0};
        input.magnitude = strafe ? 1.0f : 0.0f;
        input.heldButtons =
            static_cast<u32>(strong ? CommandHeld::StrongAttack : CommandHeld::Attack);
        if (strafe) {
            input.heldButtons |= static_cast<u32>(CommandHeld::Strafe);
        }
        native.setStrafe(strafe ? StrafeWay::Right : StrafeWay::None);
        native.setAttackSpeed((flags & powerup::kRapidFire) != 0, speed);
        native.update(strafe ? PlayerMotion::Run : PlayerMotion::Stand, 1, 1.0f / 60,
                      strong ? PlayerDeed::StrongAttack : PlayerDeed::Attack);
        if (native.released() || native.strongReleased()) {
            releases.push_back(tick);
        }
        fixture.client.update(&fixture.state, std::array{input});
        const auto shown = fixture.shown();
        const auto* replay = fixture.client.replayed(1);
        REQUIRE(replay);
        CHECK(shown.players[1]->rollback == native.checkpoint());
        REQUIRE(replay->releases.size() == releases.size());
        for (usize i = 0; i < releases.size(); ++i) {
            CHECK(replay->releases[i].tick == releases[i]);
            CHECK(replay->releases[i].shots == worn.shots());
            CHECK(replay->releases[i].strong == strong);
        }
        CHECK(shown.players[1]->health == state.health);
    }
    CHECK_FALSE(releases.empty());
    CHECK_FALSE(state.rollback->rapid);
}

TEST_CASE("item expiry changes current flags without replacing native latched sequence speed",
          "[netplay][prediction][rollback][weapon-modifier-rollback][speed-rollback]") {
    const bool speed = GENERATE(false, true);
    Prediction fixture{0};
    for (auto& sequence : fixture.tree.sequences) {
        sequence.frames = 3;
    }
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    native.setCharacter(0);
    native.setAttackSpeed(!speed, speed);
    native.update(PlayerMotion::Stand, 1, 1.0f / 60, PlayerDeed::StrongAttack);
    auto& state = *fixture.state.players[1];
    captureAction(state, native);
    REQUIRE(state.rollback->rapid == !speed);
    REQUIRE(state.rollback->speed == speed);
    REQUIRE_FALSE(state.modifiers); // it expires before the next native motion update
    fixture.reset();
    for (u64 tick = 11; tick <= 22; ++tick) {
        auto input = fixture.command(tick);
        input.direction = {};
        input.magnitude = 0;
        native.setAttackSpeed(false, false);
        native.update(PlayerMotion::Stand, 1, 1.0f / 60);
        fixture.client.update(&fixture.state, std::array{input});
        const auto shown = fixture.shown();
        REQUIRE(fixture.client.replayed(1));
        CHECK(shown.players[1]->rollback == native.checkpoint());
        CHECK_FALSE(shown.players[1]->rollback->rapid);
        CHECK_FALSE(shown.players[1]->rollback->speed);
    }
    CHECK(state.rollback->rapid == !speed);
    CHECK(state.rollback->speed == speed);
}

TEST_CASE("weapon modifier expiry and selector boundaries discard the complete replay",
          "[netplay][prediction][rollback][weapon-modifier-rollback]") {
    const u32 budget = GENERATE(0U, 2U);
    Prediction fixture{0};
    auto& state = *fixture.state.players[1];
    state.modifiers = PlayerWeaponModifiers{powerup::kRapidFire | powerup::kFiveWayShot, budget};
    REQUIRE(fixture.state.valid());
    fixture.reset();
    for (u32 step = 1; step <= budget + 1; ++step) {
        auto input = fixture.command(10 + step);
        input.direction = {};
        input.magnitude = 0;
        input.heldButtons = static_cast<u32>(CommandHeld::Attack);
        fixture.client.update(&fixture.state, std::array{input});
        const auto shown = fixture.shown();
        if (step <= budget) {
            REQUIRE(fixture.client.replayed(1));
            CHECK(shown.players[1]->rollback->rapid);
        } else {
            CHECK(fixture.client.replayed(1) == nullptr);
            CHECK(shown.players[1]->rollback == state.rollback);
            CHECK(shown.motion.players[1]->position == fixture.state.motion.players[1]->position);
        }
    }
    // A new authority packet revokes the modifier, including cached speculative
    // fan releases. No selector change is ever applied to a live inventory.
    fixture.state.motion.tick = 20;
    state.modifiers.reset();
    auto next = fixture.command(21);
    next.direction = {};
    next.magnitude = 0;
    next.pressedButtons = static_cast<u32>(CommandPress::SelectorUp);
    fixture.client.update(&fixture.state, std::array{next});
    CHECK(fixture.client.replayed(1) == nullptr);
    CHECK_FALSE(fixture.shown().players[1]->modifiers);
}

TEST_CASE("rapid and spread state does not leak between locally owned seats",
          "[netplay][prediction][rollback][weapon-modifier-rollback][speed-rollback]") {
    Prediction fixture{0};
    for (auto& sequence : fixture.tree.sequences) {
        sequence.frames = 3;
    }
    fixture.state.players[1]->modifiers =
        PlayerWeaponModifiers{powerup::kRapidFire | powerup::kFiveWayShot, 120, true};
    fixture.state.players[2]->modifiers = PlayerWeaponModifiers{powerup::kThreeWayShot, 120};
    REQUIRE(fixture.state.valid());
    fixture.reset();
    for (u64 tick = 11; tick <= 34; ++tick) {
        auto first = fixture.command(tick, 1);
        first.direction = {};
        first.magnitude = 0;
        first.heldButtons = static_cast<u32>(CommandHeld::Attack);
        auto second = first;
        second.seat = 2;
        second.grant = fixture.context.grants[2];
        fixture.client.update(&fixture.state, std::array{first, second});
    }
    const auto shown = fixture.shown();
    CHECK(shown.players[1]->rollback->rapid);
    CHECK_FALSE(shown.players[2]->rollback->rapid);
    CHECK(shown.players[1]->rollback->speed);
    CHECK_FALSE(shown.players[2]->rollback->speed);
    for (const auto seat : {1U, 2U}) {
        const auto* replay = fixture.client.replayed(static_cast<u8>(seat));
        REQUIRE(replay);
        REQUIRE_FALSE(replay->releases.empty());
        for (const auto& release : replay->releases) {
            CHECK(release.shots == (seat == 1 ? 5 : 3));
        }
    }
    CHECK(fixture.state.players[1]->rollback == fixture.state.players[2]->rollback);
}

TEST_CASE("reaction replay honors the same weapon modifier certificate",
          "[netplay][prediction][rollback][weapon-modifier-rollback][speed-rollback]") {
    const bool speed = GENERATE(false, true);
    Prediction fixture{0};
    auto& state = *fixture.state.players[1];
    state.knockback.emplace();
    state.knockbackTicks = PlayerCombatState::kKnockbackTicks;
    state.reaction = PlayerDeed::Flinch;
    state.modifiers = PlayerWeaponModifiers{powerup::kRapidFire, 1, speed};
    REQUIRE(fixture.state.valid());
    fixture.reset();
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*state.rollback));
    native.setAttackSpeed(true, speed);
    native.update(PlayerMotion::Stand, 1, 1.0f / 60, PlayerDeed::Flinch);
    auto input = fixture.command(11);
    input.direction = {};
    input.magnitude = 0;
    fixture.client.update(&fixture.state, std::array{input});
    REQUIRE(fixture.client.replayed(1));
    CHECK(fixture.shown().players[1]->rollback == native.checkpoint());
    ++input.tick;
    fixture.client.update(&fixture.state, std::array{input});
    CHECK(fixture.client.replayed(1) == nullptr);
    CHECK(fixture.shown().players[1]->rollback == state.rollback);
    CHECK(fixture.shown().players[1]->reaction == PlayerDeed::Flinch);
}

TEST_CASE("a fresh inventory certificate replaces cached Speed flags without changing native pace",
          "[netplay][prediction][rollback][weapon-modifier-rollback][speed-rollback]") {
    Prediction fixture{0};
    auto& state = *fixture.state.players[1];
    state.rollback->speed = true;
    state.modifiers = PlayerWeaponModifiers{powerup::kRapidFire, 120};
    REQUIRE(fixture.state.valid());
    fixture.reset();
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*state.rollback));
    native.setAttackSpeed(true, false);
    native.update(PlayerMotion::Run, 1, 1.0f / 60, PlayerDeed::Attack);
    auto input = fixture.command(11);
    input.heldButtons = static_cast<u32>(CommandHeld::Attack);
    fixture.client.update(&fixture.state, std::array{input});
    REQUIRE(fixture.client.replayed(1));
    CHECK(fixture.shown().players[1]->rollback == native.checkpoint());
    CHECK(fixture.shown().motion.players[1]->position.z == Approx(state.movement.speed / 60));
}

TEST_CASE("Speed rollback uses the certified clamped native pace without adding it twice",
          "[netplay][prediction][rollback][speed-rollback]") {
    const f32 bonus = GENERATE(0.0f, 2.0f, 20.0f);
    const auto requested = GENERATE(PlayerDeed::None, PlayerDeed::Attack, PlayerDeed::StrongAttack,
                                    PlayerDeed::Defend);
    const bool strafe = GENERATE(false, true);
    Prediction fixture{0};
    for (auto& sequence : fixture.tree.sequences) {
        sequence.frames = 6;
    }
    auto& state = *fixture.state.players[1];
    auto& envelope = state.movement;
    PlayerActor actor;
    actor.restoreMotion(Vec3{0}, 0, 6.25f, envelope.radius, envelope.height, envelope.followHeight);
    actor.setPaceBonus(bonus);
    envelope.speed = actor.speed();
    state.modifiers = PlayerWeaponModifiers{0, 120, true};
    REQUIRE_FALSE(state.rollback->speed); // pickup following the last native motion step
    REQUIRE(fixture.state.valid());
    fixture.reset();
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*state.rollback));
    for (u64 tick = 11; tick <= 34; ++tick) {
        CAPTURE(bonus, requested, strafe, tick);
        auto input = fixture.command(tick);
        input.direction = strafe ? Vec2{1, 0} : Vec2{0, 1};
        if (strafe) {
            input.heldButtons = static_cast<u32>(CommandHeld::Strafe);
        }
        if (requested == PlayerDeed::Attack) {
            input.heldButtons |= static_cast<u32>(CommandHeld::Attack);
        } else if (requested == PlayerDeed::StrongAttack) {
            input.heldButtons |= static_cast<u32>(CommandHeld::StrongAttack);
        } else if (requested == PlayerDeed::Defend && tick == 11) {
            input.pressedButtons = static_cast<u32>(CommandPress::Defend);
        }
        const PlayerDeed deed =
            requested == PlayerDeed::Defend && tick != 11 ? PlayerDeed::None : requested;
        const auto move = SessionInputs::playInput(input).move;
        const f32 pace = PartyMotion::actionPace(&native, deed);
        native.setStrafe(strafe ? StrafeWay::Right : StrafeWay::None);
        actor.update(PartyMotion::attackMove(actor, &native, move, deed, 0, 1.0f / 60, false), 0,
                     1.0f / 60, &fixture.floor, pace, strafe);
        actor.fall(1.0f / 60, fixture.floor);
        native.setAttackSpeed(false, true);
        native.update(PlayerMotion::Run, 1, 1.0f / 60, deed);
        fixture.client.update(&fixture.state, std::array{input});
        const auto shown = fixture.shown();
        REQUIRE(fixture.client.replayed(1));
        CHECK(shown.players[1]->rollback == native.checkpoint());
        CHECK(shown.motion.players[1]->position == actor.position());
        CHECK(shown.motion.players[1]->yaw == Approx(actor.yaw()));
        CHECK(shown.players[1]->health == state.health);
    }
    CHECK_FALSE(state.rollback->speed);
}

TEST_CASE("Speed horizon and inventory controls atomically revoke replay",
          "[netplay][prediction][rollback][speed-rollback]") {
    Prediction fixture{0};
    auto& state = *fixture.state.players[1];
    state.movement.speed = 8;
    state.modifiers = PlayerWeaponModifiers{0, 1, true};
    REQUIRE(fixture.state.valid());
    fixture.reset();
    auto input = fixture.command(11);
    fixture.client.update(&fixture.state, std::array{input});
    REQUIRE(fixture.client.replayed(1));
    CHECK(fixture.shown().motion.players[1]->position.z == Approx(8.0f / 60));
    ++input.tick;
    fixture.client.update(&fixture.state, std::array{input});
    CHECK(fixture.client.replayed(1) == nullptr);
    CHECK(fixture.shown().motion.players[1]->position == Vec3{0});
    CHECK(fixture.shown().players[1]->rollback == state.rollback);

    state.modifiers->ticks = 120;
    fixture.reset();
    input.tick = 11;
    const auto unsupported = GENERATE(0, 1, 2);
    if (unsupported == 2) {
        input.heldButtons = static_cast<u32>(CommandHeld::ThrowPotion);
    } else {
        input.pressedButtons =
            static_cast<u32>(unsupported == 0 ? CommandPress::SelectorUp : CommandPress::UsePotion);
    }
    fixture.client.update(&fixture.state, std::array{input});
    CHECK(fixture.client.replayed(1) == nullptr);
    CHECK(fixture.shown().players[1]->rollback == state.rollback);
}

TEST_CASE("uncertified Speed combinations retain newest authority without speculative releases",
          "[netplay][prediction][rollback][speed-rollback]") {
    Prediction fixture{0};
    auto input = fixture.command(11);
    fixture.client.update(&fixture.state, std::array{input});
    REQUIRE(fixture.client.replayed(1));
    auto& state = *fixture.state.players[1];
    fixture.state.motion.tick = 11;
    fixture.state.motion.players[1]->position = Vec3{-2, 0, 3};
    state.movement.speed = 8;
    state.movement.enabled = false;
    state.movement.ranged = false;
    state.movement.current = true;
    state.rollback->speed = true;
    state.modifiers.reset();
    REQUIRE(fixture.state.valid());
    ++input.tick;
    input.heldButtons = static_cast<u32>(CommandHeld::Attack);
    fixture.client.update(&fixture.state, std::array{input});
    const auto shown = fixture.shown();
    CHECK(fixture.client.replayed(1) == nullptr);
    CHECK(shown.players[1]->rollback == state.rollback);
    CHECK(shown.motion.players[1]->position == fixture.state.motion.players[1]->position);
    CHECK(shown.players[1]->health == state.health);
    CHECK(shown.projectiles.empty());
}

TEST_CASE("locomotion alone does not authorize an item-sensitive guard",
          "[netplay][prediction][rollback][weapon-modifier-rollback]") {
    Prediction fixture{0};
    auto& state = *fixture.state.players[1];
    state.movement.ranged = false;
    state.rollback->shielded = true;
    fixture.reset();
    auto input = fixture.command(11);
    fixture.client.update(&fixture.state, std::array{input});
    REQUIRE(fixture.client.replayed(1));
    CHECK(fixture.shown().motion.players[1]->position.z > 0);
    ++input.tick;
    input.pressedButtons = static_cast<u32>(CommandPress::Defend);
    fixture.client.update(&fixture.state, std::array{input});
    CHECK(fixture.client.replayed(1) == nullptr);
    CHECK(fixture.shown().players[1]->rollback == state.rollback);
    CHECK(fixture.shown().motion.players[1]->position == Vec3{0});
}

TEST_CASE("expired shield flags are refreshed before an admitted ordinary guard",
          "[netplay][prediction][rollback][weapon-modifier-rollback]") {
    Prediction fixture{0};
    auto& state = *fixture.state.players[1];
    state.rollback->shielded = true;
    fixture.reset();
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*state.rollback));
    native.setShielded(false);
    native.update(PlayerMotion::Stand, 1, 1.0f / 60, PlayerDeed::Defend);
    auto input = fixture.command(11);
    input.direction = {};
    input.magnitude = 0;
    input.pressedButtons = static_cast<u32>(CommandPress::Defend);
    fixture.client.update(&fixture.state, std::array{input});
    REQUIRE(fixture.client.replayed(1));
    CHECK(fixture.shown().players[1]->rollback == native.checkpoint());
}

TEST_CASE("charge replay keeps native first-frame pace and released-button travel",
          "[netplay][prediction][rollback][shove-rollback][speed-rollback]") {
    const bool held = GENERATE(false, true);
    const bool speed = GENERATE(false, true);
    Prediction fixture{0};
    const ClassStats stats;
    auto& state = *fixture.state.players[1];
    if (speed) {
        state.modifiers = PlayerWeaponModifiers{0, 120, true};
        state.movement.speed = 8;
    }
    TurboMeter meter;
    meter.add(5);
    state.turbo = PlayerTurboState{meter.checkpoint(), {}};
    state.turboGeometry.emplace();
    state.turboGeometry->selection.reach = state.movement.radius * 2;
    auto& region = state.turboGeometry->moving.emplace();
    region.radius = 3;
    region.ticks = PlayerMeleeState::kMaxTicks;
    state.shove = true;
    REQUIRE(fixture.state.valid());
    fixture.reset();
    REQUIRE(fixture.client.setPlayer(1, fixture.tree, &stats, 0));
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*state.rollback));
    for (u64 tick = 11; tick <= 22; ++tick) {
        auto input = fixture.command(tick);
        input.direction = {};
        input.magnitude = 0;
        input.heldButtons = held ? static_cast<u32>(CommandHeld::Turbo) : 0;
        if (tick == 11) {
            input.pressedButtons = static_cast<u32>(CommandPress::Charge);
        }
        native.setAttackSpeed(false, speed);
        native.update(
            PlayerMotion::Stand, 1, 1.0f / 60,
            PartyMotion::turboDeed(native, meter.held(), SessionInputs::playInput(input)));
        REQUIRE(native.shoving());
        meter.drain(1.0f / 60);
        meter.step(1);
        fixture.client.update(&fixture.state, std::array{input});
        const auto shown = fixture.shown();
        REQUIRE(fixture.client.replayed(1));
        CHECK(shown.players[1]->rollback == native.checkpoint());
        REQUIRE(shown.players[1]->turbo);
        CHECK(shown.players[1]->turbo->meter == meter.checkpoint());
        CHECK(shown.players[1]->turbo->move.all.empty());
        // The initiating neutral tick does not move; subsequent ticks rush on
        // at 1.5 times walking speed even though the button and stick are up.
        CHECK(shown.motion.players[1]->position.z ==
              Approx(static_cast<f32>(tick - 11) * state.movement.speed *
                     PlayerAnimator::kChargePace / 60));
        CHECK(shown.players[1]->health == state.health);
        CHECK(fixture.client.replayed(1)->releases.empty());
        CHECK(shown.projectiles.empty());
    }
    CHECK(state.turbo->meter.held == 5);
}

TEST_CASE("confirmed charge drains to zero then completes before native refill",
          "[netplay][prediction][rollback][shove-rollback]") {
    Prediction fixture{0};
    const ClassStats stats;
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    native.setCharacter(0);
    auto& sequence = fixture.tree.sequences[native.sequenceOf(PlayerAnimator::Action::Shove)];
    sequence.frames = 4;
    native.update(PlayerMotion::Stand, 0, 0, PlayerDeed::Shove);
    REQUIRE(native.shoving());
    auto& state = *fixture.state.players[1];
    captureAction(state, native);
    TurboMeter meter;
    meter.add(0.25f);
    state.turbo = PlayerTurboState{meter.checkpoint(), {}};
    state.turboGeometry.emplace();
    state.turboGeometry->selection.reach = state.movement.radius * 2;
    auto& region = state.turboGeometry->moving.emplace();
    region.radius = 2;
    region.ticks = PlayerMeleeState::kMaxTicks;
    state.shove = true;
    REQUIRE(fixture.state.valid());
    fixture.reset();
    REQUIRE(fixture.client.setPlayer(1, fixture.tree, &stats, 0));
    f32 travelled = 0;
    bool emptyWhileCharging = false;
    bool recovered = false;
    for (u64 tick = 11; tick <= 22; ++tick) {
        auto input = fixture.command(tick);
        input.direction = {};
        input.magnitude = 0;
        if (native.shoving()) {
            travelled += state.movement.speed * PlayerAnimator::kChargePace / 60;
        }
        native.update(PlayerMotion::Stand, 1, 1.0f / 60);
        if (native.shoving()) {
            meter.drain(1.0f / 60);
            emptyWhileCharging = emptyWhileCharging || meter.held() == 0;
        } else {
            meter.fill(1.0f / 60);
            recovered = true;
        }
        meter.step(1);
        fixture.client.update(&fixture.state, std::array{input});
        const auto shown = fixture.shown();
        REQUIRE(fixture.client.replayed(1));
        CHECK(shown.players[1]->rollback == native.checkpoint());
        REQUIRE(shown.players[1]->turbo);
        CHECK(shown.players[1]->turbo->meter == meter.checkpoint());
        CHECK(shown.motion.players[1]->position.z == Approx(travelled));
        CHECK(shown.players[1]->health == state.health);
        CHECK(fixture.client.replayed(1)->releases.empty());
    }
    CHECK(emptyWhileCharging);
    CHECK(recovered);
    CHECK(meter.held() > 0);
}

TEST_CASE("charge refuses contact inside the ram margin and missing authority atomically",
          "[netplay][prediction][rollback][shove-rollback]") {
    Prediction fixture{0};
    const ClassStats stats;
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    native.setCharacter(0);
    native.update(PlayerMotion::Stand, 0, 0, PlayerDeed::Shove);
    auto& state = *fixture.state.players[1];
    captureAction(state, native);
    state.turbo.emplace();
    state.turbo->meter.held = 10;
    state.turboGeometry.emplace();
    state.turboGeometry->selection.reach = state.movement.radius * 2;
    auto& region = state.turboGeometry->moving.emplace();
    region.radius = 2;
    region.ticks = PlayerMeleeState::kMaxTicks;
    state.shove = true;
    bool classBound = true;
    SECTION("host has not certified raw ram fixtures") {
        state.shove = false;
    }
    SECTION("native class data unavailable") {
        classBound = false;
    }
    SECTION("enemy inside ram range but outside body correction") {
        MeleeGeometry::Target target;
        target.id = 0;
        target.base = {0, 0, 1.375f};
        target.radius = 0.25f;
        target.height = state.movement.height;
        target.acquisition.point = target.base;
        region.targets.push_back(target);
        const std::array bodies{MeleeGeometry::body(target)};
        const Vec3 next{0, 0, state.movement.speed * PlayerAnimator::kChargePace / 60};
        REQUIRE(BodyCollision::resolve(Vec3{0}, next, state.movement.radius, state.movement.height,
                                       bodies) == next);
    }
    SECTION("obstacle inside ram range but outside body correction") {
        Obstacle obstacle;
        obstacle.centre = {0, 0, 1.375f};
        obstacle.halfAcross = 0.25f;
        obstacle.halfAlong = 0.25f;
        obstacle.height = state.movement.height;
        region.obstacles.push_back(obstacle);
        const Vec3 next{0, 0, state.movement.speed * PlayerAnimator::kChargePace / 60};
        REQUIRE(obstacle.pushOut(next, state.movement.radius) == next);
    }
    REQUIRE(fixture.state.valid());
    fixture.reset();
    if (classBound) {
        REQUIRE(fixture.client.setPlayer(1, fixture.tree, &stats, 0));
    }
    auto input = fixture.command(11);
    input.direction = {};
    input.magnitude = 0;
    fixture.client.update(&fixture.state, std::array{input});
    const auto shown = fixture.shown();
    CHECK(fixture.client.replayed(1) == nullptr);
    CHECK(shown.players[1]->rollback == state.rollback);
    CHECK(shown.players[1]->turbo == state.turbo);
    CHECK(shown.motion.players[1]->position == fixture.state.motion.players[1]->position);
    CHECK(shown.projectiles.empty());
}

TEST_CASE("moving melee fallback respects fixtures' separate endpoint height policy",
          "[netplay][prediction][rollback][moving-melee]") {
    Prediction fixture;
    auto& state = *fixture.state.players[1];
    state.movement.ranged = false;
    state.melee.emplace();
    state.melee->selection.reach = state.movement.radius * 2;
    auto& region = state.melee->moving.emplace();
    region.radius = 2;
    region.ticks = PlayerMeleeState::kMaxTicks;
    Obstacle elevated;
    elevated.centre = {0, 8, 0};
    elevated.halfAcross = 1;
    elevated.halfAlong = 1;
    elevated.height = 10;
    region.obstacles.push_back(elevated);
    const Vec3 proposed{0, 0, state.movement.speed / 60};
    // The full-height movement overlap misses this elevated fixture, but the
    // authored fixture endpoint test still expels a body beneath its tall box.
    REQUIRE(BodyCollision::resolveItems(Vec3{0}, proposed, state.movement.radius,
                                        state.movement.height, region.obstacles) == proposed);
    REQUIRE(elevated.pushOut(proposed, state.movement.radius) != proposed);
    REQUIRE(fixture.state.valid());
    fixture.reset();
    fixture.client.update(&fixture.state, std::array{fixture.command(11)});
    CHECK(fixture.shown().motion.players[1]->position == Vec3{0});
    CHECK(fixture.client.replayed(1) == nullptr);
}

TEST_CASE("moving melee rechecks fixture endpoints after native floor height changes",
          "[netplay][prediction][rollback][moving-melee][floor-riding]") {
    Prediction fixture;
    auto& state = *fixture.state.players[1];
    state.movement.ranged = false;
    state.melee.emplace();
    state.melee->selection.reach = state.movement.radius * 2;
    auto& region = state.melee->moving.emplace();
    region.radius = 2;
    region.ticks = PlayerMeleeState::kMaxTicks;
    constexpr s32 kUpperFloor = 7;
    const std::array<CollisionTriangle, 2> upper{
        CollisionTriangle{{0, 1, 0}, {Vec3{-5, 1, -5}, Vec3{5, 1, -5}, Vec3{5, 1, 5}}, kUpperFloor},
        CollisionTriangle{
            {0, 1, 0}, {Vec3{-5, 1, -5}, Vec3{5, 1, 5}, Vec3{-5, 1, 5}}, kUpperFloor}};
    fixture.floor.append(upper);
    Obstacle elevated;
    elevated.centre = {0, 5.5f, 0};
    elevated.halfAcross = 1;
    elevated.halfAlong = 1;
    elevated.height = 5;
    region.obstacles.push_back(elevated);

    // Party motion performs body correction and support ownership before fall.
    // The latter probes farther above the feet and can change the height before
    // LevelFixtures performs its separate endpoint push-out.
    std::array<PlayerRuntime, 1> native;
    auto& actor = native[0].actor;
    actor.restoreMotion(Vec3{0}, 0, state.movement.speed, state.movement.radius,
                        state.movement.height, state.movement.followHeight);
    actor.update({}, 0, 1.0f / 60, &fixture.floor);
    REQUIRE(BodyCollision::resolveItems(Vec3{0}, actor.position(), actor.radius(), actor.height(),
                                        region.obstacles) == actor.position());
    REQUIRE(elevated.pushOut(actor.position(), actor.radius()) == actor.position());
    FloorRiding::land(native, 0, Vec3{0}, fixture.floor);
    REQUIRE(native[0].floor.object == 0);
    actor.fall(1.0f / 60, fixture.floor);
    REQUIRE(actor.position() == Vec3{0, 1, 0});
    REQUIRE(glm::distance(actor.position(), region.origin) < region.radius);
    const auto support = FloorRiding::support(actor, fixture.floor);
    REQUIRE(support);
    REQUIRE(support->object == kUpperFloor);
    REQUIRE(elevated.pushOut(actor.position(), actor.radius()) != actor.position());

    REQUIRE(fixture.state.valid());
    fixture.reset();
    auto command = fixture.command(11);
    command.direction = {};
    command.magnitude = 0;
    command.heldButtons = static_cast<u32>(CommandHeld::Attack);
    fixture.client.update(&fixture.state, std::array{command});
    const auto shown = fixture.shown();
    CHECK(shown.motion.players[1]->position == Vec3{0});
    CHECK(shown.players[1]->rollback == state.rollback);
    CHECK(fixture.client.replayed(1) == nullptr);
}

TEST_CASE("moving melee uses native support ownership rather than a narrow center probe",
          "[netplay][prediction][rollback][moving-melee][floor-riding]") {
    Prediction fixture;
    auto& state = *fixture.state.players[1];
    state.movement.ranged = false;
    state.melee.emplace();
    state.melee->selection.reach = state.movement.radius * 2;
    auto& region = state.melee->moving.emplace();
    region.radius = 2;
    region.ticks = PlayerMeleeState::kMaxTicks;
    constexpr s32 kLift = 7;
    constexpr u32 kFloor = 4;
    const std::array<CollisionTriangle, 2> ledge{
        CollisionTriangle{{0, 1, 0},
                          {Vec3{-5, 0.4f, -5}, Vec3{-0.02f, 0.4f, -5}, Vec3{-0.02f, 0.4f, 5}},
                          kLift,
                          kFloor | FloorRiding::kMoving},
        CollisionTriangle{{0, 1, 0},
                          {Vec3{-5, 0.4f, -5}, Vec3{-0.02f, 0.4f, 5}, Vec3{-5, 0.4f, 5}},
                          kLift,
                          kFloor | FloorRiding::kMoving}};
    fixture.floor.append(ledge);
    fixture.floor.setMovingObjects(std::array<s32, 1>{kLift});
    fixture.floor.setObjectTransform(kLift, Mat4{1});
    std::array<PlayerRuntime, 1> native;
    native[0].actor.restoreMotion(Vec3{0}, 0, state.movement.speed, state.movement.radius,
                                  state.movement.height, state.movement.followHeight);
    const auto center = fixture.floor.floorAt(Vec3{0}, 0.25f, 0.25f);
    REQUIRE(center);
    CHECK(center->object == 0);
    const auto support = FloorRiding::support(native[0].actor, fixture.floor);
    REQUIRE(support);
    CHECK(support->object == kLift);
    FloorRiding::land(native, 0, Vec3{0}, fixture.floor);
    CHECK(native[0].floor.object == kLift);
    fixture.reset();
    fixture.client.update(&fixture.state, std::array{fixture.command(11)});
    CHECK(fixture.client.replayed(1) == nullptr);
    CHECK(fixture.shown().motion.players[1]->position == Vec3{0});
}

TEST_CASE("an idle native impulse checkpoint does not disable movement attacks or guard",
          "[netplay][prediction][rollback][reaction]") {
    const auto deed = GENERATE(PlayerDeed::None, PlayerDeed::Attack, PlayerDeed::StrongAttack,
                               PlayerDeed::Defend);
    Prediction fixture;
    Knockback nativeImpulse;
    nativeImpulse.step(1.0f / 60, fixture.state.players[1]->movement.speed);
    fixture.state.players[1]->knockback = nativeImpulse.checkpoint();
    REQUIRE(fixture.state.players[1]->knockback->frameSeconds > 0);
    fixture.reset();
    auto command = fixture.command(11);
    if (deed == PlayerDeed::Defend) {
        command.pressedButtons = static_cast<u32>(CommandPress::Defend);
    } else if (deed != PlayerDeed::None) {
        command.heldButtons = static_cast<u32>(
            deed == PlayerDeed::Attack ? CommandHeld::Attack : CommandHeld::StrongAttack);
    }
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*fixture.state.players[1]->rollback));
    const f32 firstPace = deed == PlayerDeed::StrongAttack
                              ? std::min(native.moveScale(), PlayerAnimator::kStrongThrowPace)
                              : native.moveScale();
    native.update(PlayerMotion::Run, 1, 1.0f / 60, deed);
    nativeImpulse.step(1.0f / 60, fixture.state.players[1]->movement.speed);
    nativeImpulse.endFrame();
    fixture.client.update(&fixture.state, std::array{command});
    const auto shown = fixture.shown();
    CHECK(shown.motion.players[1]->position.z == Approx(5.0f / 60 * firstPace));
    CHECK(shown.players[1]->rollback == native.checkpoint());
    CHECK(shown.players[1]->knockback == nativeImpulse.checkpoint());
    CHECK(shown.players[1]->knockback->frameSeconds == 0);
}

TEST_CASE("stationary native melee checkpoints rewind and replay without dispatching strikes",
          "[netplay][prediction][rollback][melee]") {
    enum class Attack : u8 { Plain, Strong, Buffered };
    const auto attack = GENERATE(Attack::Plain, Attack::Strong, Attack::Buffered);
    const s32 draws = GENERATE(0, 1, 4);
    Prediction fixture;
    for (auto& sequence : fixture.tree.sequences) {
        sequence.frames = 4;
    }
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    auto& player = *fixture.state.players[1];
    captureAction(player, native);
    player.movement.ranged = false;
    player.melee = PlayerMeleeState{};
    player.melee->selection.feet = Vec3{0};
    player.melee->selection.reach = player.movement.radius * 2;
    player.melee->selection.range = 30;
    player.melee->selection.facingDot = 0.99f;
    player.melee->selection.targets.push_back({Vec3{0, 0, 3}, Vec3{0, 0, 3}, 2, 2, false});
    player.melee->assistedAim = Vec3{0, 2.5f, 3};
    REQUIRE(fixture.state.valid());
    fixture.reset();
    usize blows = 0;
    for (u64 tick = 11; tick < 83; ++tick) {
        auto command = fixture.command(tick);
        command.direction = Vec2{0};
        command.magnitude = 0;
        const bool held = tick < 55;
        const bool strong = attack == Attack::Strong || (attack == Attack::Buffered && tick == 15);
        if (held) {
            command.heldButtons =
                static_cast<u32>(strong ? CommandHeld::StrongAttack : CommandHeld::Attack);
        }
        native.setMelee({MeleeRange::Swing, false, 0});
        auto deed = PlayerDeed::None;
        if (held) {
            deed = strong ? PlayerDeed::MeleeSlow : PlayerDeed::Melee;
        }
        native.update(PlayerMotion::Stand, 1, 1.0f / 60, deed);
        blows += native.meleeStruck() ? 1U : 0U;
        REQUIRE_FALSE(native.lunging());
        fixture.client.update(&fixture.state, std::array{command});
        const auto shown = fixture.shown();
        REQUIRE(shown.players[1]->rollback);
        CHECK(*shown.players[1]->rollback == native.checkpoint());
        CHECK(shown.motion.players[1]->position == Vec3{0});
        CHECK(shown.players[1]->health == 100);
        CHECK(shown.projectiles.empty());
        const auto* replay = fixture.client.replayed(1);
        REQUIRE(replay);
        CHECK(replay->releases.empty());
        for (s32 draw = 0; draw < draws; ++draw) {
            const auto frame = fixture.shown(static_cast<f32>(draw + 1) / static_cast<f32>(draws));
            CHECK(frame.motion.players[1]->position == Vec3{0});
            CHECK(frame.players[1]->health == 100);
            CHECK(frame.projectiles.empty());
        }
        if (tick % 8 == 0) {
            // Move the authoritative checkpoint into this same action, including
            // chain/latch/recovery state, then replay the retained suffix from it.
            fixture.state.motion.tick = tick;
            captureAction(*fixture.state.players[1], native);
            fixture.state.players[1]->rollbackInput = command;
            fixture.client.update(&fixture.state, {});
            CHECK(*fixture.shown().players[1]->rollback == native.checkpoint());
        }
    }
    CHECK(blows > 0);
}

TEST_CASE("confirmed knockback keeps native slide-before-kick timing through rewind",
          "[netplay][prediction][rollback][reaction]") {
    const auto reaction = GENERATE(PlayerDeed::Flinch, PlayerDeed::Reel, PlayerDeed::Spike);
    Prediction fixture;
    auto& state = *fixture.state.players[1];
    state.movement.ranged = false;
    state.reaction = reaction;
    state.knockbackTicks = PlayerCombatState::kKnockbackTicks;
    Knockback nativeImpulse;
    nativeImpulse.queue({-1, 0, 0.25f}, Knockback::kKnockBack, 5);
    state.knockback = nativeImpulse.checkpoint();
    fixture.reset();
    PlayerActor nativeBody;
    nativeBody.restoreMotion(Vec3{0}, 0, state.movement.speed, state.movement.radius,
                             state.movement.height, state.movement.followHeight);
    PlayerAnimator nativeAnimation;
    REQUIRE(nativeAnimation.bind(fixture.tree, false));
    REQUIRE(nativeAnimation.restore(*state.rollback));
    for (u64 tick = 11; tick <= 34; ++tick) {
        CAPTURE(reaction, tick);
        const f32 seconds = 1.0f / 60;
        nativeBody.slide(nativeImpulse.step(seconds, nativeBody.speed()), &fixture.floor);
        if (const auto heading = nativeImpulse.kick(nativeBody.yaw(), false)) {
            nativeBody.turnTo(*heading);
        }
        nativeBody.update({}, 0, seconds, &fixture.floor, nativeAnimation.moveScale());
        nativeBody.fall(seconds, fixture.floor);
        nativeAnimation.setStrafe(StrafeWay::None);
        nativeAnimation.setAttackSpeed(false, false);
        nativeAnimation.setShielded(false);
        nativeAnimation.setPushed(false);
        nativeAnimation.setCombo(-1, false);
        nativeAnimation.update(PlayerMotion::Stand, 1, seconds,
                               tick == 11 ? reaction : PlayerDeed::None);
        nativeImpulse.endFrame();
        auto command = fixture.command(tick);
        command.aimDirection = Vec3{1, 0, 0};
        command.heldButtons = static_cast<u32>(CommandHeld::Attack);
        fixture.client.update(&fixture.state, std::array{command});
        const auto shown = fixture.shown();
        CHECK(shown.motion.players[1]->position == nativeBody.position());
        CHECK(shown.motion.players[1]->yaw == Approx(nativeBody.yaw()).margin(0.0001));
        CHECK(shown.players[1]->knockback == nativeImpulse.checkpoint());
        CHECK(shown.players[1]->reaction == PlayerDeed::None);
        CHECK(shown.players[1]->rollback == nativeAnimation.checkpoint());
        CHECK(shown.players[1]->health == 100);
        CHECK(shown.projectiles.empty());
        if (tick == 11) {
            CHECK(nativeBody.position() == Vec3{0});
            CHECK(nativeImpulse.sliding());
        }
        if (tick % 4 == 0) {
            fixture.state.motion.tick = tick;
            fixture.state.motion.players[1]->position = nativeBody.position();
            fixture.state.motion.players[1]->yaw = nativeBody.yaw();
            captureAction(state, nativeAnimation);
            state.knockback = nativeImpulse.checkpoint();
            state.reaction = PlayerDeed::None;
            state.rollbackInput = command;
            fixture.client.update(&fixture.state, {});
            CHECK(fixture.shown().players[1]->knockback == nativeImpulse.checkpoint());
        }
    }
}

TEST_CASE("stationary melee replay follows complete native party motion with authored class data",
          "[netplay][prediction][rollback][melee][assets]") {
    const s32 character = GENERATE(0, 2, 6);
    const bool strong = GENERATE(false, true);
    enum class Aim : u8 { Assisted, Cursor, Strafe };
    const auto aim = GENERATE(Aim::Assisted, Aim::Cursor, Aim::Strafe);
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    std::array<PlayerRuntime, 1> native;
    auto& runtime = native.front();
    CharacterSave save;
    save.character = character;
    runtime.actor.spawn(1, save, nullptr, Vec3{0}, 0);
    runtime.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(runtime.figure);
    REQUIRE(runtime.figure->actionTree());
    Prediction fixture;
    fixture.tree = *runtime.figure->actionTree();
    auto& player = *fixture.state.players[1];
    player.movement.ranged = false;
    player.movement.radius = runtime.actor.radius();
    player.movement.height = runtime.actor.height();
    player.movement.followHeight = runtime.actor.followPoint().y;
    player.movement.speed = runtime.actor.speed();
    player.melee = PlayerMeleeState{};
    auto& melee = *player.melee;
    melee.selection.feet = runtime.actor.position();
    melee.selection.reach = runtime.actor.reach();
    melee.selection.range = 30;
    melee.selection.facingDot = 0.707f;
    melee.selection.targets.push_back({Vec3{1, 0, 3}, Vec3{1, 0, 3}, 2, 2, false});
    melee.assistedAim = Vec3{1, 2.5f, 3};
    captureAction(player, runtime.figure->animator());
    player.knockback = runtime.knockback.checkpoint();
    fixture.reset();
    usize nativeBlows = 0;
    PartyMotion::Events events;
    events.select = [](usize, const SelectorInput&, s32) {};
    events.advanceTurbo = [](usize, s32, f32) {};
    events.perform = [&](usize, PartyMotion::Action action) {
        if (action == PartyMotion::Action::Melee) {
            ++nativeBlows;
        }
        CHECK(action != PartyMotion::Action::ThrowWeapon);
        CHECK(action != PartyMotion::Action::StrongThrow);
    };
    events.aim = [&](usize) { return melee.assistedAim; };
    events.attackDeed = [&](usize, bool heavy, bool moved, const Vec3& facing) {
        const auto sense =
            MeleeSelection::sense(melee.selection, true, facing, runtime.actor.yaw());
        return MeleeSelection::deed(sense, heavy, moved, runtime.figure->animator().meleeChain());
    };
    events.meleeSense = [&](usize, bool held, const Vec3& facing) {
        return MeleeSelection::sense(melee.selection, held, facing, runtime.actor.yaw());
    };
    for (u64 tick = 11; tick < 131; ++tick) {
        CAPTURE(character, strong, aim, tick);
        auto command = fixture.command(tick);
        command.direction = Vec2{0};
        command.magnitude = 0;
        if (tick < 90) {
            command.heldButtons =
                static_cast<u32>(strong ? CommandHeld::StrongAttack : CommandHeld::Attack);
        }
        if (aim == Aim::Cursor) {
            command.aimDirection = glm::normalize(Vec3{0.5f, 0, 1});
        } else if (aim == Aim::Strafe) {
            command.heldButtons |= static_cast<u32>(CommandHeld::Strafe);
        }
        REQUIRE(command.valid());
        std::array<PlayInput, InputCommand::kSeats> inputs;
        inputs[1] = SessionInputs::playInput(command);
        PartyMotion::step(native, inputs, false, 0, 1, 1.0f / 60, fixture.floor, events);
        REQUIRE_FALSE(runtime.figure->animator().lunging());
        const usize priorBlows = nativeBlows;
        fixture.client.update(&fixture.state, std::array{command});
        const auto shown = fixture.shown();
        REQUIRE(shown.players[1]->rollback);
        CHECK(*shown.players[1]->rollback == runtime.figure->animator().checkpoint());
        CHECK(shown.players[1]->knockback == runtime.knockback.checkpoint());
        CHECK(shown.motion.players[1]->position == runtime.actor.position());
        CHECK(shown.motion.players[1]->yaw == Approx(runtime.actor.yaw()).margin(0.0001));
        CHECK(shown.players[1]->health == 100);
        CHECK(shown.projectiles.empty());
        CHECK(nativeBlows == priorBlows);
        if (tick % 8 == 0) {
            fixture.state.motion.tick = tick;
            fixture.state.motion.players[1]->position = runtime.actor.position();
            fixture.state.motion.players[1]->yaw = std::remainder(runtime.actor.yaw(), kTwoPi);
            melee.captureYaw = fixture.state.motion.players[1]->yaw;
            captureAction(player, runtime.figure->animator());
            player.knockback = runtime.knockback.checkpoint();
            player.rollbackInput = command;
            fixture.client.update(&fixture.state, {});
            CHECK(*fixture.shown().players[1]->rollback == runtime.figure->animator().checkpoint());
        }
    }
    CHECK(nativeBlows > 0);
}

TEST_CASE("replayed assisted facing follows native stationary cursor and strafe policy",
          "[netplay][prediction][rollback][target-assist]") {
    enum class Intent : u8 {
        Standing,
        Moving,
        StrafeStill,
        StrafeMoving,
        CursorStill,
        CursorMoving,
        Idle
    };
    const auto intent =
        GENERATE(Intent::Standing, Intent::Moving, Intent::StrafeStill, Intent::StrafeMoving,
                 Intent::CursorStill, Intent::CursorMoving, Intent::Idle);
    Prediction fixture;
    auto& motion = *fixture.state.motion.players[1];
    motion.position = {2, 0, -3};
    motion.yaw = 0.75f;
    fixture.state.players[1]->movement.cameraYaw = 0.3f;
    PlayerWeaponState weapon;
    weapon.resource = 1;
    weapon.target = Vec3{8, 4, 9};
    fixture.state.players[1]->weapon = weapon;
    fixture.reset();
    auto input = fixture.command(11);
    input.direction = Vec2{0};
    input.magnitude = 0;
    input.heldButtons = static_cast<u32>(CommandHeld::Attack);
    if (intent == Intent::Moving || intent == Intent::StrafeMoving ||
        intent == Intent::CursorMoving) {
        input.direction = {-1, 0};
        input.magnitude = 1;
    }
    if (intent == Intent::StrafeStill || intent == Intent::StrafeMoving) {
        input.heldButtons |= static_cast<u32>(CommandHeld::Strafe);
    }
    if (intent == Intent::CursorStill || intent == Intent::CursorMoving) {
        input.aimDirection = Vec3{-1, 0, 0};
    }
    if (intent == Intent::Idle) {
        input.heldButtons = 0;
    }
    std::array<PlayerRuntime, 1> native;
    native[0].actor.spawn(1, {}, nullptr, motion.position, motion.yaw);
    std::array<PlayInput, InputCommand::kSeats> inputs;
    inputs[1] = SessionInputs::playInput(input);
    usize aimQueries = 0;
    PartyMotion::Events events;
    events.perform = [](usize, PartyMotion::Action) {};
    events.select = [](usize, const SelectorInput&, s32) {};
    events.aim = [&](usize) {
        ++aimQueries;
        return weapon.target;
    };
    PartyMotion::step(native, inputs, false, fixture.state.players[1]->movement.cameraYaw, 1,
                      1.0f / 60, fixture.floor, events);
    fixture.client.update(&fixture.state, std::array{input});
    const auto shown = fixture.shown();
    CHECK(shown.motion.players[1]->position == native[0].actor.position());
    CHECK(shown.motion.players[1]->yaw == Approx(native[0].actor.yaw()));
    CHECK(aimQueries == (intent == Intent::Standing ? 1 : 0));
    if (intent == Intent::Standing) {
        CHECK(shown.motion.players[1]->yaw == Approx(std::atan2(6.0f, 12.0f)));
    }
}

TEST_CASE("native release poses keep assisted heading separate from cursor and strafe",
          "[netplay][prediction][rollback][target-assist]") {
    const bool strong = GENERATE(false, true);
    enum class Aim : u8 { Assisted, Untargeted, Cursor, Strafe };
    const auto aim = GENERATE(Aim::Assisted, Aim::Untargeted, Aim::Cursor, Aim::Strafe);
    Prediction fixture;
    for (auto& sequence : fixture.tree.sequences) {
        sequence.frames = 6;
    }
    fixture.state.motion.players[1]->position = {2, 0, -3};
    fixture.state.motion.players[1]->yaw = 0.75f;
    PlayerWeaponState weapon;
    weapon.resource = 1;
    weapon.target = Vec3{8, 4, 9};
    if (aim == Aim::Untargeted) {
        weapon.target.reset();
    }
    fixture.state.players[1]->weapon = weapon;
    fixture.reset();
    std::vector<InputCommand> commands;
    for (u64 tick = 11; tick <= 70; ++tick) {
        auto input = fixture.command(tick);
        input.direction = Vec2{0};
        input.magnitude = 0;
        input.heldButtons =
            static_cast<u32>(strong ? CommandHeld::StrongAttack : CommandHeld::Attack);
        if (aim == Aim::Cursor) {
            input.aimDirection = Vec3{-1, 0, 0};
        } else if (aim == Aim::Strafe) {
            input.heldButtons |= static_cast<u32>(CommandHeld::Strafe);
        }
        commands.push_back(input);
    }
    fixture.client.update(&fixture.state, commands);
    const auto* replay = fixture.client.replayed(1);
    REQUIRE(replay != nullptr);
    REQUIRE_FALSE(replay->releases.empty());
    f32 expected = 0.75f;
    if (aim == Aim::Assisted) {
        expected = std::atan2(6.0f, 12.0f);
    } else if (aim == Aim::Cursor) {
        expected = -glm::half_pi<f32>();
    }
    for (const auto& release : replay->releases) {
        CHECK(release.motion.position == Vec3{2, 0, -3});
        CHECK(release.motion.yaw == Approx(expected));
        CHECK(release.strong == strong);
        CHECK(release.cursorAiming == (aim == Aim::Cursor));
    }
}

TEST_CASE(
    "delayed authoritative action checkpoints replay the native machine without cadence drift",
    "[netplay][prediction][rollback]") {
    const auto deed = GENERATE(PlayerDeed::Attack, PlayerDeed::StrongAttack, PlayerDeed::Defend);
    Prediction fixture;
    for (auto& sequence : fixture.tree.sequences) {
        sequence.frames = 6; // exercise repeated wind-up, release and recovery boundaries
    }
    fixture.reset();
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*fixture.state.players[1]->rollback));
    std::vector<CombatSnapshot> completed;
    completed.push_back(fixture.state);
    std::vector<LocalMovementPrediction::Release> expected;
    u64 authorityTick = fixture.state.motion.tick;
    usize releases = 0;
    for (u64 tick = 11; tick <= 170; ++tick) {
        auto input = fixture.command(tick);
        input.magnitude = 0;
        input.direction = Vec2{0};
        if (deed == PlayerDeed::Defend) {
            input.pressedButtons = tick == 11 ? static_cast<u32>(CommandPress::Defend) : 0;
            input.heldButtons = static_cast<u32>(CommandHeld::Turbo);
        } else {
            input.heldButtons = static_cast<u32>(
                deed == PlayerDeed::Attack ? CommandHeld::Attack : CommandHeld::StrongAttack);
        }
        native.update(PlayerMotion::Stand, 1, 1.0f / 60,
                      deed == PlayerDeed::Defend && tick != 11 ? PlayerDeed::None : deed);
        releases += native.released() || native.strongReleased() ? 1 : 0;
        if (native.released() || native.strongReleased()) {
            expected.push_back({tick, native.player().generation(),
                                *fixture.state.motion.players[1], native.strongReleased(), false,
                                native.attackSeconds()});
        }
        auto checkpoint = fixture.state;
        checkpoint.motion.tick = tick;
        captureAction(*checkpoint.players[1], native);
        checkpoint.players[1]->rollbackInput = input;
        completed.push_back(checkpoint);
        const CombatSnapshot* authority = nullptr;
        if (tick >= 17 && tick % 3 == 2) {
            authority = &completed[static_cast<usize>(tick - 6 - 10)];
            authorityTick = authority->motion.tick;
        }
        fixture.client.update(authority, std::array{input});
        const auto shown = fixture.shown();
        CAPTURE(tick, deed);
        REQUIRE(shown.players[1]->rollback);
        CHECK(*shown.players[1]->rollback == native.checkpoint());
        CHECK(shown.projectiles.empty());
        CHECK(shown.players[1]->health == 100);
        const auto* replay = fixture.client.replayed(1);
        REQUIRE(replay != nullptr);
        CHECK(replay->endTick == tick);
        usize pendingRelease = 0;
        for (const auto& release : expected) {
            if (release.tick <= authorityTick) {
                continue;
            }
            REQUIRE(pendingRelease < replay->releases.size());
            const auto& actual = replay->releases[pendingRelease++];
            CHECK(actual.tick == release.tick);
            CHECK(actual.generation == release.generation);
            CHECK(actual.strong == release.strong);
            CHECK_FALSE(actual.cursorAiming);
            CHECK(actual.attackSeconds == release.attackSeconds);
            CHECK(actual.motion.position == release.motion.position);
            CHECK(actual.motion.yaw == release.motion.yaw);
            CHECK(actual.motion.grant == release.motion.grant);
            CHECK(actual.motion.continuity == release.motion.continuity);
        }
        CHECK(replay->releases.size() == pendingRelease);
        // Presentation and duplicate datagrams do not advance native releases.
        fixture.shown(0.3f);
        fixture.shown(0.8f);
    }
    CHECK(fixture.client.metrics().samples == 160);
    if (deed == PlayerDeed::Defend) {
        CHECK(releases == 0);
    } else {
        CHECK(releases >= 3);
    }
}

TEST_CASE("native strafe releases retain their exact tick pose and generation across replay",
          "[netplay][prediction][rollback]") {
    Prediction fixture;
    for (auto& sequence : fixture.tree.sequences) {
        sequence.frames = 6;
    }
    fixture.reset();
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*fixture.state.players[1]->rollback));
    PlayerActor actor;
    const auto& movement = fixture.state.players[1]->movement;
    actor.restoreMotion(Vec3{0}, 0, movement.speed, movement.radius, movement.height,
                        movement.followHeight);
    std::vector<InputCommand> commands;
    std::vector<LocalMovementPrediction::Release> expected;
    for (u64 tick = 11; tick <= 70; ++tick) {
        auto input = fixture.command(tick);
        input.direction = {1, 0};
        input.aimDirection = Vec3{0, 0, 1};
        input.heldButtons = static_cast<u32>(CommandHeld::Attack);
        commands.push_back(input);
        native.setStrafe(StrafeWay::Right);
        actor.update({input.direction, 1}, 0, 1.0f / 60, &fixture.floor, native.moveScale(), true);
        actor.fall(1.0f / 60, fixture.floor);
        native.update(PlayerMotion::Run, 1, 1.0f / 60, PlayerDeed::Attack);
        if (native.released()) {
            auto motion = *fixture.state.motion.players[1];
            motion.position = actor.position();
            motion.yaw = actor.yaw();
            expected.push_back(
                {tick, native.player().generation(), motion, false, true, native.attackSeconds()});
        }
    }
    REQUIRE(expected.size() >= 3);
    fixture.client.update(nullptr, commands);
    const auto verify = [&] {
        const auto* replay = fixture.client.replayed(1);
        REQUIRE(replay != nullptr);
        CHECK(replay->endTick == commands.back().tick);
        REQUIRE(replay->releases.size() == expected.size());
        for (usize i = 0; i < expected.size(); ++i) {
            CAPTURE(i);
            const auto& actual = replay->releases[i];
            CHECK(actual.tick == expected[i].tick);
            CHECK(actual.generation == expected[i].generation);
            CHECK(actual.motion.position == expected[i].motion.position);
            CHECK(actual.motion.yaw == expected[i].motion.yaw);
            CHECK(actual.motion.grant == fixture.context.grants[1]);
            CHECK(actual.motion.continuity == fixture.state.motion.players[1]->continuity);
            CHECK(actual.cursorAiming);
            CHECK_FALSE(actual.strong);
            CHECK(actual.attackSeconds == expected[i].attackSeconds);
        }
        CHECK(fixture.shown().projectiles.empty());
        CHECK(fixture.shown().players[1]->health == 100);
    };
    verify();
    fixture.shown(0.1f);
    fixture.shown(0.9f);
    fixture.client.update(nullptr, commands); // retransmission replaces, never appends events
    verify();
    REQUIRE(fixture.client.replayed(2) != nullptr);
    CHECK(fixture.client.replayed(2)->releases.empty());
    CHECK(fixture.client.replayed(0) == nullptr);
    CHECK(fixture.client.replayed(static_cast<u8>(InputCommand::kSeats)) == nullptr);
}

TEST_CASE("unsafe or corrected replay withdraws all speculative throw releases atomically",
          "[netplay][prediction][rollback]") {
    Prediction fixture;
    for (auto& sequence : fixture.tree.sequences) {
        sequence.frames = 6;
    }
    fixture.reset();
    std::vector<InputCommand> commands;
    for (u64 tick = 11; tick <= 45; ++tick) {
        auto input = fixture.command(tick);
        input.magnitude = 0;
        input.heldButtons = static_cast<u32>(CommandHeld::Attack);
        commands.push_back(input);
    }
    fixture.client.update(nullptr, commands);
    REQUIRE(fixture.client.replayed(1) != nullptr);
    REQUIRE_FALSE(fixture.client.replayed(1)->releases.empty());
    SECTION("unsupported input after a release withdraws the whole replay") {
        auto item = fixture.command(46);
        item.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
        fixture.client.update(nullptr, std::array{item});
        CHECK(fixture.client.replayed(1) == nullptr);
    }
    SECTION("latest authority fallback cannot emit a release") {
        fixture.state.motion.tick = 11;
        fixture.state.players[1]->movement.ranged = false;
        fixture.client.update(&fixture.state, {});
        CHECK(fixture.client.replayed(1) == nullptr);
    }
    SECTION("late discarded attack has no surviving release after its deadline") {
        fixture.state.motion.tick = 45;
        fixture.client.update(&fixture.state, {});
        const auto* replay = fixture.client.replayed(1);
        REQUIRE(replay != nullptr);
        CHECK(replay->endTick == 45);
        CHECK(replay->releases.empty());
    }
    SECTION("invalid authority clears a previously successful replay") {
        fixture.state.motion.tick = 11;
        fixture.state.players[1]->health = -1;
        fixture.client.update(&fixture.state, {});
        CHECK(fixture.client.replayed(1) == nullptr);
    }
    SECTION("death cancels the entire pending release roster") {
        fixture.state.motion.tick = 11;
        fixture.state.players[1]->life = ReplicaPlayerLife::Dying;
        fixture.state.players[1]->movement.enabled = false;
        fixture.state.players[1]->movement.current = false;
        fixture.state.players[1]->movement.ranged = false;
        fixture.client.update(&fixture.state, {});
        CHECK(fixture.client.replayed(1) == nullptr);
    }
    SECTION("expired authority withdraws releases") {
        for (u64 tick = 0; tick <= LocalMovementPrediction::kStaleTicks; ++tick) {
            fixture.client.update(nullptr, {});
        }
        CHECK(fixture.client.replayed(1) == nullptr);
    }
    SECTION("epoch changes cannot retain old release identity") {
        ++fixture.context.epoch;
        fixture.client.begin(fixture.context, 2, fixture.floor);
        CHECK(fixture.client.replayed(1) == nullptr);
    }
    SECTION("grant changes cannot retain the previous occupant's release") {
        fixture.state.motion.tick = 11;
        ++fixture.state.motion.players[1]->grant;
        fixture.client.update(&fixture.state, {});
        CHECK(fixture.client.replayed(1) == nullptr);
    }
}

TEST_CASE("late rejected actions are removed by the host post-step checkpoint, not receipt tick",
          "[netplay][prediction][rollback]") {
    Prediction fixture;
    auto input = fixture.command(11);
    input.heldButtons = static_cast<u32>(CommandHeld::Attack);
    input.magnitude = 0;
    fixture.client.update(&fixture.state, std::array{input});
    CHECK(fixture.shown().players[1]->animation.action ==
          static_cast<u32>(PlayerAnimator::Action::Throw));
    PlayerAnimator host;
    REQUIRE(host.bind(fixture.tree, false));
    REQUIRE(host.restore(*fixture.state.players[1]->rollback));
    host.update(PlayerMotion::Stand, 1, 1.0f / 60);
    captureAction(*fixture.state.players[1], host);
    fixture.state.motion.tick = 11;
    fixture.state.players[1]->movement.receivedInput = 0; // this command missed its deadline
    fixture.state.players[1]->movement.lateInputs = 1;
    fixture.client.update(&fixture.state, {});
    CHECK(fixture.client.metrics().pending == 0);
    CHECK(*fixture.shown().players[1]->rollback == host.checkpoint());
    CHECK(fixture.shown().players[1]->animation.action ==
          static_cast<u32>(PlayerAnimator::Action::Ready));
    CHECK(fixture.shown().projectiles.empty());
}

TEST_CASE("input lead gaps retain held throws but never repeat a defend edge",
          "[netplay][prediction][rollback]") {
    const bool defend = GENERATE(false, true);
    const bool corrected = GENERATE(false, true);
    Prediction fixture;
    auto first = fixture.command(11);
    first.direction = Vec2{0};
    first.magnitude = 0;
    if (defend) {
        first.pressedButtons = static_cast<u32>(CommandPress::Defend);
    } else {
        first.heldButtons = static_cast<u32>(CommandHeld::Attack);
    }
    auto last = first;
    last.tick = 16;
    last.heldButtons = 0;
    last.pressedButtons = 0;
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*fixture.state.players[1]->rollback));
    auto authority = fixture.state;
    for (u64 tick = 11; tick <= 16; ++tick) {
        PlayerDeed deed = PlayerDeed::None;
        if (tick != 16) {
            if (!defend) {
                deed = PlayerDeed::Attack;
            } else if (tick == 11) {
                deed = PlayerDeed::Defend;
            }
        }
        native.update(PlayerMotion::Stand, 1, 1.0f / 60, deed);
        if (tick == 12) {
            captureAction(*authority.players[1], native);
            authority.motion.tick = tick;
            authority.players[1]->rollbackInput = first;
        }
    }
    fixture.client.update(&fixture.state, std::array{first});
    fixture.client.update(corrected ? &authority : nullptr, std::array{last});
    REQUIRE(fixture.shown().players[1]->rollback);
    CHECK(*fixture.shown().players[1]->rollback == native.checkpoint());
}

TEST_CASE("unsupported actions and discontinuities do not leak speculative ranged state",
          "[netplay][prediction][rollback]") {
    Prediction fixture;
    auto input = fixture.command(11);
    input.heldButtons = static_cast<u32>(CommandHeld::Attack);
    SECTION("display cursors without native checkpoints cannot invent attacks") {
        fixture.state.players[1]->rollback.reset();
        fixture.state.players[1]->movement.ranged = false;
        fixture.reset();
        fixture.client.update(&fixture.state, std::array{input});
        CHECK_FALSE(fixture.shown().players[1]->rollback);
        CHECK(fixture.shown().players[1]->animation.action ==
              fixture.state.players[1]->animation.action);
    }
    SECTION("potion, turbo, combo and inventory actions wait for world authority") {
        const auto press =
            GENERATE(CommandPress::UsePotion, CommandPress::Charge, CommandPress::TurboAttack,
                     CommandPress::SelectorUp, CommandPress::SelectorRight);
        input.pressedButtons = static_cast<u32>(press);
        fixture.client.update(&fixture.state, std::array{input});
        CHECK(*fixture.shown().players[1]->rollback == *fixture.state.players[1]->rollback);
        CHECK(fixture.shown().motion.players[1]->position == Vec3{0});
    }
    SECTION("held potion charging waits for its native inventory authority") {
        input.heldButtons = static_cast<u32>(CommandHeld::ThrowPotion);
        fixture.client.update(&fixture.state, std::array{input});
        CHECK_FALSE(fixture.client.replayed(1));
        CHECK(fixture.shown().players[1]->rollback == fixture.state.players[1]->rollback);
    }
    SECTION("death revokes predicted actions") {
        fixture.client.update(&fixture.state, std::array{input});
        fixture.state.motion.tick = 11;
        fixture.state.players[1]->life = ReplicaPlayerLife::Dying;
        fixture.state.players[1]->movement.enabled = false;
        fixture.state.players[1]->movement.current = false;
        fixture.state.players[1]->movement.ranged = false;
        fixture.client.update(&fixture.state, {});
        CHECK(fixture.client.metrics().pending == 0);
        CHECK(fixture.shown().players[1]->life == ReplicaPlayerLife::Dying);
        CHECK(fixture.shown().motion.players[1]->position == Vec3{0});
    }
    SECTION("a new epoch clears all prior action history") {
        fixture.client.update(&fixture.state, std::array{input});
        ++fixture.context.epoch;
        fixture.state.motion.epoch = fixture.context.epoch;
        fixture.reset();
        fixture.client.update(nullptr, std::array{input});
        CHECK(fixture.client.metrics().pending == 0);
        CHECK(*fixture.shown().players[1]->rollback == *fixture.state.players[1]->rollback);
    }
}

TEST_CASE("unsupported combat shows newest local authority without delayed interpolation or fake "
          "movement",
          "[netplay][prediction][rollback]") {
    const bool item = GENERATE(false, true);
    Prediction fixture;
    fixture.client.update(&fixture.state, std::array{fixture.command(11)});
    CHECK(fixture.shown().motion.players[1]->position.z > 0);
    auto newest = fixture.state;
    newest.motion.tick = 12;
    newest.motion.players[1]->position.z = 0.2f;
    newest.players[1]->movement.ranged = false;
    auto input = fixture.command(15);
    input.heldButtons = static_cast<u32>(CommandHeld::Attack);
    if (item) {
        input.pressedButtons = static_cast<u32>(CommandPress::SelectorRight);
    }
    fixture.client.update(&newest, std::array{input});
    for (const f32 blend : {0.0f, 0.5f, 1.0f}) {
        // Delayed remote rendering still has the seat at tick 10. The local
        // unsupported action must not put it back there or continue its old walk.
        auto displayed = fixture.state;
        displayed.motion.players[1]->position.z = -1;
        fixture.client.present(displayed, blend);
        REQUIRE(displayed.valid());
        CHECK(displayed.motion.players[1]->position == newest.motion.players[1]->position);
        CHECK(displayed.players[1]->animation.generation ==
              newest.players[1]->animation.generation);
        CHECK(displayed.players[1]->animation.frame == newest.players[1]->animation.frame);
        CHECK(*displayed.players[1]->rollback == *newest.players[1]->rollback);
        CHECK(displayed.players[1]->health == newest.players[1]->health);
        CHECK(displayed.projectiles.empty());
    }
    // The confirmed melee is not replayable, but current=true explicitly keeps
    // this seat on latest host presentation instead of jumping into the old buffer.
    auto confirmed = newest;
    confirmed.motion.tick = 13;
    confirmed.motion.players[1]->position.z = 0.25f;
    confirmed.players[1]->movement.enabled = false;
    PlayerAnimator host;
    REQUIRE(host.bind(fixture.tree, false));
    REQUIRE(host.restore(*newest.players[1]->rollback));
    host.update(PlayerMotion::Run, 1, 1.0f / 60, PlayerDeed::Melee);
    captureAction(*confirmed.players[1], host);
    REQUIRE_FALSE(PlayerAnimator::rollbackAction(host.action()));
    fixture.client.update(&confirmed, {});
    CHECK(fixture.client.metrics().pending == 0);
    for (const f32 blend : {0.0f, 0.5f, 1.0f}) {
        auto displayed = confirmed;
        displayed.motion.players[1]->position.z = 0.22f;
        fixture.client.present(displayed, blend);
        REQUIRE(displayed.valid());
        CHECK(displayed.motion.players[1]->position == confirmed.motion.players[1]->position);
        CHECK(*displayed.players[1]->rollback == host.checkpoint());
    }
    // Returning from newest-authority fallback to native replay compares two
    // different host ticks, not a speculative result against its correction.
    const auto corrections = fixture.client.metrics().corrections;
    auto recovered = newest;
    recovered.motion.tick = 14;
    recovered.motion.players[1]->position.z = 0.4f;
    fixture.client.update(&recovered, {});
    CHECK(fixture.client.metrics().corrections == corrections);
    auto resumed = recovered;
    fixture.client.present(resumed, 1);
    CHECK(resumed.motion.players[1]->position == recovered.motion.players[1]->position);
    // A real capture/cutscene lock still revokes even current presentation. Do not
    // override the coordinated world's supplied pose in that case.
    confirmed.motion.tick = 15;
    confirmed.players[1]->movement.current = false;
    fixture.client.update(&confirmed, {});
    auto cut = confirmed;
    cut.motion.players[1]->position.z = 0.22f;
    fixture.client.present(cut, 1);
    CHECK(cut.motion.players[1]->position.z == 0.22f);
}

TEST_CASE(
    "prediction follows native ramp and wall contacts without inventing moving-platform physics",
    "[netplay][prediction]") {
    Prediction fixture;
    SECTION("ramp") {
        fixture.floor.build(
            {{{0, 0.9805807f, -0.1961161f}, {Vec3{-10, 0, 0}, Vec3{10, 0, 0}, Vec3{10, 4, 20}}, 0},
             {{0, 0.9805807f, -0.1961161f},
              {Vec3{-10, 0, 0}, Vec3{10, 4, 20}, Vec3{-10, 4, 20}},
              0}});
        fixture.reset();
        for (u64 tick = 11; tick < 25; ++tick) {
            fixture.client.update(&fixture.state, std::array{fixture.command(tick)});
        }
        const auto position = fixture.shown().motion.players[1]->position;
        CHECK(position.z > 1);
        CHECK(position.y == Approx(position.z * 0.2f).margin(0.001));
    }
    SECTION("wall") {
        const std::array triangles{
            CollisionTriangle{{0, 0, -1}, {Vec3{-10, 0, 1}, Vec3{10, 0, 1}, Vec3{10, 10, 1}}, 1},
            CollisionTriangle{{0, 0, -1}, {Vec3{-10, 0, 1}, Vec3{10, 10, 1}, Vec3{-10, 10, 1}}, 1}};
        fixture.floor.append(triangles);
        fixture.reset();
        for (u64 tick = 11; tick < 25; ++tick) {
            fixture.client.update(&fixture.state, std::array{fixture.command(tick)});
        }
        CHECK(fixture.shown().motion.players[1]->position.z <= 0.251f);
    }
    SECTION("moving support falls back to authoritative presentation") {
        fixture.floor.setMovingObjects(std::array<s32, 1>{0});
        fixture.reset();
        fixture.client.update(&fixture.state, std::array{fixture.command(11)});
        CHECK(fixture.shown().motion.players[1]->position == Vec3{0});
    }
}

TEST_CASE("prediction is render-rate independent and mouse-relative movement uses the native basis",
          "[netplay][prediction]") {
    const s32 renders = GENERATE(0, 1, 2, 4);
    Prediction fixture;
    for (u64 tick = 11; tick < 25; ++tick) {
        auto input = fixture.command(tick);
        input.aimDirection = Vec3{1, 0, 0};
        fixture.client.update(&fixture.state, std::array{input});
        for (s32 draw = 0; draw < renders; ++draw) {
            fixture.shown(static_cast<f32>(draw + 1) / static_cast<f32>(renders));
        }
    }
    const auto shown = fixture.shown();
    CHECK(shown.motion.players[1]->position.x == Approx((5.0f + 13 * 6.5f) / 60));
    CHECK(shown.motion.players[1]->position.z == Approx(0).margin(0.001));
    CHECK(shown.motion.players[1]->yaw == Approx(kPi / 2));
    CHECK(fixture.client.metrics().samples == 14);
}

TEST_CASE("prediction envelope is explicitly encoded and rejects invalid physical values",
          "[netplay][prediction]") {
    Prediction fixture;
    auto& movement = fixture.state.players[1]->movement;
    movement.receivedInput = 20;
    movement.arrivalMargin = -7;
    movement.lateInputs = 5;
    const auto bytes = CombatPacket::encode(fixture.state);
    REQUIRE(bytes);
    const auto decoded = CombatPacket::decode(*bytes);
    REQUIRE(decoded);
    CHECK(decoded->players[1]->movement.enabled);
    CHECK(decoded->players[1]->movement.receivedInput == 20);
    CHECK(decoded->players[1]->movement.arrivalMargin == -7);
    CHECK(decoded->players[1]->movement.lateInputs == 5);
    movement.radius = -1;
    CHECK_FALSE(CombatPacket::encode(fixture.state));
}

TEST_CASE("small corrections ease out without delaying new local input or changing host state",
          "[netplay][prediction]") {
    Prediction fixture;
    fixture.client.update(&fixture.state, std::array{fixture.command(15)});
    const auto before = fixture.shown().motion.players[1]->position;
    ++fixture.state.motion.tick;
    fixture.state.motion.players[1]->position.x = 0.2f;
    fixture.client.update(&fixture.state, {});
    const auto first = fixture.shown().motion.players[1]->position;
    CHECK(first.x == Approx(before.x).margin(0.001));
    CHECK(fixture.client.metrics().largestCorrection == Approx(0.2f).margin(0.001));
    for (usize tick = 0; tick < 12; ++tick) {
        fixture.client.update(&fixture.state, {});
    }
    CHECK(fixture.shown().motion.players[1]->position.x > 0.19f);
    CHECK(fixture.state.motion.players[1]->position.x == 0.2f);
}

TEST_CASE("two local seats predict independently while remote actors remain interpolated",
          "[netplay][prediction]") {
    Prediction fixture;
    auto second = fixture.command(11, 2);
    second.direction = {0, -1};
    fixture.client.update(&fixture.state, std::array{fixture.command(11), second});
    const auto shown = fixture.shown();
    CHECK(shown.motion.players[0]->position == fixture.state.motion.players[0]->position);
    CHECK(shown.motion.players[1]->position.z > 0);
    CHECK(shown.motion.players[2]->position.z < 0);
    CHECK(fixture.client.metrics().pending == 2);
}
} // namespace
