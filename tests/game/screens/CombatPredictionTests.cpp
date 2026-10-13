#include <array>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/CombatPrediction.h"
#include "game/screens/OnlineCombatReplay.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(sizeof(OnlineCombatReplay) < 4096,
              "Retained native checkpoints must not accumulate on session stacks");

TEST_CASE("private native combat replay commits and corrects the complete ordered party",
          "[rollback][combat-prediction][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    // Network simulation is fixed at 60 Hz; the native runner separately
    // covers one- and two-tick updates independently of render frequency.
    constexpr u16 rate = 60;
    const auto directory = test::sampleLevel("private-combat-empty-floor");
    writeTextFile(directory / "world.json", R"({"objects":[
      {"name":"FLOOR","position":[0,0,0],"flags":4}]})");
    writeTextFile(directory / "collision.json", R"({"objects":[
      {"object":0,"normals":[0,1,0,0,1,0],"vertices":[
      -200,0,-200,200,0,-200,200,0,200,-200,0,-200,200,0,200,-200,0,200]}]})");
    test::FakeRenderDevice device;
    MatchContext context;
    context.epoch = 3;
    context.scene = 1;
    context.tickRate = rate;
    context.owners = {0, 1, 0, 2};
    context.grants = {0, 2, 0, 5};
    PartyBootstrap::Party profiles;
    for (const s32 seat : std::array<s32, 2>{1, 3}) {
        CharacterSave save;
        save.name = "TEST";
        save.character = 0;
        save.color = seat;
        save.autoAttack = false;
        profiles[static_cast<usize>(seat)] = CharacterProfile::capture(save);
    }
    CombatPrediction host, guest;
    const LevelRef level{.name = "TEST", .directory = directory.string()};
    REQUIRE(host.open(device, root, level, context, profiles, 1));
    REQUIRE(guest.open(device, root, level, context, profiles, 1));
    auto seed = host.checkpoint();
    REQUIRE(seed);
    REQUIRE(seed->combat.players.size() == 2);
    seed->combat.players[0].actor.position = {-12, 0, 0};
    seed->combat.players[1].actor.position = {12, 0, 0};
    REQUIRE(host.begin(context.epoch, 100, *seed));
    REQUIRE(guest.begin(context.epoch, 100, *seed));
    REQUIRE(host.world()->collision().floorAt({-12, 0, 0}, 0.5f, 1));
    REQUIRE(host.world()->collision().floorAt({12, 0, 0}, 0.5f, 1));
    const auto textures = device.texturesCreated;
    const auto updates = device.textureUpdates;
    OrdinaryCombatTick::Frame frame;
    frame.ticks = 60 / rate;
    frame.seconds = 1.0f / static_cast<f32>(rate);
    frame.camera.position = {0, 35, -50};
    frame.camera.pitch = 0.6f;
    frame.projection.horizontalFov = 1.6f;
    frame.projection.aspect = 2;
    MotionSnapshot motion;
    motion.epoch = context.epoch;
    motion.tick = 100;
    motion.cameraContinuity = 1;
    motion.camera = frame.camera;
    motion.horizontalFov = frame.projection.horizontalFov;
    motion.aspect = frame.projection.aspect;
    for (const auto& player : host.players()) {
        const auto seat = static_cast<usize>(player.actor.player());
        motion.players[seat] =
            SeatMotion{context.grants[seat], 1, player.actor.position(), player.actor.yaw()};
    }
    auto authority =
        CombatCapture::capture(motion, host.players(), *host.enemies(), nullptr, host.world());
    REQUIRE(authority);
    authority->opponents = seed->opponents;
    OpponentFrame opponentFrame;
    opponentFrame.ticks = frame.ticks;
    opponentFrame.seconds = frame.seconds;
    opponentFrame.initialPlayers = 2;
    const auto views = LevelOpponents::enemyViews(host.players());
    for (usize i = 0; i < host.players().size(); ++i) {
        opponentFrame.players.push_back(
            {static_cast<u8>(host.players()[i].actor.player()), views[i]});
    }
    authority->opponentFrame = opponentFrame;
    authority->combatState = CombatStatePacket::State{seed->combat, seed->adapter};
    authority->combatFrame = frame;
    REQUIRE(authority->valid());
    const auto bytes = CombatPacket::encode(*authority);
    REQUIRE(bytes);
    const auto received = CombatPacket::decode(*bytes);
    REQUIRE(received);
    {
        OnlineCombatReplay replay;
        REQUIRE(replay.begin(context, 2, guest));
        InputCommand command;
        command.epoch = context.epoch;
        command.tick = 101;
        command.grant = context.grants[3];
        command.seat = 3;
        command.direction = {1, 0};
        command.magnitude = 1;
        std::array<PlayInput, 4> direct{};
        direct[3].move.direction = {1, 0};
        direct[3].move.magnitude = 1;
        const auto native = host.advance(101, direct, frame);
        CAPTURE(static_cast<int>(native.status));
        REQUIRE(native.status == OrdinaryCombatTick::Status::Advanced);
        host.resetReplay();
        REQUIRE(host.begin(context.epoch, 100, *seed));
        REQUIRE(replay.update(&*received, std::span{&command, 1}));
        REQUIRE(replay.active());
        CHECK(replay.tick() == 101);
        const auto result = guest.checkpoint();
        REQUIRE(result);
        CHECK(result->combat.players[0].actor.position == seed->combat.players[0].actor.position);
        CHECK(result->combat.players[1].actor.position != seed->combat.players[1].actor.position);
        // Repeated publication does not execute a tick twice; input 101 is
        // replayed from authority 100 and produces the same complete state.
        REQUIRE(replay.update(&*received, std::span{&command, 1}));
        CHECK(guest.checkpoint() == result);
        auto display = *received;
        display.opponents.reset();
        display.opponentFrame.reset();
        display.combatState.reset();
        display.combatFrame.reset();
        EnemyPresentation presentation;
        presentation.epoch = context.epoch;
        presentation.tick = display.motion.tick;
        REQUIRE(replay.present(display, presentation, 1));
        CHECK(display.motion.players[3]->position == result->combat.players[1].actor.position);
        CHECK(display.motion.players[1]->position == received->motion.players[1]->position);
        CHECK(display.motion.players[1]->yaw == received->motion.players[1]->yaw);
        CHECK(display.valid());
        CHECK(display.players[3]->rollback == result->combat.players[1].figure->animation);
        CHECK_FALSE(display.players[3]->movement.enabled);
        CHECK_FALSE(display.players[3]->rollbackInput);
        // A newer local animation must remain valid even when drawing against
        // an older checkpoint carrying different transition permissions.
        auto directDisplay = *received;
        REQUIRE(replay.present(directDisplay, presentation, 0.5f));
        CHECK(directDisplay.valid());
        CHECK_FALSE(directDisplay.combatState);
        CHECK_FALSE(directDisplay.opponents);
        for (const int boundary : {0, 1, 2, 3}) {
            auto refused = *received;
            if (boundary == 0) {
                ++refused.motion.cameraContinuity;
            } else if (boundary == 1) {
                ++refused.motion.players[3]->continuity;
            } else if (boundary == 2) {
                ++refused.motion.players[1]->continuity;
            } else {
                refused.players[3]->movement = {};
                refused.players[3]->portalPhase = 0.5f;
            }
            const auto unchanged = CombatPacket::encode(refused);
            REQUIRE(unchanged);
            CHECK_FALSE(replay.present(refused, presentation, 1));
            CHECK(CombatPacket::encode(refused) == unchanged);
        }
        ++command.tick;
        command.pressedButtons = static_cast<u32>(CommandPress::Attack);
        REQUIRE(replay.update(&*received, std::span{&command, 1}));
        CHECK(replay.active());
        CHECK(replay.tick() == command.tick);
        // Ordinary throws are now replayable. A potion still crosses the
        // bounded combat phase and must undo the entire accepted replay pass.
        ++command.tick;
        command.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
        CHECK_FALSE(replay.update(&*received, std::span{&command, 1}));
        CHECK_FALSE(replay.active());
        CHECK(guest.checkpoint() == seed);
        replay.clear();
        command.tick = 101;
        command.pressedButtons = 0;
        for (const int hold : {0, 1, 2, 3}) {
            auto held = *received;
            if (hold == 0) {
                held.opponentFrame->held = true;
            } else if (hold == 1) {
                held.opponentFrame->timeStopped = true;
            } else if (hold == 2) {
                held.combatFrame->damage.cutscene = true;
            } else {
                held.players[3]->movement = {};
                held.players[3]->portalPhase = 0.5f;
            }
            REQUIRE(held.valid());
            REQUIRE(replay.begin(context, 2, guest));
            CHECK_FALSE(replay.update(&held, std::span{&command, 1}));
            CHECK_FALSE(replay.active());
            CHECK(guest.checkpoint() == seed);
            replay.clear();
        }
    }
    REQUIRE(guest.begin(context.epoch, 100, *seed));
    std::array<PlayInput, 4> input{};
    input[1].move.direction = {0, 1};
    input[1].move.magnitude = 1;
    input[3].move.direction = {1, 0};
    input[3].move.magnitude = 1;
    for (u64 tick = 101; tick <= 110; ++tick) {
        frame.damage.playSeconds = static_cast<f32>(tick - 100) * frame.seconds;
        REQUIRE(host.advance(tick, input, frame).status == OrdinaryCombatTick::Status::Advanced);
        REQUIRE(guest.advance(tick, input, frame).status == OrdinaryCombatTick::Status::Advanced);
        CHECK(guest.checkpoint() == host.checkpoint());
    }
    const auto predicted = guest.checkpoint();
    REQUIRE(predicted);
    CHECK(predicted->combat.players[0].actor.position != seed->combat.players[0].actor.position);
    CHECK(predicted->combat.players[1].actor.position != seed->combat.players[1].actor.position);
    // Refusing an unsupported potion undoes every accepted tick in this replay pass.
    input[1].usePotion = true;
    CHECK(guest.advance(111, input, frame).status == OrdinaryCombatTick::Status::Unsupported);
    CHECK_FALSE(guest.active());
    CHECK(guest.checkpoint() == seed);
    CHECK(host.checkpoint() == predicted);
    REQUIRE(guest.begin(context.epoch, 110, *predicted));
    CHECK_FALSE(guest.begin(context.epoch, 109, *seed));
    CHECK(guest.checkpoint() == predicted);
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == updates);
    CHECK_FALSE(guest.open(device, root, level, context, profiles, 1));
    auto resumed = context;
    ++resumed.epoch;
    resumed.transition = MatchTransition::Resume;
    REQUIRE(guest.resume(resumed));
    CHECK_FALSE(guest.active());
    CHECK(guest.tick() == 0);
    CHECK_FALSE(guest.begin(context.epoch, 110, *predicted));
    REQUIRE(guest.begin(resumed.epoch, 1, *predicted));
}

TEST_CASE("private combat loading preserves native level and ordered party ownership",
          "[rollback][combat-prediction][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto stage = GENERATE("L1", "G1");
    constexpr u16 tickRate = 60;
    CAPTURE(stage, tickRate);
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(stage);
    REQUIRE(level);
    MatchContext context;
    context.epoch = 7;
    context.scene = 1;
    context.tickRate = tickRate;
    context.owners = {0, 1, 0, 2};
    context.grants = {0, 2, 0, 5};
    REQUIRE(context.valid());
    PartyBootstrap::Party profiles;
    for (const s32 seat : std::array<s32, 2>{1, 3}) {
        CharacterSave save;
        save.name = "TEST";
        save.character = seat;
        save.color = seat;
        profiles[static_cast<usize>(seat)] = CharacterProfile::capture(save);
        REQUIRE(profiles[static_cast<usize>(seat)]);
    }
    CombatPrediction first, second;
    REQUIRE(first.open(device, root, *level, context, profiles, 1));
    REQUIRE(second.open(device, root, *level, context, profiles, 1));
    REQUIRE(first.loaded());
    REQUIRE(second.loaded());
    REQUIRE(first.world());
    REQUIRE(second.world());
    CHECK(first.world() != second.world());
    CHECK(first.world()->layout().itemInstances().size() ==
          second.world()->layout().itemInstances().size());
    CHECK_FALSE(first.active());
    CHECK(first.players().empty());

    // Dormant native statues/stocks stay in these worlds. Loading must not
    // manufacture an ordinary-only roster to make the transaction eligible.
    const auto authority = first.checkpoint();
    REQUIRE(authority);
    REQUIRE(authority->combat.players.size() == 2);
    CHECK(authority->combat.players[0].player == 1);
    CHECK(authority->combat.players[1].player == 3);
    REQUIRE(second.begin(context.epoch, 100, *authority));
    CHECK(second.checkpoint() == authority);
    REQUIRE(second.players().size() == 2);
    CHECK(second.players()[0].figure.get() != nullptr);
    const auto created = device.texturesCreated;
    const auto updated = device.textureUpdates;
    for (s32 attempt = 0; attempt < 3; ++attempt) {
        REQUIRE(second.begin(context.epoch, 100, *authority));
        OrdinaryCombatTick::Frame frame;
        frame.ticks = 60 / tickRate;
        frame.seconds = static_cast<f32>(frame.ticks) / 60;
        const std::array<PlayInput, 4> inputs{};
        // These authored worlds contain uncaptured interacting fixtures. Their
        // refusal is intentional; this test proves independent restoration, not
        // complete live-level combat prediction.
        CHECK(second.advance(101, inputs, frame).status != OrdinaryCombatTick::Status::Advanced);
        CHECK_FALSE(second.active());
        CHECK(second.tick() == 100);
        CHECK(second.players().empty());
        CHECK(second.checkpoint() == authority);
        CHECK(first.checkpoint() == authority);
    }
    CHECK(device.texturesCreated == created);
    CHECK(device.textureUpdates == updated);
    REQUIRE(second.begin(context.epoch, 100, *authority));
    CHECK_FALSE(second.begin(context.epoch - 1, 101, *authority));
    CHECK_FALSE(second.active());
    CHECK(second.checkpoint() == authority);
    auto malformed = *authority;
    malformed.combat.players[1].player = 1;
    CHECK_FALSE(second.begin(context.epoch, 101, malformed));
    CHECK(second.checkpoint() == authority);
    const auto* prior = second.world();
    auto incomplete = profiles;
    incomplete[3].reset();
    CHECK_FALSE(second.open(device, root, *level, context, incomplete, 1));
    CHECK(second.world() == prior);
    CHECK(second.checkpoint() == authority);
    second.clear();
    CHECK_FALSE(second.loaded());
    CHECK_FALSE(second.active());
    CHECK_FALSE(second.checkpoint());
}

} // namespace
