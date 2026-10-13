#include <array>
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "game/players/PlayerAnimator.h"
#include "game/screens/LocalMovementPrediction.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TreeInfo meleeTree() {
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

void capture(PlayerCombatState& state, const PlayerAnimator& animator) {
    state.rollback = animator.checkpoint();
    state.transitionPose = animator.transitionPose();
    const auto& cursor = animator.player();
    state.animation = {static_cast<u32>(animator.action()), cursor.sequence(), cursor.generation(),
                       cursor.presentationFrame(), cursor.transition()};
}

struct Fixture {
    TreeInfo tree = meleeTree();
    WorldCollision floor;
    MatchContext context;
    CombatSnapshot authority;
    LocalMovementPrediction replay;

    Fixture() {
        floor.build({{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}}, 0},
                     {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}}, 0}});
        context.epoch = 3;
        context.scene = 1;
        context.owners = {1, 2, 2, 0};
        context.grants = {1, 4, 2, 0};
        authority.motion.epoch = context.epoch;
        authority.motion.tick = 10;
        authority.motion.cameraContinuity = 1;
        PlayerAnimator native;
        REQUIRE(native.bind(tree, false));
        for (usize seat = 0; seat < 3; ++seat) {
            authority.motion.players[seat] = SeatMotion{context.grants[seat], 1, Vec3{0}, 0};
            auto& player = authority.players[seat].emplace();
            player.health = 100;
            player.movement.enabled = true;
            player.movement.current = true;
            capture(player, native);
            target(static_cast<u8>(seat), 0, false);
        }
        EnemyCombatState enemy;
        enemy.instance = 42;
        enemy.kind = 1;
        enemy.health = enemy.fullHealth = 100;
        authority.enemies.push_back(enemy);
    }
    void target(u8 seat, f32 yaw, bool low) {
        auto& descriptor = authority.players[seat]->melee.emplace();
        descriptor.captureYaw = authority.motion.players[seat]->yaw;
        descriptor.selection.feet = authority.motion.players[seat]->position;
        descriptor.selection.reach = 1.5f;
        descriptor.selection.range = 30;
        descriptor.selection.facingDot = 0.707f;
        MeleeSelection::Target contact;
        contact.base = {std::sin(yaw), 0, std::cos(yaw)};
        contact.offset = contact.base;
        contact.distance = -0.25f; // An overlapping body permits side and rear selection.
        contact.score = -0.25f;
        contact.low = low;
        descriptor.selection.contact = MeleeSelection::Contact{contact, false};
    }
    void reset() {
        replay.begin(context, 2, floor);
        REQUIRE(replay.setPlayer(1, tree));
        REQUIRE(replay.setPlayer(2, tree));
        accept(authority);
    }
    void accept(const CombatSnapshot& host, std::span<const InputCommand> commands = {}) {
        const auto encoded = CombatPacket::encode(host);
        REQUIRE(encoded);
        const auto decoded = CombatPacket::decode(*encoded);
        REQUIRE(decoded);
        authority = *decoded;
        replay.update(&authority, commands);
    }
    InputCommand attack(u64 tick, u8 seat = 1, bool strong = false) const {
        InputCommand input;
        input.epoch = context.epoch;
        input.tick = tick;
        input.grant = context.grants[seat];
        input.seat = seat;
        input.heldButtons =
            static_cast<u32>(strong ? CommandHeld::StrongAttack : CommandHeld::Attack);
        return input;
    }
    CombatSnapshot shown() const {
        auto result = authority;
        replay.present(result, 1);
        REQUIRE(result.valid());
        return result;
    }
};

TEST_CASE("stationary melee replay uses fresh low and directional context before host execution",
          "[netplay][melee-prediction]") {
    const bool low = GENERATE(false, true);
    const bool assisted = GENERATE(false, true);
    const f32 targetYaw = GENERATE(-2.5f, -1.2f, 0.0f, 1.2f, 2.5f);
    Fixture fixture;
    fixture.target(1, targetYaw, low);
    if (assisted) {
        fixture.authority.players[1]->melee->assistedAim =
            fixture.authority.players[1]->melee->selection.contact->target.base;
    }
    fixture.reset();
    const auto unchanged = CombatPacket::encode(fixture.authority);
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*fixture.authority.players[1]->rollback));
    const Vec3 target = fixture.authority.players[1]->melee->selection.contact->target.base;
    const f32 bearing = std::atan2(target.x, target.z);
    const f32 expectedYaw = assisted ? bearing : 0;
    native.setMelee({MeleeRange::Swing, low, std::remainder(bearing - expectedYaw, kTwoPi)});
    native.update(PlayerMotion::Stand, 1, 1.0f / 60,
                  low ? PlayerDeed::MeleeLow : PlayerDeed::Melee);
    fixture.replay.update(&fixture.authority, std::array{fixture.attack(11)});
    const auto shown = fixture.shown();
    REQUIRE(shown.players[1]->rollback);
    CHECK(*shown.players[1]->rollback == native.checkpoint());
    CHECK(shown.motion.players[1]->yaw == Approx(expectedYaw).margin(1e-6f));
    CHECK(shown.motion.players[1]->position == Vec3{0});
    CHECK(shown.players[0]->animation.action == fixture.authority.players[0]->animation.action);
    CHECK(shown.players[2]->animation.action == fixture.authority.players[2]->animation.action);
    CHECK(shown.projectiles.empty());
    CHECK(shown.enemies.front().health == 100);
    CHECK(shown.players[1]->health == 100);
    CHECK(CombatPacket::encode(fixture.authority) == unchanged);
}

TEST_CASE("delayed transported melee checkpoints preserve native chains and contact ticks",
          "[netplay][melee-prediction]") {
    const bool strong = GENERATE(false, true);
    const bool low = GENERATE(false, true);
    Fixture fixture;
    fixture.target(1, 0, low);
    fixture.reset();
    const auto initial = fixture.authority;
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*initial.players[1]->rollback));
    std::vector<CombatSnapshot> history{initial};
    usize contacts = 0;
    for (u64 tick = 11; tick <= 100; ++tick) {
        auto input = fixture.attack(tick, 1, strong);
        const bool held = (tick - 11) % 24 < 14;
        if (!held) {
            input.heldButtons = 0;
        }
        PlayerDeed deed = PlayerDeed::None;
        if (held) {
            if (strong) {
                deed = low ? PlayerDeed::MeleeSlowLow : PlayerDeed::MeleeSlow;
            } else {
                deed = low ? PlayerDeed::MeleeLow : PlayerDeed::Melee;
            }
        }
        native.setMelee({MeleeRange::Swing, low, 0});
        native.update(PlayerMotion::Stand, 1, 1.0f / 60, deed);
        contacts += native.meleeStruck() ? 1 : 0;
        auto host = initial;
        host.motion.tick = tick;
        capture(*host.players[1], native);
        host.players[1]->rollbackInput = input;
        history.push_back(host);
        if (tick >= 17 && tick % 3 == 2) {
            fixture.accept(history[static_cast<usize>(tick - 6 - 10)], std::array{input});
        } else {
            fixture.replay.update(&fixture.authority, std::array{input});
        }
        CAPTURE(tick, strong, low);
        const auto shown = fixture.shown();
        REQUIRE(shown.players[1]->rollback);
        CHECK(*shown.players[1]->rollback == native.checkpoint());
        CHECK(shown.projectiles.empty());
        CHECK(shown.enemies.front().health == 100);
        CHECK(shown.players[1]->health == 100);
        const auto* replayed = fixture.replay.replayed(1);
        REQUIRE(replayed != nullptr);
        CHECK(replayed->releases.empty());
        CHECK(replayed->endTick == tick);
    }
    CHECK(contacts > 0);
}

TEST_CASE("rejected melee input and guard cancellation discard speculative contact",
          "[netplay][melee-prediction]") {
    const bool rejected = GENERATE(false, true);
    Fixture fixture;
    fixture.reset();
    fixture.replay.update(&fixture.authority, std::array{fixture.attack(11)});
    REQUIRE(PlayerAnimator::isMelee(
        static_cast<PlayerAnimator::Action>(fixture.shown().players[1]->animation.action)));
    if (rejected) {
        auto host = fixture.authority;
        host.motion.tick = 11;
        fixture.accept(host);
        CHECK(fixture.shown().players[1]->animation.action ==
              static_cast<u32>(PlayerAnimator::Action::Ready));
        CHECK(fixture.replay.metrics().pending == 0);
    } else {
        PlayerAnimator native;
        REQUIRE(native.bind(fixture.tree, false));
        REQUIRE(native.restore(*fixture.shown().players[1]->rollback));
        native.setMelee({MeleeRange::Swing, false, 0});
        native.update(PlayerMotion::Stand, 1, 1.0f / 60, PlayerDeed::Defend);
        auto guard = fixture.attack(12);
        guard.heldButtons = 0;
        guard.pressedButtons = static_cast<u32>(CommandPress::Defend);
        fixture.replay.update(&fixture.authority, std::array{guard});
        REQUIRE(fixture.shown().players[1]->rollback);
        CHECK(*fixture.shown().players[1]->rollback == native.checkpoint());
        CHECK_FALSE(native.meleeStruck());
    }
    CHECK(fixture.shown().enemies.front().health == 100);
    CHECK(fixture.shown().projectiles.empty());
}

TEST_CASE("replacement melee geometry removes vanished targets and ignores reordered authority",
          "[netplay][melee-prediction]") {
    Fixture fixture;
    fixture.target(1, 0, true);
    fixture.reset();
    fixture.replay.update(&fixture.authority, std::array{fixture.attack(11)});
    REQUIRE(fixture.shown().players[1]->animation.action ==
            static_cast<u32>(PlayerAnimator::Action::LowKick));

    const auto oldHost = fixture.authority;
    auto host = oldHost;
    host.motion.tick = 11;
    host.players[1]->melee->selection.contact.reset();
    host.enemies.clear();
    fixture.accept(host, std::array{fixture.attack(12)});

    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*host.players[1]->rollback));
    native.setMelee({MeleeRange::Beyond, false, 0});
    native.update(PlayerMotion::Stand, 1, 1.0f / 60, PlayerDeed::Attack);
    const auto shown = fixture.shown();
    REQUIRE(shown.players[1]->rollback);
    CHECK(*shown.players[1]->rollback == native.checkpoint());
    CHECK(shown.enemies.empty());
    CHECK_FALSE(PlayerAnimator::isMelee(native.action()));

    fixture.replay.update(&oldHost, {});
    CHECK(fixture.shown().players[1]->rollback == shown.players[1]->rollback);
    CHECK(fixture.shown().enemies.empty());
}

TEST_CASE("cursor melee uses its new facing without stealing another local seat's context",
          "[netplay][melee-prediction]") {
    Fixture fixture;
    fixture.target(1, 1.2f, false);
    fixture.target(2, -1.2f, true);
    fixture.authority.players[1]->melee->assistedAim = Vec3{-1, 0, 0};
    fixture.authority.players[2]->melee->assistedAim = Vec3{1, 0, 0};
    fixture.reset();
    const std::array<f32, 2> headings{0.4f, -0.4f};
    std::array<InputCommand, 2> inputs{fixture.attack(11, 1), fixture.attack(11, 2)};
    for (usize i = 0; i < inputs.size(); ++i) {
        inputs[i].aimDirection = Vec3{std::sin(headings[i]), 0, std::cos(headings[i])};
    }
    fixture.replay.update(&fixture.authority, inputs);
    const auto shown = fixture.shown();
    for (usize i = 0; i < inputs.size(); ++i) {
        const usize seat = i + 1;
        const bool low = seat == 2;
        const Vec3 target = fixture.authority.players[seat]->melee->selection.contact->target.base;
        const Vec3 facing = *inputs[i].aimDirection;
        const f32 bodyYaw = std::atan2(facing.x, facing.z);
        const f32 bearing = std::atan2(target.x, target.z);
        PlayerAnimator native;
        REQUIRE(native.bind(fixture.tree, false));
        REQUIRE(native.restore(*fixture.authority.players[seat]->rollback));
        native.setMelee({MeleeRange::Swing, low, std::remainder(bearing - bodyYaw, kTwoPi)});
        native.update(PlayerMotion::Stand, 1, 1.0f / 60,
                      low ? PlayerDeed::MeleeLow : PlayerDeed::Melee);
        CAPTURE(seat);
        REQUIRE(shown.players[seat]->rollback);
        CHECK(*shown.players[seat]->rollback == native.checkpoint());
        CHECK(shown.motion.players[seat]->yaw == Approx(headings[i]).margin(1e-6f));
        CHECK(shown.players[seat]->health == 100);
    }
    CHECK(shown.enemies.front().health == 100);
    CHECK(shown.projectiles.empty());
}

TEST_CASE("unreplayable melee context falls back to the newest host state",
          "[netplay][melee-prediction]") {
    enum class Unsafe : u8 { Missing, Moving, Heading, Feet, Item, Lunge };
    const auto unsafe = GENERATE(Unsafe::Missing, Unsafe::Moving, Unsafe::Heading, Unsafe::Feet,
                                 Unsafe::Item, Unsafe::Lunge);
    Fixture fixture;
    if (unsafe == Unsafe::Missing) {
        fixture.authority.players[1]->melee.reset();
    } else if (unsafe == Unsafe::Heading) {
        fixture.authority.players[1]->melee->captureYaw = 1;
    } else if (unsafe == Unsafe::Lunge) {
        PlayerAnimator native;
        REQUIRE(native.bind(fixture.tree, false));
        native.setMelee({MeleeRange::Step, false, 0});
        native.update(PlayerMotion::Run, 1, 1.0f / 60, PlayerDeed::Melee);
        REQUIRE(native.action() == PlayerAnimator::Action::Step1);
        capture(*fixture.authority.players[1], native);
    }
    fixture.reset();
    auto input = fixture.attack(11);
    if (unsafe == Unsafe::Moving) {
        input.direction = {0, 1};
        input.magnitude = 1;
    } else if (unsafe == Unsafe::Item) {
        input.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    } else if (unsafe == Unsafe::Feet) {
        // A context may remain in a presentation-only snapshot after local feet
        // changed, but a newer network checkpoint must not carry that mismatch.
        fixture.authority.motion.tick = 11;
        fixture.authority.players[1]->melee->selection.feet.x = 1;
        input.tick = 12;
        CHECK_FALSE(CombatPacket::encode(fixture.authority));
    }
    fixture.replay.update(&fixture.authority, std::array{input});
    const auto shown = fixture.shown();
    CHECK(shown.players[1]->animation.action == fixture.authority.players[1]->animation.action);
    CHECK(shown.motion.players[1]->position == fixture.authority.motion.players[1]->position);
    CHECK(fixture.replay.replayed(1) == nullptr);
    CHECK(shown.enemies.front().health == 100);
    CHECK(shown.projectiles.empty());
}

} // namespace
