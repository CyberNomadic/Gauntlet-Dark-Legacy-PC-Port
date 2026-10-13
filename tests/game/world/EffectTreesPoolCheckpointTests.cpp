#include <limits>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "ProjectileCheckpointFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void compareDraws(EffectTrees& native, EffectTrees& replay, test::FakeRenderDevice& device) {
    const CameraFrame camera;
    for (const f32 blend : {0.0f, 0.5f, 1.0f}) {
        device.draws.clear();
        native.draw(device, Mat4{1}, {}, &camera, blend);
        const auto expected = device.draws;
        device.draws.clear();
        replay.draw(device, Mat4{1}, {}, &camera, blend);
        REQUIRE(device.draws.size() == expected.size());
        for (usize index = 0; index < expected.size(); ++index) {
            CHECK(device.draws[index].vertices == expected[index].vertices);
            CHECK(device.draws[index].texture == expected[index].texture);
            CHECK(device.draws[index].state == expected[index].state);
        }
    }
}

TEST_CASE("full effect pool restores native identities tails shared clocks and draw interpolation",
          "[effect-pool-checkpoint]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    test::ProjectileCheckpointFixture fixture;
    EffectTrees native;
    EffectTrees replay;
    EffectTrees::Setting setting;
    setting.persistent = true;
    const auto first = native.startSet(fixture.device, fixture.archive, "FLIGHT", {}, setting);
    REQUIRE(first != 0);
    REQUIRE(native.tag(first, {2, 49, 2, 0}));
    native.update(0.117f);
    const auto second =
        native.startSet(fixture.device, fixture.archive, "FLIGHT", {4, 5, 6}, setting);
    REQUIRE(second != 0);
    native.capturePresentation();
    native.placeAt(first, glm::translate(Mat4{1}, Vec3{1, 2, 3}), Vec3{0, 1, 2});
    native.update(seconds);
    native.finish(first);
    const auto saved = native.checkpointPool(fixture.resources);
    REQUIRE(saved);
    REQUIRE(saved->effects.size() == 2);
    REQUIRE(saved->effects[0].state.retiring);
    REQUIRE(saved->effects[0].state.particles.field.entries.size() == 1);
    REQUIRE(saved->effects[0].state.particles.field.entries[0].emitter.particles.size() > 0);
    const auto uploads = fixture.device.texturesCreated;
    auto prepared = replay.prepareRestorePool(*saved, fixture.resources);
    REQUIRE(prepared);
    CHECK(replay.count() == 0);
    REQUIRE(replay.commitRestore(std::move(*prepared)));
    CHECK(replay.checkpointPool(fixture.resources) == saved);
    compareDraws(native, replay, fixture.device);
    for (s32 tick = 0; tick < 90; ++tick) {
        CAPTURE(tick, seconds);
        native.capturePresentation();
        replay.capturePresentation();
        const auto position = Vec3{0.1f * tick, 2, 3};
        native.moveTo(second, position);
        replay.moveTo(second, position);
        native.update(seconds);
        replay.update(seconds);
        CHECK(native.checkpointPool(fixture.resources) == replay.checkpointPool(fixture.resources));
        compareDraws(native, replay, fixture.device);
    }
    CHECK_FALSE(native.playing(first));
    native.stop(second);
    replay.stop(second);
    const auto empty = native.checkpointPool(fixture.resources);
    REQUIRE(empty);
    CHECK(empty->effects.empty());
    REQUIRE(empty->banks.size() == 1);
    REQUIRE(replay.restorePool(*empty, fixture.resources));
    // Retiring all effects must not reset the clock or recycle identities.
    const auto later = native.startSet(fixture.device, fixture.archive, "FLIGHT", {}, setting);
    const auto restoredLater =
        replay.startSet(fixture.device, fixture.archive, "FLIGHT", {}, setting);
    CHECK(later == restoredLater);
    CHECK(native.checkpointPool(fixture.resources) == replay.checkpointPool(fixture.resources));
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("effect pool preparation rejects malformed state and has exact single owner lifetime",
          "[effect-pool-checkpoint]") {
    test::ProjectileCheckpointFixture fixture;
    EffectTrees pool;
    EffectTrees other;
    EffectTrees::Setting setting;
    setting.persistent = true;
    REQUIRE(pool.startSet(fixture.device, fixture.archive, "FLIGHT", {}, setting) != 0);
    pool.update(0.117f);
    const auto saved = pool.checkpointPool(fixture.resources);
    REQUIRE(saved);
    for (s32 mutation = 0; mutation < 11; ++mutation) {
        CAPTURE(mutation);
        auto bad = *saved;
        switch (mutation) {
        case 0: bad.effects[0].resource = 999; break;
        case 1: bad.effects[0].instance = 0; break;
        case 2: bad.effects.push_back(bad.effects[0]); break;
        case 3: bad.effects[0].state.setting.then = "FLIGHT"; break;
        case 4: bad.effects[0].previousTransform[1][3] = 1; break;
        case 5: bad.effects[0].previousFrame = std::numeric_limits<f32>::quiet_NaN(); break;
        case 6: bad.effects[0].state.particles.field.entries[0].emitter.random = 0; break;
        case 7: bad.banks.clear(); break;
        case 8: bad.banks[0].clock.remainder = 1; break;
        case 9: bad.nextId = bad.effects[0].id; break;
        case 10: bad.effects[0].provenance = EffectTrees::Provenance{4, 1, 0, 0}; break;
        }
        CHECK_FALSE(pool.prepareRestorePool(bad, fixture.resources));
        CHECK(pool.checkpointPool(fixture.resources) == saved);
    }
    CHECK_FALSE(pool.checkpointPool({}));
    auto prepared = pool.prepareRestorePool(*saved, fixture.resources);
    auto sibling = pool.prepareRestorePool(*saved, fixture.resources);
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK_FALSE(other.commitRestore(std::move(*prepared)));
    REQUIRE(pool.canCommit(*prepared));
    REQUIRE(pool.commitRestore(std::move(*prepared)));
    CHECK_FALSE(pool.canCommit(*prepared));
    CHECK_FALSE(pool.commitRestore(std::move(*sibling)));
    prepared = pool.prepareRestorePool(*saved, fixture.resources);
    REQUIRE(prepared);
    pool.clear();
    CHECK_FALSE(pool.commitRestore(std::move(*prepared)));
    REQUIRE(pool.restorePool(*saved, fixture.resources));
    prepared = pool.prepareRestorePool(*saved, fixture.resources);
    REQUIRE(prepared);
    EffectTrees moved{std::move(pool)};
    pool = std::move(moved);
    CHECK_FALSE(pool.canCommit(*prepared));
    CHECK(pool.checkpointPool(fixture.resources) == saved);
}

TEST_CASE("standalone emitter pool resumes exact RNG and attached trail topology",
          "[effect-pool-checkpoint]") {
    test::ProjectileCheckpointFixture fixture;
    ParticleDescriptor descriptor;
    descriptor.texture = "WHITE";
    descriptor.emitFrames = ParticleDescriptor::kEndless;
    descriptor.rate = {0.7f, 0.7f, 0.7f, 0.7f};
    descriptor.rateRandom = 0.4f;
    descriptor.particleLife = 18;
    descriptor.volume = {1, 2, 3};
    descriptor.speed = 0.3f;
    EffectTrees prototype;
    REQUIRE(prototype.startParticles(fixture.device, fixture.archive, descriptor, 0, Mat4{1}) != 0);
    const std::array resources{EffectTrees::PoolResource{
        3, &fixture.archive, {}, &fixture.textures, &prototype.effect(0)}};
    EffectTrees native;
    const auto id = native.startParticles(fixture.device, fixture.archive, descriptor, 0, Mat4{1});
    REQUIRE(id != 0);
    native.update(0.117f);
    const auto saved = native.checkpointPool(resources);
    REQUIRE(saved);
    EffectTrees replay;
    REQUIRE(replay.restorePool(*saved, resources));
    for (s32 tick = 0; tick < 80; ++tick) {
        if (tick == 10) {
            native.finish(id);
            replay.finish(id);
        }
        native.capturePresentation();
        replay.capturePresentation();
        native.update(1.0f / 60);
        replay.update(1.0f / 60);
        CHECK(native.checkpointPool(resources) == replay.checkpointPool(resources));
        compareDraws(native, replay, fixture.device);
    }
    CHECK_FALSE(native.playing(id));
}
} // namespace
