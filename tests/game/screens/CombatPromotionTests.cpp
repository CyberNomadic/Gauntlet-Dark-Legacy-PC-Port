#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/CombatPrediction.h"
#include "game/screens/SessionInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using State = OrdinaryCombatCheckpoint::State;

std::unique_ptr<State> capture(const CombatPrediction& owner, bool stage) {
    auto value = owner.checkpoint(stage);
    return value ? std::make_unique<State>(std::move(*value)) : nullptr;
}
struct PromotionFixture {
    test::FakeRenderDevice device;
    CombatPrediction authority, predicted;
    MatchContext context;
    OrdinaryCombatTick::Frame frame;
    SessionInputs::Frame inputs{};
    bool stage;
    u16 drawEvery;
    explicit PromotionFixture(bool includeStage, u16 renderRate)
        : stage(includeStage), drawEvery(static_cast<u16>(60 / renderRate)) {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        const auto directory = test::sampleLevel("combat-promotion-floor");
        writeTextFile(directory / "world.json", R"({"objects":[
          {"name":"FLOOR","position":[0,0,0],"flags":4}]})");
        writeTextFile(directory / "collision.json", R"({"objects":[
          {"object":0,"normals":[0,1,0,0,1,0],"vertices":[
          -200,0,-200,200,0,-200,200,0,200,-200,0,-200,200,0,200,-200,0,200]}]})");
        const LevelRef level{
            .name = "TEST", .directory = directory.string(), .items = "ITEMS/levelG"};
        context.epoch = 3;
        context.scene = 1;
        context.tickRate = 60;
        context.owners = {0, 1, 0, 0};
        context.grants = {0, 2, 0, 0};
        CharacterSave save;
        save.name = "TEST";
        save.character = 0;
        save.color = 1;
        save.autoAttack = false;
        PartyBootstrap::Party profiles;
        profiles[1] = CharacterProfile::capture(save);
        REQUIRE(authority.open(device, root, level, context, profiles, 1));
        REQUIRE(predicted.open(device, root, level, context, profiles, 1));
        auto seed = capture(authority, stage);
        REQUIRE(seed);
        seed->combat.players[0].actor.position = {0, 0, 0};
        REQUIRE(authority.begin(context.epoch, 100, *seed));
        REQUIRE(predicted.begin(context.epoch, 100, *seed));
        frame.ticks = 1;
        frame.seconds = static_cast<f32>(frame.ticks) / 60;
        frame.camera.position = {0, 35, -50};
        frame.camera.pitch = 0.6f;
        frame.projection.horizontalFov = 1.6f;
        frame.projection.aspect = 2;
        inputs[1].move.direction = {1, 0};
        inputs[1].move.magnitude = 0.5f;
    }
    void step(CombatPrediction& owner, u64 tick, OrdinaryCombatTick::Frame& context) {
        const auto result = owner.advance(tick, inputs, context);
        CAPTURE(tick, static_cast<s32>(result.status), static_cast<s32>(result.refusal));
        REQUIRE(result.status == OrdinaryCombatTick::Status::Advanced);
        context.damage.playSeconds += context.seconds;
        if (&owner == &authority && tick % drawEvery == 0) {
            owner.world()->draw(device, Mat4{1}, context.camera, 0.25f);
        }
    }
};
} // namespace

TEST_CASE("matching native authority promotes without replay and replaces the complete undo",
          "[rollback][combat-promotion][assets]") {
    const bool stage = GENERATE(false, true);
    const u16 renderRate = GENERATE(30, 60);
    CAPTURE(stage, renderRate);
    auto fixture = std::make_unique<PromotionFixture>(stage, renderRate);
    auto& f = *fixture;
    auto nativeFrame = f.frame;
    auto replayFrame = f.frame;
    for (u64 tick = 101; tick <= 102; ++tick) {
        f.step(f.authority, tick, nativeFrame);
        f.step(f.predicted, tick, replayFrame);
    }
    auto baseline = capture(f.authority, stage);
    REQUIRE(baseline);
    // The authoritative renderer may rebuild its draw-only lights at a
    // different cadence. Keep its actual incoming values for subsequent undo.
    if (stage) {
        baseline->stage->world.core.light.points = {{{1, 2, 3}, {1, 0, 0}, 5, 2}};
        baseline->stage->world.lighting.points = baseline->stage->world.core.light.points;
        baseline->stage->world.litNow.points = baseline->stage->world.core.light.points;
        REQUIRE(baseline->valid());
    }
    for (u64 tick = 103; tick <= 108; ++tick) {
        f.step(f.predicted, tick, replayFrame);
    }
    const auto current = capture(f.predicted, stage);
    REQUIRE(current);
    const auto created = f.device.texturesCreated;
    const auto updates = f.device.textureUpdates;
    auto token = f.predicted.preparePromotion(f.context.epoch, 102, *baseline, nativeFrame);
    REQUIRE(token);
    CHECK(token->baseline() == 102);
    CHECK(token->currentTick() == 108);
    const auto expectedBaseline = std::make_unique<State>(*baseline);
    baseline->combat.players[0].actor.position.x += 100;
    baseline.reset(); // Prepared undo owns values, not the caller's decoded seed.
    REQUIRE(f.predicted.commitPromotion(std::move(*token)));
    CHECK_FALSE(f.predicted.commitPromotion(std::move(*token)));
    CHECK(f.predicted.tick() == 108);
    CHECK(*capture(f.predicted, stage) == *current);
    CHECK(f.device.texturesCreated == created);
    CHECK(f.device.textureUpdates == updates);
    const auto measured = f.predicted.agreementTimings();
    CHECK(measured.recorded == 8);
    CHECK(measured.checked == 1);
    CHECK(measured.prepared == 1);
    CHECK(measured.promoted == 1);
    // Invalid continuation withdraws the whole accepted suffix to the NEW
    // authority, not the original tick100. No live restore happened at promotion.
    CHECK(f.predicted.advance(110, f.inputs, replayFrame).status !=
          OrdinaryCombatTick::Status::Advanced);
    CHECK_FALSE(f.predicted.active());
    CHECK(f.predicted.tick() == 102);
    CHECK(*capture(f.predicted, stage) == *expectedBaseline);
    CHECK_FALSE(f.predicted.begin(f.context.epoch, 101, *expectedBaseline));
}

TEST_CASE("promotion mismatch and stale tokens preserve the existing private interval",
          "[rollback][combat-promotion][assets]") {
    auto fixture = std::make_unique<PromotionFixture>(true, 60);
    auto& f = *fixture;
    const auto original = capture(f.predicted, true);
    REQUIRE(original);
    auto nativeFrame = f.frame;
    auto replayFrame = f.frame;
    f.step(f.authority, 101, nativeFrame);
    f.step(f.predicted, 101, replayFrame);
    const auto baseline = capture(f.authority, true);
    REQUIRE(baseline);
    f.step(f.predicted, 102, replayFrame);
    const auto current = capture(f.predicted, true);
    REQUIRE(current);
    auto changed = std::make_unique<State>(*baseline);
    changed->combat.players[0].actor.position.x += 0.125f;
    CHECK_FALSE(f.predicted.preparePromotion(f.context.epoch, 101, *changed, nativeFrame));
    changed = std::make_unique<State>(*baseline);
    changed->opponents.enemies.frame += 1;
    CHECK_FALSE(f.predicted.preparePromotion(f.context.epoch, 101, *changed, nativeFrame));
    changed = std::make_unique<State>(*baseline);
    changed->adapter.hitStreak[1] += 1;
    CHECK_FALSE(f.predicted.preparePromotion(f.context.epoch, 101, *changed, nativeFrame));
    changed = std::make_unique<State>(*baseline);
    changed->stage->world.frameRemainder += 0.125f;
    CHECK_FALSE(f.predicted.preparePromotion(f.context.epoch, 101, *changed, nativeFrame));
    auto wrongFrame = nativeFrame;
    wrongFrame.autoMelee = !wrongFrame.autoMelee;
    CHECK_FALSE(f.predicted.preparePromotion(f.context.epoch, 101, *baseline, wrongFrame));
    CHECK_FALSE(f.predicted.preparePromotion(f.context.epoch + 1, 101, *baseline, nativeFrame));
    CHECK_FALSE(f.predicted.preparePromotion(f.context.epoch, 99, *baseline, nativeFrame));
    CHECK(*capture(f.predicted, true) == *current);

    // Even a normalized-away malformed cache must fail full preparation.
    changed = std::make_unique<State>(*baseline);
    changed->stage->world.core.light.points = {{{0, 0, 0}, {1, 1, 1}, -1, 2}};
    CHECK_FALSE(changed->valid());
    CHECK_FALSE(f.predicted.preparePromotion(f.context.epoch, 101, *changed, nativeFrame));
    auto token = f.predicted.preparePromotion(f.context.epoch, 101, *baseline, nativeFrame);
    REQUIRE(token);
    CHECK_FALSE(f.authority.commitPromotion(std::move(*token)));
    f.step(f.predicted, 103, replayFrame);
    CHECK_FALSE(f.predicted.commitPromotion(std::move(*token)));
    f.predicted.resetReplay();
    CHECK(f.predicted.tick() == 100);
    CHECK(*capture(f.predicted, true) == *original);
}

TEST_CASE("promotion keeps absolute history ticks across a wrapped bounded interval",
          "[rollback][combat-promotion][assets]") {
    auto fixture = std::make_unique<PromotionFixture>(true, 30);
    auto& f = *fixture;
    auto nativeFrame = f.frame;
    auto replayFrame = f.frame;
    for (u64 tick = 101; tick <= 160; ++tick) {
        f.step(f.authority, tick, nativeFrame);
        f.step(f.predicted, tick, replayFrame);
    }
    const auto first = capture(f.authority, true);
    REQUIRE(first);
    for (u64 tick = 161; tick <= 220; ++tick) {
        f.step(f.predicted, tick, replayFrame);
    }
    auto promotion = f.predicted.preparePromotion(f.context.epoch, 160, *first, nativeFrame);
    REQUIRE(promotion);
    REQUIRE(f.predicted.commitPromotion(std::move(*promotion)));

    for (u64 tick = 161; tick <= 210; ++tick) {
        f.step(f.authority, tick, nativeFrame);
    }
    const auto second = capture(f.authority, true);
    REQUIRE(second);
    // The history array has wrapped, but tick210 was recorded before the first
    // promotion and remains a witness for the unchanged native suffix.
    for (u64 tick = 221; tick <= 230; ++tick) {
        f.step(f.predicted, tick, replayFrame);
    }
    promotion = f.predicted.preparePromotion(f.context.epoch, 210, *second, nativeFrame);
    REQUIRE(promotion);
    REQUIRE(f.predicted.commitPromotion(std::move(*promotion)));
    CHECK(f.predicted.tick() == 230);
    CHECK(f.predicted.agreementTimings().promoted == 2);
    CHECK_FALSE(f.predicted.preparePromotion(f.context.epoch, 160, *first, nativeFrame));
    f.predicted.resetReplay();
    CHECK(f.predicted.tick() == 210);
    CHECK(*capture(f.predicted, true) == *second);

    auto resumed = f.context;
    ++resumed.epoch;
    resumed.transition = MatchTransition::Resume;
    REQUIRE(f.predicted.resume(resumed));
    CHECK_FALSE(f.predicted.preparePromotion(f.context.epoch, 210, *second, nativeFrame));
    REQUIRE(f.predicted.begin(resumed.epoch, 10, *second));
    f.step(f.predicted, 11, nativeFrame);
    CHECK_FALSE(f.predicted.preparePromotion(resumed.epoch, 210, *second, nativeFrame));
    f.predicted.resetReplay();
    CHECK(f.predicted.tick() == 10);
    CHECK(*capture(f.predicted, true) == *second);
}

TEST_CASE("promotion tokens cannot cross owner resets or a new authority interval",
          "[rollback][combat-promotion][assets]") {
    const int invalidate = GENERATE(0, 1, 2, 3);
    CAPTURE(invalidate);
    auto fixture = std::make_unique<PromotionFixture>(true, 60);
    auto& f = *fixture;
    auto nativeFrame = f.frame;
    auto replayFrame = f.frame;
    f.step(f.authority, 101, nativeFrame);
    f.step(f.predicted, 101, replayFrame);
    const auto baseline = capture(f.authority, true);
    REQUIRE(baseline);
    f.step(f.predicted, 102, replayFrame);
    auto token = f.predicted.preparePromotion(f.context.epoch, 101, *baseline, nativeFrame);
    REQUIRE(token);
    if (invalidate == 0) {
        f.predicted.resetReplay();
    } else if (invalidate == 1) {
        auto context = f.context;
        ++context.epoch;
        context.transition = MatchTransition::Resume;
        REQUIRE(f.predicted.resume(context));
    } else if (invalidate == 2) {
        REQUIRE(f.predicted.begin(f.context.epoch, 105, *baseline));
    } else {
        f.predicted.clear();
        CHECK_FALSE(f.predicted.commitPromotion(std::move(*token)));
        return;
    }
    const auto before = capture(f.predicted, true);
    REQUIRE(before);
    const auto tick = f.predicted.tick();
    CHECK_FALSE(f.predicted.commitPromotion(std::move(*token)));
    CHECK(f.predicted.tick() == tick);
    CHECK(*capture(f.predicted, true) == *before);
}
