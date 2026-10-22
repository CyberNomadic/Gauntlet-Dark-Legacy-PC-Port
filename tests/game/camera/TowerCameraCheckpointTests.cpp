#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "game/camera/CameraMovementLimit.h"
#include "game/camera/TowerCameraCheckpoint.h"
#include "game/level/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

CameraSubject standing(const Vec3& feet) {
    return {feet, feet + Vec3{0, 2.5f, 0}, 1.25f};
}
struct CameraFixture {
    test::FakeRenderDevice device;
    std::filesystem::path root;
    LevelWorld world;
    const LevelRef level{"TEST", 0, "CAMERA", "Camera checkpoint", "", "", "", 0};
    const CameraView projection;
    std::array<CameraSubject, 2> party{standing({-2, 0, 0}), standing({2, 0, 0})};
    TowerCamera native, replay;
    std::unique_ptr<TowerCameraCheckpoint> nativeOwner, replayOwner;

    CameraFixture() : root(test::sampleLevel("tower-camera-checkpoint")) {
        writeTextFile(root / "world.json", R"({
          "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4}],
          "locators":[
            {"type":"cameraGame","position":[0,0,0],"rotation":[0.5,0,0]},
            {"type":"cameraGame","position":[100,0,0],"rotation":[0.8,1.7,0]},
            {"type":"triggerCamera","position":[50,0,50],"rotation":[0.6,2,0]}]
        })");
        REQUIRE(world.load(device, root, level));
        REQUIRE(world.cameraMarkers().size() == 2);
        native.reset(party, world.cameraMarkers(), world.cameraRange(), projection);
        replay.reset(party, world.cameraMarkers(), world.cameraRange(), projection);
        nativeOwner = std::make_unique<TowerCameraCheckpoint>(native, world);
        replayOwner = std::make_unique<TowerCameraCheckpoint>(replay, world);
    }
    static TowerCameraCheckpoint::State capture(const TowerCameraCheckpoint& owner) {
        auto state = owner.checkpoint();
        REQUIRE(state);
        return std::move(*state);
    }
    void update(TowerCamera& camera, f32 seconds) {
        camera.update(party, world.cameraMarkers(), world.cameraRange(), projection, seconds);
    }
    void beginTurn(f32 seconds) {
        party = {standing({86, 0, -2}), standing({94, 0, 3})};
        for (s32 i = 0; i < 3; ++i) {
            update(native, seconds);
        }
        REQUIRE(native.turning());
        REQUIRE(native.marker() == 1);
    }
};

TEST_CASE("camera checkpoint resumes every native smoothing and turn field at thirty and sixty Hz",
          "[game][camera-checkpoint]") {
    const auto rate = GENERATE(30, 60);
    CAPTURE(rate);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    CameraFixture fixture;
    fixture.beginTurn(seconds);
    const auto baseline = CameraFixture::capture(*fixture.nativeOwner);
    REQUIRE(baseline.turnStep > 0);
    REQUIRE(baseline.turnStep < TowerCamera::kTurnSteps);
    REQUIRE(baseline.markers.size() == 2); // never imports the trigger-cut marker
    REQUIRE(fixture.replayOwner->restore(baseline));
    REQUIRE(CameraFixture::capture(*fixture.replayOwner) == baseline);
    const auto textures = fixture.device.texturesCreated;
    bool finishedTurn = false;
    bool wrappedRing = false;
    for (s32 tick = 0; tick < rate * 3; ++tick) {
        CAPTURE(tick);
        const f32 step = static_cast<f32>(tick) / static_cast<f32>(rate);
        fixture.party = {standing({90 + step * 3, step, -4 - step}),
                         standing({94 - step, 0, 4 + step * 2})};
        fixture.update(fixture.native, seconds);
        fixture.update(fixture.replay, seconds);
        const auto expected = CameraFixture::capture(*fixture.nativeOwner);
        CHECK(CameraFixture::capture(*fixture.replayOwner) == expected);
        wrappedRing = wrappedRing || expected.ringIndex < baseline.ringIndex;
        finishedTurn = finishedTurn || !fixture.native.turning();
    }
    CHECK(wrappedRing);
    CHECK(finishedTurn);
    CHECK(fixture.device.texturesCreated == textures);
}

TEST_CASE("camera rewind preserves subsequent native leash and camera relative directions",
          "[game][camera-checkpoint][camera-limit]") {
    const auto rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    CameraFixture fixture;
    fixture.beginTurn(seconds);
    const auto baseline = CameraFixture::capture(*fixture.nativeOwner);
    auto undo = fixture.nativeOwner->prepareRestore(baseline);
    REQUIRE(undo);
    for (s32 tick = 0; tick < rate; ++tick) {
        fixture.party = {standing({-100, 0, -20}), standing({100, 0, 80})};
        fixture.update(fixture.native, seconds);
    }
    REQUIRE(fixture.nativeOwner->canCommit(*undo)); // native updates are rewindable
    REQUIRE(fixture.nativeOwner->commitRestore(std::move(*undo)));
    REQUIRE(fixture.replayOwner->restore(baseline));
    CHECK(CameraFixture::capture(*fixture.nativeOwner) == baseline);
    bool limited = false;
    for (s32 tick = 0; tick < 120; ++tick) {
        CAPTURE(rate, tick);
        const Vec3 before = fixture.native.attention() + Vec3{60, -2.5f, 0};
        const Vec3 desired = before + Vec3{0.2f, 0, 0.1f};
        const Vec3 expected = CameraMovementLimit::constrain(
            before, desired, fixture.native.attention(), fixture.native.camera(),
            fixture.projection, {0, 2.5f, 0});
        const Vec3 actual = CameraMovementLimit::constrain(
            before, desired, fixture.replay.attention(), fixture.replay.camera(),
            fixture.projection, {0, 2.5f, 0});
        CHECK(actual == expected);
        CHECK(fixture.replay.camera().right() == fixture.native.camera().right());
        CHECK(fixture.replay.camera().forward() == fixture.native.camera().forward());
        limited = limited || expected != desired;
        fixture.party = {standing(expected),
                         standing(fixture.native.attention() + Vec3{-20, 0, 0})};
        fixture.update(fixture.native, seconds);
        fixture.update(fixture.replay, seconds);
        CHECK(CameraFixture::capture(*fixture.replayOwner) ==
              CameraFixture::capture(*fixture.nativeOwner));
    }
    CHECK(limited);
}

TEST_CASE("camera checkpoint handles trigger attention snap without discarding native history",
          "[game][camera-checkpoint]") {
    CameraFixture fixture;
    fixture.beginTurn(1.0f / 60);
    const auto before = CameraFixture::capture(*fixture.nativeOwner);
    REQUIRE(fixture.replayOwner->restore(before));
    fixture.party = {standing({-30, 3, 4}), standing({20, 0, -3})};
    fixture.native.snapAttention(fixture.party, fixture.world.cameraRange());
    fixture.replay.snapAttention(fixture.party, fixture.world.cameraRange());
    const auto snapped = CameraFixture::capture(*fixture.nativeOwner);
    CHECK(snapped.ringDistances == before.ringDistances);
    CHECK(snapped.yawRate == before.yawRate);
    CHECK(snapped.turnStep == before.turnStep);
    CHECK(CameraFixture::capture(*fixture.replayOwner) == snapped);
    fixture.update(fixture.native, 1.0f / 60);
    fixture.update(fixture.replay, 1.0f / 60);
    CHECK(CameraFixture::capture(*fixture.replayOwner) ==
          CameraFixture::capture(*fixture.nativeOwner));
}

TEST_CASE("camera checkpoint rejects malformed and foreign authored bindings without mutation",
          "[game][camera-checkpoint]") {
    CameraFixture fixture;
    fixture.beginTurn(1.0f / 60);
    const auto baseline = CameraFixture::capture(*fixture.nativeOwner);
    using State = TowerCameraCheckpoint::State;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const std::array<std::function<void(State&)>, 20> damage{
        [=](auto& state) { state.camera.position.x = nan; },
        [=](auto& state) { state.camera.pitch = nan; },
        [](auto& state) { state.camera.yaw = 100000; },
        [](auto& state) { state.camera.roll = 1; },
        [=](auto& state) { state.attention.y = nan; },
        [](auto& state) { state.distance = -1; },
        [](auto& state) { state.marker = 2; },
        [](auto& state) { state.aimedMarker = -2; },
        [=](auto& state) { state.yawRate = nan; },
        [](auto& state) { state.yawRate = 100000; },
        [=](auto& state) { state.pitchRate = nan; },
        [](auto& state) { state.turnStep = 51; },
        [](auto& state) { state.ringIndex = TowerCamera::kRing; },
        [=](auto& state) { state.ringPositions[8].z = nan; },
        [](auto& state) { state.ringDistances[8] = -1; },
        [](auto& state) { state.markers.resize(State::kMaxMarkers + 1); },
        [](auto& state) { state.markers[0].rotation.y += 0.5f; },
        [](auto& state) { state.markers[1].delay += 1; },
        [](auto& state) { state.range.radiusMax += 1; },
        [](auto& state) { state.level.name = "OTHER"; }};
    for (usize index = 0; index < damage.size(); ++index) {
        CAPTURE(index);
        auto corrupt = baseline;
        damage[index](corrupt);
        CHECK_FALSE(fixture.nativeOwner->prepareRestore(corrupt));
        CHECK_FALSE(fixture.nativeOwner->restore(corrupt));
        CHECK(CameraFixture::capture(*fixture.nativeOwner) == baseline);
    }
}

TEST_CASE("camera prepared restoration is owner bound single use and invalidates sibling plans",
          "[game][camera-checkpoint]") {
    CameraFixture fixture;
    const auto baseline = CameraFixture::capture(*fixture.nativeOwner);
    auto first = fixture.nativeOwner->prepareRestore(baseline);
    auto sibling = fixture.nativeOwner->prepareRestore(baseline);
    const TowerCameraCheckpoint secondWrapper{fixture.native, fixture.world};
    auto otherWrapper = secondWrapper.prepareRestore(baseline);
    REQUIRE(first);
    REQUIRE(sibling);
    REQUIRE(otherWrapper);
    CHECK_FALSE(fixture.replayOwner->canCommit(*first));
    CHECK_FALSE(fixture.replayOwner->commitRestore(std::move(*first)));
    REQUIRE(fixture.nativeOwner->canCommit(*first));
    REQUIRE(fixture.nativeOwner->commitRestore(std::move(*first)));
    CHECK_FALSE(fixture.nativeOwner->canCommit(*first));
    CHECK_FALSE(fixture.nativeOwner->commitRestore(std::move(*first)));
    CHECK_FALSE(fixture.nativeOwner->canCommit(*sibling));
    CHECK_FALSE(secondWrapper.canCommit(*otherWrapper));
    CHECK(CameraFixture::capture(*fixture.nativeOwner) == baseline);
}

TEST_CASE("camera reset and value assignment cannot revive old prepared owner identities",
          "[game][camera-checkpoint]") {
    CameraFixture fixture;
    fixture.beginTurn(1.0f / 60);
    const auto baseline = CameraFixture::capture(*fixture.nativeOwner);
    auto beforeReset = fixture.nativeOwner->prepareRestore(baseline);
    REQUIRE(beforeReset);
    TowerCamera copied{fixture.native};
    const TowerCameraCheckpoint copyOwner{copied, fixture.world};
    REQUIRE(copyOwner.checkpoint() == std::optional{baseline});
    fixture.native.reset(fixture.party, fixture.world.cameraMarkers(), fixture.world.cameraRange(),
                         fixture.projection);
    CHECK_FALSE(fixture.nativeOwner->canCommit(*beforeReset));
    auto beforeAssignment = fixture.nativeOwner->prepareRestore(baseline);
    REQUIRE(beforeAssignment);
    fixture.native = copied;
    CHECK(CameraFixture::capture(*fixture.nativeOwner) == baseline);
    CHECK_FALSE(fixture.nativeOwner->canCommit(*beforeReset));
    CHECK_FALSE(fixture.nativeOwner->canCommit(*beforeAssignment));
    auto beforeMove = fixture.nativeOwner->prepareRestore(baseline);
    REQUIRE(beforeMove);
    fixture.native = std::move(copied);
    CHECK(CameraFixture::capture(*fixture.nativeOwner) == baseline);
    CHECK_FALSE(fixture.nativeOwner->canCommit(*beforeMove));
}

TEST_CASE("camera restore refuses stale world and wrapper lifetimes even after equal reload",
          "[game][camera-checkpoint]") {
    CameraFixture fixture;
    const auto baseline = CameraFixture::capture(*fixture.nativeOwner);
    auto stale = fixture.nativeOwner->prepareRestore(baseline);
    REQUIRE(stale);
    fixture.nativeOwner.reset();
    fixture.nativeOwner = std::make_unique<TowerCameraCheckpoint>(fixture.native, fixture.world);
    CHECK_FALSE(fixture.nativeOwner->canCommit(*stale));
    auto beforeClear = fixture.nativeOwner->prepareRestore(baseline);
    REQUIRE(beforeClear);
    fixture.world.clear();
    CHECK_FALSE(fixture.nativeOwner->checkpoint());
    CHECK_FALSE(fixture.nativeOwner->canCommit(*beforeClear));
    REQUIRE(fixture.world.load(fixture.device, fixture.root, fixture.level));
    CHECK_FALSE(fixture.nativeOwner->binds(fixture.native, fixture.world));
    CHECK_FALSE(fixture.nativeOwner->canCommit(*beforeClear));
    CHECK_FALSE(fixture.nativeOwner->restore(baseline));
    TowerCameraCheckpoint rebound{fixture.native, fixture.world};
    REQUIRE(rebound.restore(baseline));
    CHECK(rebound.checkpoint() == std::optional{baseline});
}
} // namespace
