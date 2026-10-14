#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/InputTimeline.h"
#include "game/netplay/StageStatePacket.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/OnlineCombatReplay.h"
#include "game/screens/SessionInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using State = OrdinaryCombatCheckpoint::State;

std::unique_ptr<State> capture(const CombatPrediction& owner, bool stage) {
    auto state = owner.checkpoint(stage);
    return state ? std::make_unique<State>(std::move(*state)) : nullptr;
}

std::shared_ptr<const StageParticlePresentation>
sampleParticles(const std::shared_ptr<const StageReplayPresentation::Publication>& publication,
                const CombatSnapshot& authority) {
    auto shown = std::make_unique<CombatSnapshot>(authority);
    EnemyPresentation presentation;
    presentation.epoch = shown->motion.epoch;
    presentation.tick = shown->motion.tick;
    presentation.shownGeometry = shown->geometry;
    StageReplayPresentation presenter;
    const StageReplayPresentation::Time start{};
    REQUIRE(presenter.adopt(publication, publication->authority(), start));
    REQUIRE(presenter.present(*shown, presentation, publication->authority(), start));
    REQUIRE(presentation.particles);
    return presentation.particles;
}

struct Fixture {
    test::FakeRenderDevice device;
    CombatPrediction host, guest;
    OnlineCombatReplay replay;
    MatchContext context;
    OrdinaryCombatTick::Frame frame;
    bool stage;

    explicit Fixture(bool includeStage) : stage(includeStage) {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        const auto directory = test::sampleLevel("online-combat-promotion");
        writeTextFile(directory / "world.json", R"({"objects":[
          {"name":"FLOOR","position":[0,0,0],"flags":4}]})");
        writeTextFile(directory / "collision.json", R"({"objects":[
          {"object":0,"normals":[0,1,0,0,1,0],"vertices":[
          -200,0,-200,200,0,-200,200,0,200,-200,0,-200,200,0,200,-200,0,200]}]})");
        const LevelRef level{
            .name = "TEST", .directory = directory.string(), .items = "ITEMS/levelG"};
        context.epoch = 3;
        context.scene = 1;
        context.owners = {0, 1, 0, 2};
        context.grants = {0, 2, 0, 5};
        PartyBootstrap::Party profiles;
        for (const s32 seat : {1, 3}) {
            CharacterSave save;
            save.name = "TEST";
            save.character = 0;
            save.color = seat;
            save.autoAttack = false;
            profiles[static_cast<usize>(seat)] = CharacterProfile::capture(save);
        }
        REQUIRE(host.open(device, root, level, context, profiles, 1));
        REQUIRE(guest.open(device, root, level, context, profiles, 1));
        auto seed = capture(host, stage);
        REQUIRE(seed);
        seed->combat.players[0].actor.position = {-12, 0, 0};
        seed->combat.players[1].actor.position = {12, 0, 0};
        REQUIRE(host.begin(context.epoch, 100, *seed));
        REQUIRE(guest.begin(context.epoch, 100, *seed));
        frame.ticks = 1;
        frame.seconds = 1.0f / 60;
        frame.camera.position = {0, 35, -50};
        frame.camera.pitch = 0.6f;
        frame.projection.horizontalFov = 1.6f;
        frame.projection.aspect = 2;
        REQUIRE(replay.begin(context, 2, guest));
    }

    InputCommand local(u64 tick) const {
        InputCommand command;
        command.epoch = context.epoch;
        command.tick = tick;
        command.grant = context.grants[3];
        command.seat = 3;
        command.direction = {0, 1};
        command.magnitude = 0.5f;
        return command;
    }

    void stepHost(u64 tick, const InputCommand& localInput,
                  const std::optional<InputCommand>& remote = std::nullopt) {
        SessionInputs::Frame inputs{};
        inputs[3] = SessionInputs::playInput(localInput);
        if (remote) {
            inputs[1] = SessionInputs::playInput(*remote);
        }
        const auto result = host.advance(tick, inputs, frame);
        CAPTURE(tick, static_cast<s32>(result.status), static_cast<s32>(result.refusal));
        REQUIRE(result.status == OrdinaryCombatTick::Status::Advanced);
        frame.damage.playSeconds += frame.seconds;
    }

    std::unique_ptr<CombatSnapshot> authority(u64 tick) const {
        const auto seed = capture(host, stage);
        REQUIRE(seed);
        MotionSnapshot motion;
        motion.epoch = context.epoch;
        motion.tick = tick;
        motion.cameraContinuity = 1;
        motion.camera = frame.camera;
        motion.horizontalFov = frame.projection.horizontalFov;
        motion.aspect = frame.projection.aspect;
        for (const auto& player : host.players()) {
            const auto seat = static_cast<usize>(player.actor.player());
            motion.players[seat] =
                SeatMotion{context.grants[seat], 1, player.actor.position(), player.actor.yaw()};
        }
        auto value =
            CombatCapture::capture(motion, host.players(), *host.enemies(), nullptr, host.world());
        REQUIRE(value);
        auto result = std::make_unique<CombatSnapshot>(std::move(*value));
        result->opponents = seed->opponents;
        result->opponentFrame.emplace();
        result->opponentFrame->ticks = frame.ticks;
        result->opponentFrame->seconds = frame.seconds;
        result->opponentFrame->initialPlayers = 2;
        const auto views = LevelOpponents::enemyViews(host.players());
        for (usize i = 0; i < host.players().size(); ++i) {
            result->opponentFrame->players.push_back(
                {static_cast<u8>(host.players()[i].actor.player()), views[i]});
        }
        result->combatState = CombatStatePacket::State{seed->combat, seed->adapter};
        if (stage) {
            result->geometry = host.world()->scene().geometry();
            const auto catalog = StageStatePacket::Catalog::build(*seed->stage);
            REQUIRE(catalog);
            result->combatState->stage = StageStatePacket::encode(*seed->stage, *catalog);
            REQUIRE(result->combatState->stage);
        }
        result->combatFrame = frame;
        // This is native post-step input history, not an interpolation clock.
        if (tick > 100) {
            result->players[3]->rollbackInput = local(tick);
        }
        REQUIRE(result->valid());
        return result;
    }
};
} // namespace

TEST_CASE("online replay promotes matching authority and only advances its new suffix",
          "[rollback][online-combat-promotion][assets]") {
    const bool stage = GENERATE(false, true);
    const bool append = GENERATE(false, true);
    auto fixture = std::make_unique<Fixture>(stage);
    auto& f = *fixture;
    auto initial = f.authority(100);
    for (u64 tick = 101; tick <= 106; ++tick) {
        const auto input = f.local(tick);
        REQUIRE(f.replay.update(tick == 101 ? initial.get() : nullptr, std::span{&input, 1}));
        if (tick <= 102) {
            f.stepHost(tick, input);
        }
    }
    auto corrected = f.authority(102);
    const auto newBaseline = capture(f.host, stage);
    REQUIRE(newBaseline);
    const auto domain = static_cast<usize>(stage ? OnlineCombatReplay::Domain::Stage
                                                 : OnlineCombatReplay::Domain::Legacy);
    REQUIRE(f.replay.diagnostics().domains[domain].advancedTicks == 6);
    auto next = f.local(107);
    REQUIRE(f.replay.update(corrected.get(),
                            append ? std::span{&next, 1} : std::span<const InputCommand>{}));
    CHECK(f.replay.diagnostics().domains[domain].promotedPasses == 1);
    CHECK(f.replay.diagnostics().domains[domain].restorePasses == 1);
    CHECK(f.replay.diagnostics().domains[domain].advancedTicks == (append ? 7 : 6));
    CHECK(f.replay.diagnostics().domains[domain].reusedPasses == (append ? 0 : 1));
    const u64 end = append ? 107 : 106;
    if (stage) {
        const auto publication = f.replay.publication();
        REQUIRE(publication);
        CHECK(publication->tick() == end);
    }
    for (u64 tick = 103; tick <= end; ++tick) {
        f.stepHost(tick, f.local(tick));
    }
    CHECK(*capture(f.guest, stage) == *capture(f.host, stage));

    // Refusal must return to the newly promoted authority, not the old tick100.
    next = f.local(end + 1);
    next.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    CHECK_FALSE(f.replay.update(nullptr, std::span{&next, 1}));
    CHECK_FALSE(f.replay.active());
    CHECK(*capture(f.guest, stage) == *newBaseline);
}

TEST_CASE("matching authority cannot retain a suffix with changed effective commands",
          "[rollback][online-combat-promotion][assets]") {
    const bool remoteChanged = GENERATE(false, true);
    auto fixture = std::make_unique<Fixture>(true);
    auto& f = *fixture;
    auto initial = f.authority(100);
    auto first = f.local(101);
    REQUIRE(f.replay.update(initial.get(), std::span{&first, 1}));
    auto horizon = f.local(106);
    REQUIRE(f.replay.update(nullptr, std::span{&horizon, 1}));
    f.stepHost(101, first);
    f.stepHost(102, f.local(102));
    auto corrected = f.authority(102);
    std::optional<InputCommand> remote;
    if (remoteChanged) {
        remote = InputCommand{f.context.epoch, 102, f.context.grants[1], 1};
        remote->direction = {1, 0};
        remote->magnitude = 0.25f;
        corrected->players[1]->rollbackInput = remote;
    } else {
        // A real local command replaces what the earlier pass concealed at105.
        auto late = f.local(105);
        late.direction = {1, 0};
        REQUIRE(f.replay.update(corrected.get(), std::span{&late, 1}));
    }
    auto next = f.local(107);
    REQUIRE(f.replay.update(remoteChanged ? corrected.get() : nullptr, std::span{&next, 1}));
    const auto domain = static_cast<usize>(OnlineCombatReplay::Domain::Stage);
    CHECK(f.replay.diagnostics().domains[domain].promotedPasses == 0);
    CHECK(f.replay.diagnostics().domains[domain].restorePasses == 2);
    for (u64 tick = 103; tick <= 107; ++tick) {
        auto input = f.local(tick);
        if (!remoteChanged && tick == 105) {
            input.direction = {1, 0};
        }
        f.stepHost(tick, input, remote);
    }
    CHECK(*capture(f.guest, true) == *capture(f.host, true));
}

TEST_CASE("authority-only promotion retains its endpoint and queues all newer input",
          "[rollback][online-combat-promotion][combat-replay-rebase][assets]") {
    auto fixture = std::make_unique<Fixture>(true);
    auto& f = *fixture;
    auto initial = f.authority(100);
    for (u64 tick = 101; tick <= 106; ++tick) {
        const auto input = f.local(tick);
        REQUIRE(f.replay.update(tick == 101 ? initial.get() : nullptr, std::span{&input, 1}));
        if (tick <= 102) {
            f.stepHost(tick, input);
        }
    }
    auto corrected = f.authority(102);
    const auto baseline = capture(f.host, true);
    const auto before = capture(f.guest, true);
    REQUIRE(baseline);
    REQUIRE(before);
    const auto previousPublication = f.replay.publication();
    REQUIRE(previousPublication);
    const auto particles = sampleParticles(previousPublication, *initial);
    const auto uploads = f.device.texturesCreated;
    const auto textureUpdates = f.device.textureUpdates;
    const auto draws = f.device.draws.size();
    std::vector<InputCommand> future;
    for (u64 tick = 107; tick <= 110; ++tick) {
        auto input = f.local(tick);
        input.direction = {1, 0};
        future.push_back(input);
    }
    REQUIRE(f.replay.promoteAuthority(*corrected, future));
    CHECK(f.replay.tick() == 106);
    CHECK(*capture(f.guest, true) == *before);
    const auto& counters = f.replay.diagnostics().domains[2];
    CHECK(counters.advancedTicks == 6);
    CHECK(counters.attemptedTicks == 6);
    CHECK(counters.promotedPasses == 1);
    CHECK(counters.restorePasses == 1);
    auto promoted = f.replay.publication();
    REQUIRE(promoted);
    CHECK(promoted->authority().tick() == 102);
    CHECK(promoted->tick() == 106);
    CHECK(promoted->revision() > previousPublication->revision());
    CHECK(sampleParticles(promoted, *corrected) == particles);
    CHECK(f.device.texturesCreated == uploads);
    CHECK(f.device.textureUpdates == textureUpdates);
    CHECK(f.device.draws.size() == draws);

    const auto diagnostics = f.replay.diagnostics();
    CHECK_FALSE(f.replay.promoteAuthority(*corrected, future));
    CHECK(f.replay.diagnostics() == diagnostics);
    CHECK(f.replay.publication()->revision() == promoted->revision());
    // No retransmission of the future list is needed. Promotion recorded it,
    // but did not consume or run any of its ticks.
    REQUIRE(f.replay.update(nullptr, {}));
    CHECK(f.replay.tick() == 110);
    CHECK(counters.advancedTicks == 10);
    for (u64 tick = 103; tick <= 110; ++tick) {
        f.stepHost(tick, tick < 107 ? f.local(tick) : future[static_cast<usize>(tick - 107)]);
    }
    CHECK(*capture(f.guest, true) == *capture(f.host, true));
    auto refused = f.local(111);
    refused.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    CHECK_FALSE(f.replay.update(nullptr, std::span{&refused, 1}));
    CHECK(*capture(f.guest, true) == *baseline);
}

TEST_CASE("authority-only promotion refuses changed local or remote effective suffixes",
          "[rollback][online-combat-promotion][combat-replay-rebase][assets]") {
    const bool remoteChanged = GENERATE(false, true);
    auto fixture = std::make_unique<Fixture>(true);
    auto& f = *fixture;
    auto initial = f.authority(100);
    const auto first = f.local(101);
    REQUIRE(f.replay.update(initial.get(), std::span{&first, 1}));
    const auto horizon = f.local(106);
    REQUIRE(f.replay.update(nullptr, std::span{&horizon, 1}));
    f.stepHost(101, first);
    f.stepHost(102, f.local(102));
    auto corrected = f.authority(102);
    std::vector<InputCommand> incoming;
    std::optional<InputCommand> remote;
    if (remoteChanged) {
        remote = InputCommand{f.context.epoch, 102, f.context.grants[1], 1};
        remote->direction = {1, 0};
        remote->magnitude = .25f;
        corrected->players[1]->rollbackInput = remote;
    } else {
        auto late = f.local(105);
        late.direction = {1, 0};
        incoming.push_back(late);
    }
    incoming.push_back(f.local(107));
    const auto attempted = f.replay.diagnostics().domains[2].attemptedTicks;
    CHECK_FALSE(f.replay.promoteAuthority(*corrected, incoming));
    CHECK_FALSE(f.replay.active());
    CHECK_FALSE(f.replay.publication());
    CHECK(f.replay.diagnostics().domains[2].attemptedTicks == attempted);
    CHECK(f.replay.diagnostics().domains[2].promotedPasses == 0);
    // The ordinary latest job still has a complete input journal and may
    // recover by restoring the new authority instead of retaining a wrong C.
    REQUIRE(f.replay.update(corrected.get(), incoming));
    for (u64 tick = 103; tick <= 107; ++tick) {
        auto input = f.local(tick);
        if (!remoteChanged && tick == 105) {
            input.direction = {1, 0};
        }
        f.stepHost(tick, input, remote);
    }
    CHECK(*capture(f.guest, true) == *capture(f.host, true));
}

TEST_CASE("authority at or beyond the native endpoint requires a new interval",
          "[rollback][online-combat-promotion][combat-replay-rebase][assets]") {
    const u64 extra = GENERATE(u64{0}, u64{1});
    auto fixture = std::make_unique<Fixture>(true);
    auto& f = *fixture;
    auto initial = f.authority(100);
    for (u64 tick = 101; tick <= 106 + extra; ++tick) {
        const auto input = f.local(tick);
        if (tick <= 106) {
            REQUIRE(f.replay.update(tick == 101 ? initial.get() : nullptr, std::span{&input, 1}));
        }
        f.stepHost(tick, input);
    }
    auto corrected = f.authority(106 + extra);
    const auto before = capture(f.guest, true);
    REQUIRE(before);
    const auto next = f.local(108);
    CHECK_FALSE(f.replay.promoteAuthority(*corrected, std::span{&next, 1}));
    CHECK(f.replay.tick() == 106);
    CHECK(*capture(f.guest, true) == *before);
    CHECK(f.replay.diagnostics().domains[2].advancedTicks == 6);
    REQUIRE(f.replay.update(corrected.get(), std::span{&next, 1}));
    const auto publication = f.replay.publication();
    REQUIRE(publication);
    CHECK(publication->authority().tick() == 106 + extra);
    CHECK(publication->tick() == 108);
    for (u64 tick = 107 + extra; tick <= 108; ++tick) {
        f.stepHost(tick, f.local(tick));
    }
    CHECK(*capture(f.guest, true) == *capture(f.host, true));
}
