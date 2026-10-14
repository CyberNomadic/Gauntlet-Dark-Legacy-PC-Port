#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/InputTimeline.h"
#include "game/netplay/StageStatePacket.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/CombatPrediction.h"
#include "game/screens/OnlineCombatReplay.h"
#include "game/screens/SessionInputs.h"
#include "game/screens/StageParticlePresentation.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(sizeof(OnlineCombatReplay) < 4096,
              "Retained native checkpoints must not accumulate on session stacks");

using NativeState = OrdinaryCombatCheckpoint::State;

std::unique_ptr<NativeState> checkpoint(const CombatPrediction& prediction,
                                        std::optional<bool> includeStage = std::nullopt) {
    auto state = includeStage ? prediction.checkpoint(*includeStage) : prediction.checkpoint();
    return state ? std::make_unique<NativeState>(std::move(*state)) : nullptr;
}

bool matches(const CombatPrediction& prediction, const NativeState& expected) {
    const auto actual = checkpoint(prediction);
    return actual && *actual == expected;
}

bool matches(const CombatPrediction& first, const CombatPrediction& second) {
    const auto expected = checkpoint(second);
    return expected && matches(first, *expected);
}

std::unique_ptr<CombatSnapshot> capture(const MotionSnapshot& motion,
                                        const CombatPrediction& prediction) {
    auto state = CombatCapture::capture(motion, prediction.players(), *prediction.enemies(),
                                        nullptr, prediction.world());
    return state ? std::make_unique<CombatSnapshot>(std::move(*state)) : nullptr;
}

std::unique_ptr<CombatSnapshot> decode(std::span<const u8> bytes) {
    auto state = CombatPacket::decode(bytes);
    return state ? std::make_unique<CombatSnapshot>(std::move(*state)) : nullptr;
}

TEST_CASE("private native combat replay commits and corrects the complete ordered party",
          "[rollback][combat-prediction][incremental-combat-replay][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const bool includeStage = GENERATE(false, true);
    CAPTURE(includeStage);
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
    const LevelRef level{.name = "TEST", .directory = directory.string(), .items = "ITEMS/levelG"};
    REQUIRE(host.open(device, root, level, context, profiles, 1));
    REQUIRE(guest.open(device, root, level, context, profiles, 1));
    auto seed = checkpoint(host, includeStage);
    REQUIRE(seed);
    REQUIRE(seed->stage.has_value() == includeStage);
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
    auto authority = capture(motion, host);
    REQUIRE(authority);
    if (includeStage) {
        authority->geometry = host.world()->scene().geometry();
    }
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
    if (seed->stage) {
        const auto catalog = StageStatePacket::Catalog::build(*seed->stage);
        REQUIRE(catalog);
        authority->combatState->stage = StageStatePacket::encode(*seed->stage, *catalog);
        REQUIRE(authority->combatState->stage);
        CHECK(guest.decodeStage(*authority->combatState->stage) == seed->stage);
    }
    authority->combatFrame = frame;
    REQUIRE(authority->valid());
    const auto bytes = CombatPacket::encode(*authority);
    REQUIRE(bytes);
    const auto received = decode(*bytes);
    REQUIRE(received);
    {
        OnlineCombatReplay replay;
        const auto domain = static_cast<usize>(includeStage ? OnlineCombatReplay::Domain::Stage
                                                            : OnlineCombatReplay::Domain::Legacy);
        const auto commandAt = [&](u64 tick) {
            InputCommand command;
            command.epoch = context.epoch;
            command.tick = tick;
            command.grant = context.grants[3];
            command.seat = 3;
            command.direction = {1, 0};
            command.magnitude = 0.5f;
            return command;
        };
        const auto verifyFullReplay = [&](std::span<const InputCommand> sent, u64 end) {
            RollbackInputHistory history;
            REQUIRE(history.begin(context.epoch, {0, 0, 0, context.grants[3]}));
            REQUIRE(history.resolve(context.epoch, 100));
            for (const auto& command : sent) {
                REQUIRE(history.record(std::span{&command, 1}));
            }
            const auto inputs = history.replay(3, received->players[3]->rollbackInput);
            REQUIRE(inputs.size() == end - 100);
            REQUIRE(host.begin(context.epoch, 100, *seed));
            auto nativeFrame = *received->combatFrame;
            for (const auto& entry : inputs) {
                SessionInputs::Frame nativeInputs{};
                nativeInputs[1] = SessionInputs::playInput(
                    InputTimeline::heldOrNeutral(received->players[1]->rollbackInput, context.epoch,
                                                 entry.input.tick, context.grants[1], 1));
                nativeInputs[3] = SessionInputs::playInput(entry.input);
                REQUIRE(host.advance(entry.input.tick, nativeInputs, nativeFrame).status ==
                        OrdinaryCombatTick::Status::Advanced);
                nativeFrame.damage.playSeconds += nativeFrame.seconds;
            }
            CHECK(matches(guest, host));
        };
        REQUIRE(replay.begin(context, 2, guest));
        std::vector<InputCommand> sent{commandAt(101)};
        REQUIRE(replay.update(received.get(), sent));
        verifyFullReplay(sent, 101);
        const auto revision = guest.replayRevision();
        const auto firstPicture = [&] {
            auto shown = std::make_unique<CombatSnapshot>(*received);
            EnemyPresentation presentation;
            presentation.epoch = context.epoch;
            presentation.tick = 100;
            presentation.shownGeometry = shown->geometry;
            REQUIRE(replay.present(*shown, presentation, 0.5f));
            return std::pair{CombatPacket::encode(*shown), presentation.particles};
        }();
        REQUIRE(firstPicture.first);
        REQUIRE(replay.update(received.get(), sent));
        REQUIRE(replay.update(nullptr, {}));
        CHECK(guest.replayRevision() == revision);
        CHECK(replay.diagnostics().domains[domain].advancedTicks == 1);
        CHECK(replay.diagnostics().domains[domain].decodeAttempts == (includeStage ? 1 : 0));
        {
            auto shown = std::make_unique<CombatSnapshot>(*received);
            EnemyPresentation presentation;
            presentation.epoch = context.epoch;
            presentation.tick = 100;
            presentation.shownGeometry = shown->geometry;
            REQUIRE(replay.present(*shown, presentation, 0.5f));
            CHECK(CombatPacket::encode(*shown) == firstPicture.first);
            CHECK(presentation.particles == firstPicture.second);
        }
        for (u64 tick = 102; tick <= 110; ++tick) {
            sent.push_back(commandAt(tick));
            sent.back().direction = tick % 2 == 0 ? Vec2{0, 1} : Vec2{1, 0};
            REQUIRE(replay.update(nullptr, std::span{&sent.back(), 1}));
            CHECK(replay.diagnostics().domains[domain].advancedTicks == tick - 100);
            verifyFullReplay(sent, tick);
        }
        // A batch append must capture the penultimate NEW tick, not reuse the
        // old current picture as if only one tick had been appended.
        for (u64 tick = 111; tick <= 113; ++tick) {
            sent.push_back(commandAt(tick));
        }
        REQUIRE(replay.update(nullptr, std::span{sent}.last(3)));
        CHECK(replay.diagnostics().domains[domain].advancedTicks == 13);
        verifyFullReplay(sent, 113);
        const auto incremental = checkpoint(guest);
        REQUIRE(incremental);
        auto shown = std::make_unique<CombatSnapshot>(*received);
        EnemyPresentation presentation;
        presentation.epoch = context.epoch;
        presentation.tick = 100;
        presentation.shownGeometry = shown->geometry;
        REQUIRE(replay.present(*shown, presentation, 0.5f));
        const auto incrementalPicture = CombatPacket::encode(*shown);
        REQUIRE(incrementalPicture);
        // Deliberately use the public owner outside the coordinator, at the
        // SAME endpoint. Equal ticks cannot authorize incremental continuation.
        REQUIRE(guest.begin(context.epoch, 100, *seed));
        SessionInputs::Frame neutral{};
        auto otherFrame = frame;
        for (u64 tick = 101; tick <= 113; ++tick) {
            REQUIRE(guest.advance(tick, neutral, otherFrame).status ==
                    OrdinaryCombatTick::Status::Advanced);
            otherFrame.damage.playSeconds += otherFrame.seconds;
        }
        CHECK_FALSE(replay.active());
        REQUIRE(replay.update(nullptr, {}));
        CHECK(replay.diagnostics().domains[domain].advancedTicks == 26);
        CHECK(matches(guest, *incremental));
        shown = std::make_unique<CombatSnapshot>(*received);
        presentation = {};
        presentation.epoch = context.epoch;
        presentation.tick = 100;
        presentation.shownGeometry = shown->geometry;
        REQUIRE(replay.present(*shown, presentation, 0.5f));
        CHECK(CombatPacket::encode(*shown) == incrementalPicture);
        // One accepted append before a later unsupported action must rewind
        // all earlier updates too, not only the append batch's starting point.
        std::array refused{commandAt(114), commandAt(115)};
        refused[1].pressedButtons = static_cast<u32>(CommandPress::UsePotion);
        CHECK_FALSE(replay.update(nullptr, refused));
        CHECK_FALSE(replay.active());
        CHECK(matches(guest, *seed));

        // A received frame can fill a concealed gap inside the executed prefix.
        REQUIRE(replay.begin(context, 2, guest));
        sent = {commandAt(101), commandAt(106)};
        REQUIRE(replay.update(received.get(), sent));
        CHECK(replay.diagnostics().domains[domain].advancedTicks == 6);
        auto gap = commandAt(102); // currently identical to native concealment
        REQUIRE(replay.update(nullptr, std::span{&gap, 1}));
        sent.push_back(gap);
        CHECK(replay.diagnostics().domains[domain].advancedTicks == 6);
        gap = commandAt(103);
        gap.aimDirection = Vec3{0, 0, -1};
        gap.direction = {0, -1};
        REQUIRE(replay.update(nullptr, std::span{&gap, 1}));
        sent.push_back(gap);
        CHECK(replay.diagnostics().domains[domain].advancedTicks == 12);
        verifyFullReplay(sent, 106);
        gap = commandAt(114); // crosses the six-tick held-input expiration
        REQUIRE(replay.update(nullptr, std::span{&gap, 1}));
        sent.push_back(gap);
        CHECK(replay.diagnostics().domains[domain].advancedTicks == 20);
        verifyFullReplay(sent, 114);

        // Early admission failures used to be cleaned up by entry-time reset.
        // Each must now explicitly clear the retained successful picture.
        for (const int invalid : {0, 1, 2, 3}) {
            REQUIRE(replay.begin(context, 2, guest));
            auto input = commandAt(101);
            REQUIRE(replay.update(received.get(), std::span{&input, 1}));
            if (invalid == 0) {
                ++input.grant;
            } else if (invalid == 1) {
                ++input.epoch;
            } else if (invalid == 2) {
                input.direction = {-1, 0}; // conflicting actual tick, not a concealed gap
            } else {
                input.tick = 221; // bound remains measured from authority, not last append
            }
            CHECK_FALSE(replay.update(nullptr, std::span{&input, 1}));
            CHECK_FALSE(replay.active());
            CHECK(matches(guest, *seed));
        }
        REQUIRE(replay.begin(context, 2, guest));
        const auto first = commandAt(101);
        REQUIRE(replay.update(received.get(), std::span{&first, 1}));
        for (s32 repeat = 0; repeat < 30; ++repeat) {
            REQUIRE(replay.update(nullptr, {}));
        }
        CHECK_FALSE(replay.update(nullptr, {}));
        CHECK_FALSE(replay.active());
        CHECK(replay.diagnostics().domains[domain].advancedTicks == 1);
        CHECK(replay.diagnostics()
                  .domains[domain]
                  .reasons[static_cast<usize>(OnlineCombatReplay::Refusal::StaleAuthority)] == 1);
        CHECK(matches(guest, *seed));
        replay.clear();
        REQUIRE(host.begin(context.epoch, 100, *seed));
    }
    {
        OnlineCombatReplay replay;
        REQUIRE(replay.begin(context, 2, guest));
        const auto domain = static_cast<usize>(includeStage ? OnlineCombatReplay::Domain::Stage
                                                            : OnlineCombatReplay::Domain::Legacy);
        const auto otherDomain = static_cast<usize>(
            includeStage ? OnlineCombatReplay::Domain::Legacy : OnlineCombatReplay::Domain::Stage);
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
        const bool replayed = replay.update(&*received, std::span{&command, 1});
        for (usize reason = 0; reason < OnlineCombatReplay::kRefusals.size(); ++reason) {
            if (replay.diagnostics().domains[domain].reasons[reason] != 0) {
                UNSCOPED_INFO(OnlineCombatReplay::kRefusals[reason]
                              << '=' << replay.diagnostics().domains[domain].reasons[reason]);
            }
        }
        REQUIRE(replayed);
        REQUIRE(replay.active());
        CHECK(replay.tick() == 101);
        CHECK(replay.diagnostics().domains[domain].updates == 1);
        CHECK(replay.diagnostics().domains[domain].passes == 1);
        CHECK(replay.diagnostics().domains[domain].advancedTicks == 1);
        CHECK(replay.diagnostics().domains[domain].decodeSuccesses == (includeStage ? 1 : 0));
        CHECK(replay.diagnostics().domains[otherDomain] == OnlineCombatReplay::Counters{});
        const auto result = checkpoint(guest);
        REQUIRE(result);
        CHECK(result->combat.players[0].actor.position == seed->combat.players[0].actor.position);
        CHECK(result->combat.players[1].actor.position != seed->combat.players[1].actor.position);
        // Repeated publication retains the exact already-executed native state.
        REQUIRE(replay.update(&*received, std::span{&command, 1}));
        CHECK(matches(guest, *result));
        CHECK(replay.diagnostics().domains[domain].advancedTicks == 1);
        CHECK(replay.diagnostics().domains[domain].decodeAttempts == (includeStage ? 1 : 0));
        CHECK(replay.staged() == includeStage);
        auto display = std::make_unique<CombatSnapshot>(*received);
        display->opponents.reset();
        display->opponentFrame.reset();
        display->combatState.reset();
        display->combatFrame.reset();
        EnemyPresentation presentation;
        presentation.epoch = context.epoch;
        presentation.tick = display->motion.tick;
        presentation.shownGeometry = display->geometry;
        REQUIRE(replay.present(*display, presentation, 1));
        CHECK(static_cast<bool>(presentation.particles) == includeStage);
        if (includeStage) {
            CHECK(presentation.particleTick == 101);
            CHECK(presentation.particleBlend == 1);
        }
        CHECK(replay.diagnostics().domains[domain].presents == 1);
        CHECK(display->motion.players[3]->position == result->combat.players[1].actor.position);
        CHECK(display->motion.players[1]->position == received->motion.players[1]->position);
        CHECK(display->motion.players[1]->yaw == received->motion.players[1]->yaw);
        CHECK(display->valid());
        CHECK(display->players[3]->rollback == result->combat.players[1].figure->animation);
        CHECK_FALSE(display->players[3]->movement.enabled);
        CHECK_FALSE(display->players[3]->rollbackInput);
        // A newer local animation must remain valid even when drawing against
        // an older checkpoint carrying different transition permissions.
        auto directDisplay = std::make_unique<CombatSnapshot>(*received);
        presentation = {};
        presentation.epoch = context.epoch;
        presentation.tick = directDisplay->motion.tick;
        presentation.shownGeometry = directDisplay->geometry;
        REQUIRE(replay.present(*directDisplay, presentation, 0.5f));
        if (includeStage) {
            REQUIRE(presentation.particles);
            CHECK(presentation.particles->matches(context.epoch, 101));
            CHECK(presentation.particleBlend == 0.5f);
        }
        CHECK(directDisplay->valid());
        CHECK_FALSE(directDisplay->combatState);
        CHECK_FALSE(directDisplay->opponents);
        for (const int boundary : {0, 1, 2, 3}) {
            auto refused = std::make_unique<CombatSnapshot>(*received);
            if (boundary == 0) {
                ++refused->motion.cameraContinuity;
            } else if (boundary == 1) {
                ++refused->motion.players[3]->continuity;
            } else if (boundary == 2) {
                ++refused->motion.players[1]->continuity;
            } else {
                refused->players[3]->movement = {};
                refused->players[3]->portalPhase = 0.5f;
            }
            const auto unchanged = CombatPacket::encode(*refused);
            REQUIRE(unchanged);
            const auto particles = presentation.particles;
            CHECK_FALSE(replay.present(*refused, presentation, 1));
            CHECK(CombatPacket::encode(*refused) == unchanged);
            CHECK(presentation.particles == particles);
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
        CHECK(matches(guest, *seed));
        CHECK(replay.diagnostics().domains[domain].refusals == 1);
        CHECK(replay.diagnostics().domains[domain].unsupportedTicks == 1);
        CHECK(replay.diagnostics()
                  .domains[domain]
                  .tickRefusals[static_cast<usize>(OrdinaryCombatTick::Refusal::Environment)] == 1);
        CHECK(replay.diagnostics().domains[domain].reasons[static_cast<usize>(
                  OnlineCombatReplay::Refusal::NativeUnsupported)] == 1);
        replay.clear();
        CHECK(replay.diagnostics() == OnlineCombatReplay::Diagnostics{});
        command.tick = 101;
        command.pressedButtons = 0;
        for (const int hold : {0, 1, 2, 3}) {
            auto held = std::make_unique<CombatSnapshot>(*received);
            if (hold == 0) {
                held->opponentFrame->held = true;
            } else if (hold == 1) {
                held->opponentFrame->timeStopped = true;
            } else if (hold == 2) {
                held->combatFrame->damage.cutscene = true;
            } else {
                held->players[3]->movement = {};
                held->players[3]->portalPhase = 0.5f;
            }
            REQUIRE(held->valid());
            REQUIRE(replay.begin(context, 2, guest));
            CHECK_FALSE(replay.update(held.get(), std::span{&command, 1}));
            CHECK_FALSE(replay.active());
            CHECK(matches(guest, *seed));
            auto reason = OnlineCombatReplay::Refusal::Held;
            if (hold == 2) {
                reason = OnlineCombatReplay::Refusal::Cutscene;
            } else if (hold == 3) {
                reason = OnlineCombatReplay::Refusal::SeatMismatch;
            }
            CHECK(replay.diagnostics().domains[domain].refusals == 1);
            CHECK(replay.diagnostics().domains[domain].reasons[static_cast<usize>(reason)] == 1);
            CHECK(replay.diagnostics().domains[domain].advancedTicks == 0);
            replay.clear();
        }
        if (includeStage) {
            auto foreign = std::make_unique<CombatSnapshot>(*received);
            REQUIRE(foreign->combatState->stage);
            // The enclosing packet only authenticates the bounded envelope.
            // A structurally valid but foreign load-time catalog must fail at
            // the guest's native Stage boundary, never become a legacy seed.
            (*foreign->combatState->stage)[16] ^= 1;
            REQUIRE(foreign->valid());
            REQUIRE(replay.begin(context, 2, guest));
            CHECK_FALSE(replay.update(foreign.get(), std::span{&command, 1}));
            CHECK_FALSE(replay.active());
            CHECK(matches(guest, *seed));
            CHECK(replay.diagnostics().domains[domain].decodeAttempts == 1);
            CHECK(replay.diagnostics().domains[domain].decodeFailures == 1);
            CHECK(replay.diagnostics().domains[domain].decodeSuccesses == 0);
            CHECK(replay.diagnostics()
                      .domains[domain]
                      .reasons[static_cast<usize>(OnlineCombatReplay::Refusal::StageDecode)] == 1);
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
        CHECK(matches(guest, host));
    }
    const auto predicted = checkpoint(guest);
    REQUIRE(predicted);
    CHECK(predicted->combat.players[0].actor.position != seed->combat.players[0].actor.position);
    CHECK(predicted->combat.players[1].actor.position != seed->combat.players[1].actor.position);
    // Refusing an unsupported potion undoes every accepted tick in this replay pass.
    input[1].usePotion = true;
    CHECK(guest.advance(111, input, frame).status == OrdinaryCombatTick::Status::Unsupported);
    CHECK_FALSE(guest.active());
    CHECK(matches(guest, *seed));
    CHECK(matches(host, *predicted));
    REQUIRE(guest.begin(context.epoch, 110, *predicted));
    CHECK_FALSE(guest.begin(context.epoch, 109, *seed));
    CHECK(matches(guest, *predicted));
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

    if (includeStage) {
        // The native owner is selected from the incoming domain, not whichever
        // checkpoint happened to run previously. Neither path reloads resources.
        auto legacy = std::make_unique<NativeState>(*predicted);
        legacy->stage.reset();
        REQUIRE(guest.begin(resumed.epoch, 2, *legacy));
        CHECK(matches(guest, *legacy));
        REQUIRE(guest.begin(resumed.epoch, 3, *predicted));
        CHECK(matches(guest, *predicted));
        auto malformed = std::make_unique<NativeState>(*predicted);
        malformed->stage->world.core.catalog[0] ^= 1;
        CHECK_FALSE(guest.begin(resumed.epoch, 4, *malformed));
        CHECK_FALSE(guest.active());
        CHECK(matches(guest, *predicted));
        CHECK(device.texturesCreated == textures);
        CHECK(device.textureUpdates == updates);

        // Decoded geometry is local-catalog state, not a packet-independent
        // cache. Clear/open of the SAME wrapper must expire its binding token.
        guest.clear();
        REQUIRE(guest.open(device, root, level, context, profiles, 1));
        OnlineCombatReplay replay;
        REQUIRE(replay.begin(context, 2, guest));
        InputCommand command;
        command.epoch = context.epoch;
        command.tick = 101;
        command.grant = context.grants[3];
        command.seat = 3;
        const auto domain = static_cast<usize>(OnlineCombatReplay::Domain::Stage);
        REQUIRE(replay.update(received.get(), std::span{&command, 1}));
        REQUIRE(replay.staged());
        const auto firstPicture = checkpoint(guest);
        REQUIRE(firstPicture);
        REQUIRE(replay.update(nullptr, {}));
        CHECK(matches(guest, *firstPicture));
        CHECK(replay.diagnostics().domains[domain].decodeAttempts == 1);
        auto display = std::make_unique<CombatSnapshot>(*received);
        EnemyPresentation presentation;
        presentation.epoch = context.epoch;
        presentation.tick = received->motion.tick;
        REQUIRE(replay.present(*display, presentation, 1));
        REQUIRE(presentation.particles);
        const auto retainedParticles = presentation.particles;
        REQUIRE(retainedParticles->matches(context.epoch, 101));
        const auto oldLifetime = guest.resourceLifetime();
        guest.clear();
        CHECK(oldLifetime.expired());
        CHECK_FALSE(retainedParticles->matches(context.epoch, 101));
        CHECK_FALSE(replay.active());
        CHECK_FALSE(replay.update(nullptr, {}));
        CHECK(replay.diagnostics().domains[domain].decodeAttempts == 1);
        REQUIRE(guest.open(device, root, level, context, profiles, 1));
        REQUIRE(replay.update(nullptr, {}));
        CHECK(replay.diagnostics().domains[domain].decodeAttempts == 2);
        CHECK(matches(guest, *firstPicture));
        auto corrected = std::make_unique<CombatSnapshot>(*received);
        ++corrected->motion.tick;
        ++command.tick;
        REQUIRE(corrected->valid());
        REQUIRE(replay.update(corrected.get(), std::span{&command, 1}));
        CHECK(replay.diagnostics().domains[domain].decodeAttempts == 3);
        REQUIRE(replay.update(nullptr, {}));
        CHECK(replay.diagnostics().domains[domain].decodeAttempts == 3);
        ++corrected->motion.tick;
        ++command.tick;
        (*corrected->combatState->stage)[16] ^= 1;
        REQUIRE(corrected->valid());
        CHECK_FALSE(replay.update(corrected.get(), std::span{&command, 1}));
        CHECK_FALSE(replay.active());
        CHECK(replay.diagnostics().domains[domain].decodeFailures == 1);
        CHECK(replay.diagnostics().domains[domain].decodeAttempts == 4);
        replay.clear();
        REQUIRE(replay.begin(context, 2, guest));
        command.tick = 101;
        REQUIRE(replay.update(received.get(), std::span{&command, 1}));
        CHECK(replay.diagnostics().domains[domain].decodeAttempts == 1);
    }
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
    const auto authority = checkpoint(first);
    REQUIRE(authority);
    REQUIRE(authority->combat.players.size() == 2);
    CHECK(authority->combat.players[0].player == 1);
    CHECK(authority->combat.players[1].player == 3);
    REQUIRE(second.begin(context.epoch, 100, *authority));
    CHECK(matches(second, *authority));
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
        CHECK(matches(second, *authority));
        CHECK(matches(first, *authority));
    }
    CHECK(device.texturesCreated == created);
    CHECK(device.textureUpdates == updated);
    REQUIRE(second.begin(context.epoch, 100, *authority));
    CHECK_FALSE(second.begin(context.epoch - 1, 101, *authority));
    CHECK_FALSE(second.active());
    CHECK(matches(second, *authority));
    auto malformed = std::make_unique<NativeState>(*authority);
    malformed->combat.players[1].player = 1;
    CHECK_FALSE(second.begin(context.epoch, 101, *malformed));
    CHECK(matches(second, *authority));
    const auto* prior = second.world();
    auto incomplete = profiles;
    incomplete[3].reset();
    CHECK_FALSE(second.open(device, root, *level, context, incomplete, 1));
    CHECK(second.world() == prior);
    CHECK(matches(second, *authority));
    second.clear();
    CHECK_FALSE(second.loaded());
    CHECK_FALSE(second.active());
    CHECK_FALSE(checkpoint(second));
}

TEST_CASE("private combat retains and restores complete native stage state across owners",
          "[rollback][combat-prediction][combat-stage-prediction][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto name = GENERATE("L1", "G1");
    CAPTURE(name);
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(name);
    REQUIRE(level);
    MatchContext context;
    context.epoch = 4;
    context.scene = 1;
    context.tickRate = 60;
    context.owners = {0, 1, 0, 2};
    context.grants = {0, 2, 0, 5};
    PartyBootstrap::Party profiles;
    for (const s32 seat : std::array<s32, 2>{1, 3}) {
        CharacterSave save;
        save.name = "TEST";
        save.character = seat;
        save.color = seat;
        save.autoAttack = false;
        profiles[static_cast<usize>(seat)] = CharacterProfile::capture(save);
    }
    CombatPrediction host, guest;
    REQUIRE(host.open(device, root, *level, context, profiles, 1));
    REQUIRE(guest.open(device, root, *level, context, profiles, 1));
    auto seed = checkpoint(host, true);
    REQUIRE(seed);
    REQUIRE(seed->stage);
    REQUIRE_FALSE(seed->stage->world.core.placements.empty());
    REQUIRE(seed->stage->world.core.placements.size() == host.world()->layout().objects().size());
    CHECK(seed->stage->world.pickups.items.size() == host.world()->placedItems().size());
    const auto stageCatalog = StageStatePacket::Catalog::build(*seed->stage);
    REQUIRE(stageCatalog);
    const auto encoded = StageStatePacket::encode(*seed->stage, *stageCatalog);
    REQUIRE(encoded);
    const auto textures = device.texturesCreated;
    const auto updates = device.textureUpdates;
    auto received = guest.decodeStage(*encoded);
    REQUIRE(received);
    CHECK(received == seed->stage);
    seed->stage = std::move(received);
    REQUIRE(guest.begin(context.epoch, 100, *seed));
    CHECK(matches(guest, *seed));
    guest.resetReplay();
    CHECK(matches(guest, *seed));
    auto changed = std::make_unique<NativeState>(*seed);
    changed->stage->world.core.catalog[0] ^= 1;
    REQUIRE_FALSE(guest.begin(context.epoch, 101, *changed));
    CHECK(matches(guest, *seed));
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == updates);
}

} // namespace
