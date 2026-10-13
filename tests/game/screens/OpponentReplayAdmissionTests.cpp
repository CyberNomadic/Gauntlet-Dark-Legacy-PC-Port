#include <array>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "OpponentPredictionFixture.h"
#include "game/screens/OnlineOpponentReplay.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TreeInfo actionTree() {
    TreeInfo tree;
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
    return tree;
}

bool absent(const LocalMovementPrediction::ReplayCallbacks& callbacks) {
    return !callbacks.begin && !callbacks.beforeTick && !callbacks.afterTick && !callbacks.end;
}

struct AdmissionFixture {
    test::ContinuationFixture native{1, 2};
    TreeInfo tree = actionTree();
    MatchContext context;
    CombatSnapshot authority;
    OnlineOpponentReplay coordinator;

    explicit AdmissionFixture(bool populated = true) {
        context.epoch = 7;
        context.scene = 1;
        context.owners = {1, 2, 0, 0};
        context.grants = {1, 1, 0, 0};
        REQUIRE(context.valid());
        authority.motion.epoch = context.epoch;
        authority.motion.tick = 100;
        authority.motion.cameraContinuity = 1;
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        if (populated) {
            native.spawn();
        }
        authority.opponents = native.capture();
        auto& frame = authority.opponentFrame.emplace();
        frame.initialPlayers = 2;
        for (const u8 seat : std::array<u8, 2>{0, 1}) {
            const Vec3 position = seat == 0 ? Vec3{30, 0, 0} : Vec3{0};
            authority.motion.players[seat] = SeatMotion{1, 1, position, 0};
            auto& player = authority.players[seat].emplace();
            player.health = 100;
            player.rollback = animator.checkpoint();
            player.transitionPose = animator.transitionPose();
            const auto& cursor = animator.player();
            player.animation = {static_cast<u32>(animator.action()), cursor.sequence(),
                                cursor.generation(), cursor.presentationFrame(),
                                cursor.transition()};
            player.movement.enabled = true;
            player.movement.current = true;
            player.movement.ranged = true;
            player.floor = PlayerFloorState{0, 0, std::nullopt};
            auto& contacts = player.locomotion.emplace();
            contacts.origin = position;
            contacts.radius = PlayerLocomotionState::kMaxRadius;
            contacts.ticks = PlayerLocomotionState::kMaxTicks;
            EnemyView view;
            view.player = seat;
            view.position = position;
            frame.players.push_back({seat, view});
        }
        authority.geometry = native.world.scene().geometry();
        authority.worldMotion = native.world.motion();
        REQUIRE(authority.valid());
        coordinator.begin(context, 2, native.prediction, native.world.scene(),
                          native.world.layout(), native.world.collision());
        // Establish that the complete native seed really qualifies. Otherwise
        // an always-disabled coordinator could make the refusal tests vacuous.
        if (populated) {
            const auto callbacks = coordinator.callbacks(&authority);
            REQUIRE(callbacks.begin);
            REQUIRE(callbacks.beforeTick);
            REQUIRE(callbacks.afterTick);
            REQUIRE(callbacks.end);
            REQUIRE(callbacks.begin(authority));
            callbacks.end(false);
        }
    }

    InputCommand command(u64 tick) const {
        InputCommand input;
        input.epoch = context.epoch;
        input.tick = tick;
        input.grant = context.grants[1];
        input.seat = 1;
        return input;
    }
    void bind(LocalMovementPrediction& prediction) const {
        prediction.begin(context, 2, native.collision);
        REQUIRE(prediction.setPlayer(1, tree));
        prediction.update(&authority, {});
    }
};

TEST_CASE("empty opponent pools leave independent local movement available",
          "[rollback][opponent-replay-admission][assets]") {
    AdmissionFixture fixture(false);
    REQUIRE(fixture.authority.opponents);
    REQUIRE(fixture.authority.opponents->generators.generators.empty());
    REQUIRE(fixture.authority.opponents->missiles.missiles.empty());
    REQUIRE(fixture.native.enemies.count() == 0);
    const auto before = fixture.native.prediction.checkpoint();
    LocalMovementPrediction independent;
    LocalMovementPrediction candidate;
    fixture.bind(independent);
    fixture.bind(candidate);
    std::vector<InputCommand> inputs;
    for (u64 tick = 101; tick <= 104; ++tick) {
        auto input = fixture.command(tick);
        input.direction = {0, 1};
        input.magnitude = 1;
        inputs.push_back(input);
    }
    const auto callbacks = fixture.coordinator.callbacks(&fixture.authority, inputs);
    REQUIRE(absent(callbacks));
    independent.update(&fixture.authority, inputs);
    candidate.update(&fixture.authority, inputs, callbacks);
    const auto* expected = independent.replayed(1);
    const auto* actual = candidate.replayed(1);
    REQUIRE(expected != nullptr);
    REQUIRE(actual != nullptr);
    REQUIRE_FALSE(expected->frames.empty());
    REQUIRE_FALSE(actual->frames.empty());
    CHECK(expected->frames.back().motion.position != fixture.authority.motion.players[1]->position);
    CHECK(actual->frames.back().motion.position == expected->frames.back().motion.position);
    CHECK(actual->frames.back().animation == expected->frames.back().animation);
    CHECK_FALSE(fixture.coordinator.active());
    CHECK(fixture.native.prediction.checkpoint() == before);
}

TEST_CASE("opponent coupling preserves long-lead stationary throw replay",
          "[rollback][opponent-replay-admission][assets]") {
    AdmissionFixture fixture;
    LocalMovementPrediction independent;
    LocalMovementPrediction candidate;
    fixture.bind(independent);
    fixture.bind(candidate);
    std::vector<InputCommand> inputs;
    for (u64 tick = 101; tick <= 115; ++tick) {
        auto input = fixture.command(tick);
        input.heldButtons = static_cast<u32>(CommandHeld::Attack);
        inputs.push_back(input);
    }
    const auto originalPools = fixture.native.capture();
    const auto callbacks = fixture.coordinator.callbacks(&fixture.authority, inputs);
    REQUIRE(absent(callbacks)); // preserve the independent native attack lane
    independent.update(&fixture.authority, inputs);
    candidate.update(&fixture.authority, inputs, callbacks);
    const auto* expected = independent.replayed(1);
    const auto* actual = candidate.replayed(1);
    REQUIRE(expected != nullptr);
    REQUIRE(actual != nullptr);
    REQUIRE_FALSE(expected->releases.empty());
    CHECK(actual->endTick == 115);
    REQUIRE(actual->releases.size() == expected->releases.size());
    for (usize index = 0; index < actual->releases.size(); ++index) {
        CHECK(actual->releases[index].tick == expected->releases[index].tick);
        CHECK(actual->releases[index].generation == expected->releases[index].generation);
        CHECK(actual->releases[index].motion.position == expected->releases[index].motion.position);
    }
    auto expectedView = fixture.authority;
    auto actualView = fixture.authority;
    independent.present(expectedView, 1);
    candidate.present(actualView, 1);
    CHECK(actualView.players[1]->rollback == expectedView.players[1]->rollback);
    CHECK(actualView.motion.players[1]->position == expectedView.motion.players[1]->position);
    CHECK_FALSE(fixture.coordinator.active());
    CHECK(fixture.native.capture() == originalPools);
    // The attack exclusion must remain recorded even without a new input packet.
    CHECK(absent(fixture.coordinator.callbacks(&fixture.authority)));
    fixture.authority.motion.tick = 103; // shorter horizon does not resolve held attacks
    CHECK(absent(fixture.coordinator.callbacks(&fixture.authority)));
    fixture.authority.motion.tick = 115;
    CHECK_FALSE(absent(fixture.coordinator.callbacks(&fixture.authority)));
}

TEST_CASE("opponent coupling refuses a movement horizon beyond its contact certificate",
          "[rollback][opponent-replay-admission][assets]") {
    AdmissionFixture fixture;
    std::vector<InputCommand> inputs;
    for (u64 tick = 101; tick <= 115; ++tick) {
        auto input = fixture.command(tick);
        input.direction = {0, 1};
        input.magnitude = 1;
        REQUIRE(input.valid());
        inputs.push_back(input);
    }
    const auto before = fixture.native.capture();
    CHECK(absent(fixture.coordinator.callbacks(&fixture.authority, inputs)));
    CHECK(absent(fixture.coordinator.callbacks(&fixture.authority)));
    fixture.authority.motion.tick = 102;
    CHECK(absent(fixture.coordinator.callbacks(&fixture.authority))); // still 13 ticks
    fixture.authority.motion.tick = 103;
    CHECK_FALSE(absent(fixture.coordinator.callbacks(&fixture.authority))); // exactly 12
    CHECK(fixture.native.capture() == before);
    CHECK_FALSE(fixture.coordinator.active());
}

TEST_CASE("unsupported new action inputs retain independent replay until host resolution",
          "[rollback][opponent-replay-admission][assets]") {
    enum class Input {
        Attack,
        HeldAttack,
        StrongAttack,
        Turbo,
        UsePotion,
        ThrowPotion,
        Charge,
        Combo,
        Selector
    };
    const auto kind = GENERATE(Input::Attack, Input::HeldAttack, Input::StrongAttack, Input::Turbo,
                               Input::UsePotion, Input::ThrowPotion, Input::Charge, Input::Combo,
                               Input::Selector);
    CAPTURE(static_cast<u32>(kind));
    AdmissionFixture fixture;
    auto input = fixture.command(103);
    switch (kind) {
    case Input::Attack: input.pressedButtons = static_cast<u32>(CommandPress::Attack); break;
    case Input::HeldAttack: input.heldButtons = static_cast<u32>(CommandHeld::Attack); break;
    case Input::StrongAttack:
        input.heldButtons = static_cast<u32>(CommandHeld::StrongAttack);
        break;
    case Input::Turbo: input.pressedButtons = static_cast<u32>(CommandPress::TurboAttack); break;
    case Input::UsePotion: input.pressedButtons = static_cast<u32>(CommandPress::UsePotion); break;
    case Input::ThrowPotion: input.heldButtons = static_cast<u32>(CommandHeld::ThrowPotion); break;
    case Input::Charge: input.pressedButtons = static_cast<u32>(CommandPress::Charge); break;
    case Input::Combo: input.heldButtons = static_cast<u32>(CommandHeld::Combo); break;
    case Input::Selector: input.pressedButtons = static_cast<u32>(CommandPress::SelectorUp); break;
    }
    REQUIRE(input.valid());
    const auto before = fixture.native.capture();
    REQUIRE(absent(fixture.coordinator.callbacks(&fixture.authority, std::array{input})));
    CHECK(absent(fixture.coordinator.callbacks(&fixture.authority))); // not only this packet
    CHECK(absent(fixture.coordinator.callbacks(&fixture.authority, std::array{input})));
    fixture.authority.motion.tick = 102;
    CHECK(absent(fixture.coordinator.callbacks(&fixture.authority)));
    fixture.authority.motion.tick = 103; // actual post-step resolution, not input receipt
    CHECK_FALSE(absent(fixture.coordinator.callbacks(&fixture.authority)));
    CHECK(fixture.native.capture() == before);
    CHECK_FALSE(fixture.coordinator.active());
}

TEST_CASE("known unsupported opponent output opts out before arming local replay",
          "[rollback][opponent-replay-admission][assets]") {
    AdmissionFixture fixture;
    auto unsupported = fixture.authority;
    EnemyBlow blow;
    blow.player = 1;
    blow.kind = kGruntKind;
    blow.damage = 1;
    unsupported.opponents->enemies.blows.push_back(blow);
    REQUIRE(unsupported.valid());
    const auto before = fixture.native.prediction.checkpoint();
    CHECK(absent(fixture.coordinator.callbacks(&unsupported)));
    CHECK_FALSE(fixture.coordinator.active());
    CHECK(fixture.native.prediction.checkpoint() == before);
    CHECK_FALSE(absent(fixture.coordinator.callbacks(&fixture.authority)));
    fixture.coordinator.clear();
    fixture.native.prediction.clear();
    CHECK(absent(fixture.coordinator.callbacks(&fixture.authority)));
}

TEST_CASE("captured attacks cannot release into the quiet opponent phase on neutral input",
          "[rollback][opponent-replay-admission][assets]") {
    const auto seat = GENERATE(0U, 1U); // remote and locally owned actors both affect the pool
    AdmissionFixture fixture;
    const auto before = fixture.native.prediction.checkpoint();
    auto& player = *fixture.authority.players[seat];
    SECTION("an already-started weapon wind-up") {
        PlayerAnimator animator;
        REQUIRE(animator.bind(fixture.tree, false));
        animator.update(PlayerMotion::Stand, 1, 1.0f / 60, PlayerDeed::Attack);
        REQUIRE(animator.action() == PlayerAnimator::Action::Throw);
        player.rollback = animator.checkpoint();
        player.transitionPose = animator.transitionPose();
        const auto& cursor = animator.player();
        player.animation = {static_cast<u32>(animator.action()), cursor.sequence(),
                            cursor.generation(), cursor.presentationFrame(), cursor.transition()};
    }
    SECTION("a held-input seed can start the next throw") {
        auto held = fixture.command(fixture.authority.motion.tick);
        held.seat = static_cast<u8>(seat);
        held.grant = fixture.context.grants[seat];
        held.heldButtons = static_cast<u32>(CommandHeld::Attack);
        player.rollbackInput = held;
    }
    REQUIRE(fixture.authority.valid());
    const auto neutral = fixture.command(101);
    CHECK(absent(fixture.coordinator.callbacks(&fixture.authority, std::array{neutral})));
    CHECK_FALSE(fixture.coordinator.active());
    CHECK(fixture.native.prediction.checkpoint() == before);
}
} // namespace
