#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/OnlinePair.h"
#include "game/players/PlayerAnimator.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/OnlinePlay.h"
#include "game/screens/PartyCollision.h"
#include "game/screens/PartyMotion.h"

namespace {
using namespace gdl;
using namespace gdl::game;

bool throwing(const PlayerCombatState& state) {
    switch (static_cast<PlayerAnimator::Action>(state.animation.action)) {
    case PlayerAnimator::Action::Throw:
    case PlayerAnimator::Action::ThrowMoving:
    case PlayerAnimator::Action::ThrowRelease:
    case PlayerAnimator::Action::ThrowMovingRelease:
    case PlayerAnimator::Action::ThrowRecover:
    case PlayerAnimator::Action::ThrowMovingRecover: return true;
    default: return false;
    }
}
bool playerProjectile(const CombatSnapshot& state) {
    return std::ranges::any_of(state.projectiles, [](const auto& projectile) {
        return projectile.source == ProjectileSource::Player ||
               projectile.source == ProjectileSource::PlayerEffect ||
               projectile.source == ProjectileSource::Streak;
    });
}
using ProjectileKey = std::pair<ProjectileSource, u64>;
std::optional<ProjectileKey> missileKey(const PlayerMissiles& missiles, s32 owner) {
    for (usize i = 0; i < missiles.count(); ++i) {
        const auto& missile = missiles.missile(i);
        if (missile.owner != owner) {
            continue;
        }
        if (missile.effect == 0) {
            return ProjectileKey{ProjectileSource::Player, missile.instance};
        }
        // Native weapon trees are replicated as PlayerEffect. Player denotes
        // only the model-only fallback, not every player's physical missile.
        const auto& visuals = missiles.visuals();
        for (usize effect = 0; effect < visuals.count(); ++effect) {
            const auto& visual = visuals.effect(effect);
            if (visual.id == missile.effect) {
                return ProjectileKey{ProjectileSource::PlayerEffect, visual.instance};
            }
        }
    }
    return std::nullopt;
}

struct RollbackPair {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice hostDevice;
    test::FakeRenderDevice guestDevice;
    GameConfig config;
    StringTable strings;
    LevelCatalog catalog;
    GameContext context;
    test::OnlinePair wire{{}, false, 1, 2};
    OnlineParty hostParty;
    OnlineParty guestParty;
    OnlinePlay host;
    OnlinePlay guest;

    RollbackPair() {
        hostDevice.recordDraws = false;
        guestDevice.recordDraws = false;
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        context.config = &config;
        context.strings = &strings;
        context.levels = &catalog;
        context.unpackedRoot = root;
        const auto choice = [](s32 device, s32 color) {
            CharacterSave save;
            save.name = "TEST";
            save.color = color;
            save.moviesSeen = {"PRIVATE"};
            return PartyMember{device, save, {}};
        };
        REQUIRE(hostParty.select(wire.hostSession, std::array{choice(0, 0)}));
        REQUIRE(guestParty.select(wire.guestSession, std::array{choice(0, 1), choice(2, 2)}));
        wire.start();
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{0};
        REQUIRE(guest.open(guestDevice, context, wire.guestSession, guestParty));
        REQUIRE(host.open(hostDevice, context, wire.hostSession, hostParty, LevelRef::tower(),
                          options));
        for (s32 tick = 0; tick < 3600 && (host.phase() != OnlinePlay::Phase::Playing ||
                                           guest.phase() != OnlinePlay::Phase::Playing);
             ++tick) {
            step();
        }
        REQUIRE(host.phase() == OnlinePlay::Phase::Playing);
        REQUIRE(guest.phase() == OnlinePlay::Phase::Playing);
        for (s32 tick = 0; tick < 150; ++tick) {
            step();
        }
        draw();
        REQUIRE(wire.guest.playback().latest());
        REQUIRE(guest.shown());
        REQUIRE(wire.guest.playback().latest()->players[1]);
        REQUIRE(wire.guest.playback().latest()->players[2]);
        REQUIRE(wire.guest.playback().latest()->players[1]->rollback);
        REQUIRE(wire.guest.playback().latest()->players[2]->rollback);
    }
    void draw() {
        guest.render(guestDevice, Mat4{1}, 1280, 720, 1.0 / 60, 1);
        REQUIRE(guest.phase() != OnlinePlay::Phase::Failed);
    }
    void step(const SessionInputs::Frame& input = {}) {
        REQUIRE(host.update({}) != OnlinePlay::Phase::Failed);
        REQUIRE(guest.update(input) != OnlinePlay::Phase::Failed);
        if (host.entryVisible()) {
            host.render(hostDevice, Mat4{1}, 1280, 720, 1.0 / 60, 1);
        }
        if (guest.entryVisible()) {
            draw();
        }
    }
    usize discardThrowPackets() {
        return std::erase_if(wire.hostWire.events, [](const auto& event) {
            const auto commands = InputPacket::decode(event.bytes);
            return commands && std::ranges::any_of(*commands, [](const auto& command) {
                       return command.pressed(CommandPress::Attack);
                   });
        });
    }
};

struct StationaryActions {
    test::FakeRenderDevice device;
    WorldCollision floor;
    std::array<PlayerRuntime, 1> native;
    MatchContext context;
    CombatSnapshot baseline;
    LocalMovementPrediction prediction;

    StationaryActions() {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        floor.build(
            {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
             {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
        CharacterSave save;
        save.autoAttack = false;
        auto& runtime = native.front();
        runtime.actor.spawn(1, save, nullptr, Vec3{0}, 0);
        runtime.figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(runtime.figure);
        REQUIRE(runtime.figure->actionTree());
        FloorRiding::land(native, 0, runtime.actor.position(), floor);
        context.epoch = 7;
        context.scene = 1;
        context.owners = {1, 2, 0, 0};
        context.grants = {1, 2, 0, 0};
        baseline.motion.epoch = context.epoch;
        baseline.motion.tick = 10;
        baseline.motion.cameraContinuity = 1;
        baseline.motion.players[0] = SeatMotion{1, 1, {4, 0, 0}, 0};
        baseline.players[0].emplace().health = 100;
        baseline.motion.players[1] =
            SeatMotion{2, 1, runtime.actor.position(), runtime.actor.yaw()};
        auto& state = baseline.players[1].emplace();
        state.health = 100;
        const auto& animator = runtime.figure->animator();
        const auto& cursor = animator.player();
        state.animation = {static_cast<u32>(animator.action()), cursor.sequence(),
                           cursor.generation(), cursor.presentationFrame(), cursor.transition()};
        state.rollback = animator.checkpoint();
        state.transitionPose = animator.transitionPose();
        state.knockback = runtime.knockback.checkpoint();
        state.floor =
            PlayerFloorState{runtime.floor.object, runtime.floor.flags, runtime.floor.placement};
        state.movement.enabled = state.movement.current = state.movement.ranged = true;
        state.movement.radius = runtime.actor.radius();
        state.movement.height = runtime.actor.height();
        state.movement.followHeight = runtime.actor.followPoint().y - runtime.actor.position().y;
        state.movement.speed = runtime.actor.speed();
        auto& contacts = state.locomotion.emplace();
        contacts.origin = runtime.actor.position();
        contacts.radius = 8;
        contacts.ticks = 12;
        contacts.party.push_back({{4, 0, 0}, runtime.actor.radius(), runtime.actor.height()});
        reset();
    }
    void reset() {
        REQUIRE(baseline.valid());
        prediction.begin(context, 2, floor);
        REQUIRE(prediction.setPlayer(1, *native.front().figure->actionTree()));
        prediction.update(&baseline, {});
    }
    InputCommand input(u64 tick) const {
        InputCommand command;
        command.epoch = context.epoch;
        command.tick = tick;
        command.grant = context.grants[1];
        command.seat = 1;
        return command;
    }
    CombatSnapshot shown() const {
        auto state = baseline;
        prediction.present(state, 1);
        REQUIRE(state.valid());
        return state;
    }
};

TEST_CASE("stationary actions retain native facing and animation past dynamic contact expiry",
          "[netplay][rollback-integration][stationary-action][assets]") {
    const auto deed = GENERATE(PlayerDeed::Attack, PlayerDeed::StrongAttack, PlayerDeed::Defend);
    StationaryActions fixture;
    usize nativeCallbacks = 0;
    PartyMotion::Events events;
    events.select = [&](usize, const SelectorInput&, s32) { ++nativeCallbacks; };
    events.advanceTurbo = [&](usize, s32, f32) { ++nativeCallbacks; };
    events.perform = [&](usize, PartyMotion::Action) { ++nativeCallbacks; };
    for (u64 tick = 11; tick <= 25; ++tick) {
        auto command = fixture.input(tick);
        // Cursor-only facing changes are native zero-displacement actions, not
        // permission to lengthen the frozen collision response's movement window.
        if (tick >= 15) {
            command.aimDirection = Vec3{1, 0, 0};
        }
        if (tick == 25) {
            if (deed == PlayerDeed::Defend) {
                command.pressedButtons = static_cast<u32>(CommandPress::Defend);
            } else {
                command.heldButtons = static_cast<u32>(
                    deed == PlayerDeed::Attack ? CommandHeld::Attack : CommandHeld::StrongAttack);
            }
        }
        std::array<PlayInput, InputCommand::kSeats> inputs;
        inputs[1] = SessionInputs::playInput(command);
        PartyMotion::step(fixture.native, inputs, false, 0, 1, 1.0f / 60, fixture.floor, events);
        const auto& runtime = fixture.native.front();
        const auto inventory = runtime.actor.save().progress().inventory;
        const auto nativeAnimation = runtime.figure->animator().checkpoint();
        const auto callbacks = nativeCallbacks;
        fixture.prediction.update(&fixture.baseline, std::array{command});
        CAPTURE(deed, tick);
        REQUIRE(fixture.prediction.replayed(1));
        const auto shown = fixture.shown();
        CHECK(shown.motion.players[1]->position == fixture.baseline.motion.players[1]->position);
        CHECK(shown.motion.players[1]->position == runtime.actor.position());
        CHECK(std::remainder(shown.motion.players[1]->yaw - runtime.actor.yaw(), kTwoPi) ==
              Catch::Approx(0).margin(0.0001));
        CHECK(shown.players[1]->rollback == nativeAnimation);
        CHECK(shown.players[1]->transitionPose == runtime.figure->animator().transitionPose());
        CHECK(shown.players[1]->knockback == runtime.knockback.checkpoint());
        CHECK(shown.players[1]->health == fixture.baseline.players[1]->health);
        CHECK(shown.projectiles.empty());
        CHECK(runtime.actor.save().progress().inventory == inventory);
        CHECK(runtime.figure->animator().checkpoint() == nativeAnimation);
        CHECK(nativeCallbacks == callbacks);
    }
    CHECK(fixture.baseline.players[1]->locomotion->ticks == 12);
    CHECK(fixture.prediction.replayed(1)->endTick == 25);
    CHECK(fixture.shown().players[1]->rollback != fixture.baseline.players[1]->rollback);
}

TEST_CASE("stationary action fallback cannot ignore contacts or replay a moving prefix",
          "[netplay][rollback-integration][stationary-action][assets]") {
    StationaryActions fixture;
    auto& state = *fixture.baseline.players[1];
    auto& contacts = *state.locomotion;
    bool moved = false;
    SECTION("coincident zero-length party contact still queues a native shove") {
        contacts.party.front().base = fixture.native.front().actor.position();
        std::array<PlayerRuntime, 2> party;
        for (auto& runtime : party) {
            runtime.actor.restoreMotion(Vec3{0}, 0, 0, state.movement.radius, state.movement.height,
                                        0);
        }
        const auto before = party[1].knockback.checkpoint();
        REQUIRE(PartyCollision::step(party, 0, Vec3{0}, 1.0f / 60, &fixture.floor));
        CHECK(party[0].actor.position() == Vec3{0});
        CHECK(party[1].knockback.checkpoint() != before);
    }
    SECTION("tangent party contact also remains authoritative") {
        contacts.party.front().base = {state.movement.radius + contacts.party.front().radius, 0, 0};
    }
    SECTION("zero-length body sweep cannot prove an overlapping enemy is clear") {
        contacts.bodies.push_back({Vec3{0}, state.movement.radius, state.movement.height});
    }
    SECTION("generator overlap cannot be skipped after expiry") {
        contacts.generators.push_back({Vec3{0}, 0, 1, 1, 2});
    }
    SECTION("post-action fixture push cannot be skipped after expiry") {
        contacts.fixtures.push_back({Vec3{0}, 0, 1, 1, 2});
    }
    SECTION("even one movement input keeps the complete prefix bounded") {
        moved = true;
    }
    SECTION("automatic melee does not become an ordinary throwing permission") {
        contacts.automatic = true;
    }
    SECTION("queued impulse is not a stationary action") {
        state.knockback->shoved = true;
    }
    fixture.reset();
    for (u64 tick = 11; tick <= 25; ++tick) {
        auto command = fixture.input(tick);
        if (moved && tick == 11) {
            command.direction = {0, 1};
            command.magnitude = 1;
        }
        if (tick == 25) {
            command.heldButtons = static_cast<u32>(CommandHeld::Attack);
        }
        fixture.prediction.update(&fixture.baseline, std::array{command});
    }
    CHECK_FALSE(fixture.prediction.replayed(1));
    const auto shown = fixture.shown();
    CHECK(shown.players[1]->rollback == state.rollback);
    CHECK(shown.motion.players[1]->position == fixture.baseline.motion.players[1]->position);
    CHECK(shown.projectiles.empty());
    CHECK(contacts.ticks == 12);
}

TEST_CASE("native rollback starts the owning guest throw before host acknowledgement",
          "[netplay][rollback-integration][assets]") {
    const u64 latency = GENERATE(0, 6);
    RollbackPair pair;
    pair.wire.hostWire.latency = latency;
    pair.wire.guestWire.latency = latency;
    // Let receipt feedback establish the delayed path's input lead before the
    // one-shot attack. This test measures local replay, not startup packet loss.
    for (s32 tick = 0; tick < 90; ++tick) {
        pair.step();
    }
    pair.draw();
    // No new authority may arrive while this input's immediate response is
    // checked. Subsequent snapshots must recover the deliberately dropped ones.
    pair.wire.guestWire.events.clear();
    pair.wire.guestWire.delayed.clear();
    const u64 hostTick = pair.wire.host.tick();
    const u64 checkpoint = pair.wire.guest.playback().latest()->motion.tick;
    REQUIRE_FALSE(throwing(*pair.guest.shown()->players[2]));
    SessionInputs::Frame input;
    input[2].attack = input[2].attackPressed = true; // second local device -> room seat 2
    REQUIRE(pair.guest.update(input) != OnlinePlay::Phase::Failed);
    pair.draw();
    const auto& localBaseline = *pair.wire.guest.playback().latest()->players[2];
    const auto& baselineMotion = *pair.wire.guest.playback().latest()->motion.players[2];
    const auto sampled = std::ranges::find_if(
        pair.wire.guest.sampledInputs(), [](const auto& command) { return command.seat == 2; });
    REQUIRE(sampled != pair.wire.guest.sampledInputs().end());
    INFO("latency=" << latency << " host=" << hostTick << " checkpoint=" << checkpoint
                    << " sampled=" << sampled->tick << " prefix=" << sampled->tick - checkpoint
                    << " enabled=" << localBaseline.movement.enabled << " current="
                    << localBaseline.movement.current << " ranged=" << localBaseline.movement.ranged
                    << " contacts=" << localBaseline.locomotion.has_value() << " contactTicks="
                    << (localBaseline.locomotion ? localBaseline.locomotion->ticks : 0)
                    << " knockbackTicks=" << localBaseline.knockbackTicks
                    << " pending=" << pair.guest.predictionMetrics().pending
                    << " stalled=" << pair.guest.predictionMetrics().stalled);
    f32 smallestPartyClearance = 1'000'000;
    if (localBaseline.locomotion) {
        for (const auto& other : localBaseline.locomotion->party) {
            smallestPartyClearance = std::min(smallestPartyClearance,
                                              glm::distance(baselineMotion.position, other.base) -
                                                  localBaseline.movement.radius - other.radius);
        }
    }
    INFO("smallest party clearance=" << smallestPartyClearance);
    CHECK(pair.wire.host.tick() == hostTick);
    CHECK(pair.wire.guest.playback().latest()->motion.tick == checkpoint);
    CHECK(throwing(*pair.guest.shown()->players[2]));
    CHECK_FALSE(throwing(*pair.guest.shown()->players[0]));
    CHECK_FALSE(throwing(*pair.guest.shown()->players[1]));
    CHECK_FALSE(throwing(*pair.host.shown()->players[2]));
    CHECK_FALSE(playerProjectile(*pair.host.shown()));

    bool hostThrow = false;
    bool hostProjectile = false;
    bool guestConfirmedThrow = false;
    bool guestProjectile = false;
    bool guestPredictedProjectile = false;
    bool hostIdentifiedProjectile = false;
    std::vector<ProjectileKey> launched;
    for (s32 tick = 0; tick < 90; ++tick) {
        pair.step();
        pair.draw();
        std::set<std::pair<ShotIdentity, ProjectileSource>> components;
        for (const auto& shot : pair.guest.shown()->projectiles) {
            if (shot.rollback) {
                CHECK(components.emplace(shot.rollback->origin, shot.source).second);
                guestPredictedProjectile = guestPredictedProjectile ||
                                           shot.instance >= PredictedShotLedger::kPrivateInstance;
            }
        }
        for (const auto& shot : pair.host.shown()->projectiles) {
            if (shot.rollback && shot.rollback->origin.seat == 2) {
                hostIdentifiedProjectile = true;
                CHECK(shot.rollback->origin.tick <= pair.host.shown()->motion.tick);
                CHECK(shot.rollback->origin.grant == pair.wire.host.context().grants[2]);
            }
        }
        hostThrow = hostThrow || throwing(*pair.host.shown()->players[2]);
        const auto& missiles = pair.host.hostScene()->missiles();
        if (const auto key = missileKey(missiles, 2)) {
            if (std::ranges::find(launched, *key) == launched.end()) {
                launched.push_back(*key);
            }
            hostProjectile = hostProjectile || std::ranges::any_of(pair.host.shown()->projectiles,
                                                                   [&](const auto& shot) {
                                                                       return shot.key() == *key;
                                                                   });
        }
        guestProjectile = guestProjectile ||
                          std::ranges::any_of(pair.wire.guest.playback().latest()->projectiles,
                                              [&](const auto& shot) {
                                                  return std::ranges::find(launched, shot.key()) !=
                                                         launched.end();
                                              });
        guestConfirmedThrow =
            guestConfirmedThrow || throwing(*pair.wire.guest.playback().latest()->players[2]);
        CHECK_FALSE(throwing(*pair.guest.shown()->players[0]));
        CHECK_FALSE(throwing(*pair.guest.shown()->players[1]));
    }
    CAPTURE(latency);
    CHECK(hostThrow);
    CHECK_FALSE(launched.empty());
    CHECK(hostProjectile);
    CHECK(guestProjectile);
    CHECK(guestConfirmedThrow);
    CHECK(hostIdentifiedProjectile);
    CHECK(guestPredictedProjectile);
    CHECK_FALSE(throwing(*pair.guest.shown()->players[2]));
    CHECK_FALSE(pair.guest.hostScene());
}

TEST_CASE("lost guest attack predictions reconcile without inventing authoritative projectiles",
          "[netplay][rollback-integration][assets]") {
    RollbackPair pair;
    SessionInputs::Frame input;
    input[0].attack = input[0].attackPressed = true;
    REQUIRE(pair.guest.update(input) != OnlinePlay::Phase::Failed);
    pair.draw();
    CHECK(throwing(*pair.guest.shown()->players[1]));
    const auto command = pair.wire.guest.sampledInputs().front();
    REQUIRE(command.pressed(CommandPress::Attack));
    usize dropped = pair.discardThrowPackets();
    bool resolved = false;
    for (s32 tick = 0; tick < 90; ++tick) {
        pair.step();
        dropped += pair.discardThrowPackets();
        pair.draw();
        CHECK_FALSE(throwing(*pair.host.shown()->players[1]));
        CHECK_FALSE(playerProjectile(*pair.host.shown()));
        const auto* authority = pair.wire.guest.playback().latest();
        if (authority->motion.tick >= command.tick) {
            resolved = true;
            CHECK_FALSE(throwing(*pair.guest.shown()->players[1]));
        }
    }
    CHECK(dropped >= InputHistory::kTicks);
    CHECK(resolved);
    CHECK_FALSE(playerProjectile(*pair.guest.shown()));
    CHECK_FALSE(pair.guest.hostScene());
}
} // namespace
