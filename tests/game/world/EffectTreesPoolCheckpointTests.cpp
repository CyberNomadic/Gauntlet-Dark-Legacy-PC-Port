#include <array>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

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
    REQUIRE(!saved->effects[0].state.particles.field.entries[0].emitter.particles.empty());
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
        const auto position = Vec3{0.1f * static_cast<f32>(tick), 2, 3};
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
        case 3: bad.effects[0].state.setting.then = "UNBOUND_FUTURE"; break;
        case 4: bad.effects[0].previousTransform[1][3] = 1; break;
        case 5: bad.effects[0].previousFrame = std::numeric_limits<f32>::quiet_NaN(); break;
        case 6: bad.effects[0].state.particles.field.entries[0].emitter.random = 0; break;
        case 7: bad.banks.clear(); break;
        case 8: bad.banks[0].clock.remainder = 1; break;
        case 9: bad.nextId = bad.effects[0].id; break;
        case 10: bad.effects[0].provenance = EffectTrees::Provenance{4, 1, 0, 0}; break;
        default:
            break;
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

TEST_CASE("direct pool capture preserves ordered sparse identities and complete native particles",
          "[effect-pool-checkpoint][checkpoint-capture-validation]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    test::ProjectileCheckpointFixture fixture;
    EffectTrees native;
    EffectTrees replay;
    std::array<u32, 48> ids{};
    for (usize index = 0; index < ids.size(); ++index) {
        EffectTrees::Setting setting;
        setting.persistent = true;
        setting.scale = 0.5f + static_cast<f32>(index) / 32;
        setting.velocity = {static_cast<f32>(index) / 8, 1, -2};
        setting.alpha = 0.25f + static_cast<f32>(index) / 96;
        ids[index] = native.startSet(fixture.device, fixture.archive, "FLIGHT",
                                     {static_cast<f32>(index), 2, 3}, setting);
        REQUIRE(ids[index] != 0);
        REQUIRE(native.tag(ids[index], {2, 71, static_cast<u16>(index), 0}));
    }
    for (usize index = 0; index < ids.size(); index += 7) {
        native.stop(ids[index]);
        CHECK_FALSE(native.checkpoint(ids[index]));
    }
    native.capturePresentation();
    native.update(0.117f);
    for (usize index = 1; index < ids.size(); index += 5) {
        native.finish(ids[index]);
    }
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    const auto saved = native.checkpointPool(fixture.resources);
    REQUIRE(saved);
    REQUIRE(saved->valid());
    REQUIRE(saved->effects.size() == native.count());
    REQUIRE(saved->effects.size() > 32);
    usize particles = 0;
    for (usize index = 0; index < native.count(); ++index) {
        const auto& live = native.effect(index);
        const auto individual = native.checkpoint(live.id);
        REQUIRE(individual);
        const auto& captured = saved->effects[index];
        CHECK(captured.state == *individual);
        CHECK(captured.id == live.id);
        CHECK(captured.instance == live.instance);
        CHECK(captured.provenance == live.provenance);
        CHECK(captured.previousTransform == live.previousTransform);
        particles += live.particles.field().particleCount();
    }
    REQUIRE(particles > 0);
    REQUIRE(replay.restorePool(*saved, fixture.resources));
    CHECK(replay.checkpointPool(fixture.resources) == saved);
    for (s32 tick = 0; tick < 8; ++tick) {
        native.capturePresentation();
        replay.capturePresentation();
        native.update(seconds);
        replay.update(seconds);
        const auto current = native.checkpointPool(fixture.resources);
        REQUIRE(current);
        CHECK(current->valid());
        CHECK(replay.checkpointPool(fixture.resources) == current);
    }
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
}

TEST_CASE("direct effect capture still rejects malformed live state and mismatched resources",
          "[effect-pool-checkpoint][checkpoint-capture-validation]") {
    test::ProjectileCheckpointFixture fixture;
    EffectTrees pool;
    const auto id = pool.startSet(fixture.device, fixture.archive, "FLIGHT", {}, {});
    REQUIRE(id != 0);
    const auto before = pool.checkpointPool(fixture.resources);
    REQUIRE(before);
    REQUIRE(before->valid());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& live = const_cast<EffectTrees::Effect&>(pool.effect(0));
    const auto lived = std::exchange(live.lived, std::numeric_limits<f32>::quiet_NaN());
    const auto individual = pool.checkpoint(id);
    REQUIRE(individual);
    CHECK_FALSE(individual->valid());
    CHECK_FALSE(pool.checkpointPool(fixture.resources));
    live.lived = lived;
    CHECK(pool.checkpointPool(fixture.resources) == before);
    auto prototype = fixture.prototypes.effect(0);
    auto resources = fixture.resources;
    resources[0].prototype = &prototype;
    prototype.emitParticles = false;
    CHECK_FALSE(pool.checkpointPool(resources));
    CHECK_FALSE(pool.prepareRestorePool(*before, resources));
    CHECK(pool.checkpointPool(fixture.resources) == before);
    auto pending = pool.prepareRestorePool(*before, fixture.resources);
    REQUIRE(pending);
    pool.clear();
    CHECK_FALSE(pool.canCommit(*pending));
    CHECK_FALSE(pool.commitRestore(std::move(*pending)));
    REQUIRE(pool.restorePool(*before, fixture.resources));
    CHECK(pool.checkpointPool(fixture.resources) == before);
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

struct MorphFixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    TextureAnimator textures;
    EffectTrees prototypes;
    std::array<EffectTrees::PoolResource, 2> resources;

    MorphFixture() {
        const auto directory = test::projectileCheckpointArchive();
        writeTextFile(directory / "animations.json", R"({
          "particles":[{"enables":16928,"texture":"WHITE","particleLife":[1,0.2],
            "rate":[30,30,30,30],"flags":128,"flagMask":128}],
          "trees":[
            {"name":"ENTRANCE","nodes":[
              {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
              {"name":"EMITTER","parent":0,"position":[0,1,0],"particle":0}],
             "sequences":[{"name":"ACTIVE","frames":4,"frameRate":30,
               "tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[0,2]}]}]},
            {"name":"LOOP","nodes":[
              {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
              {"name":"JOINT","parent":0,"position":[1,2,0]},
              {"name":"EMITTER","parent":1,"position":[0,1,0],"particle":0}],
             "sequences":[{"name":"ACTIVE","frames":9,"frameRate":30,
               "tracks":[{"node":1,"flags":16,"frames":[0,8],"values":[1,3]}]}]}]})");
        REQUIRE(archive.load(directory));
        textures.bind(archive.trees.textureAnimations(), archive.textures, device);
        EffectTrees::Setting setting;
        setting.persistent = true;
        REQUIRE(prototypes.startSet(device, archive, "ENTRANCE", {}, setting) != 0);
        REQUIRE(prototypes.startSet(device, archive, "LOOP", {}, setting) != 0);
        resources = {{{1, &archive, {}, &textures, &prototypes.effect(0)},
                      {2, &archive, {}, &textures, &prototypes.effect(1)}}};
    }
};

void compareTexture(const Texture* actual, const Texture* expected) {
    REQUIRE((actual != nullptr) == (expected != nullptr));
    if (actual == nullptr) {
        return;
    }
    const auto* a = dynamic_cast<const test::FakeTexture*>(actual);
    const auto* b = dynamic_cast<const test::FakeTexture*>(expected);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->width() == b->width());
    CHECK(a->height() == b->height());
    CHECK((a->pixels == b->pixels));
}

void compareMorphDraws(EffectTrees& native, EffectTrees& replay, test::FakeRenderDevice& device) {
    const CameraFrame camera;
    for (const f32 blend : {0.0f, 0.5f, 1.0f}) {
        device.draws.clear();
        native.draw(device, Mat4{1}, {}, &camera, blend);
        const auto expected = device.draws;
        device.draws.clear();
        replay.draw(device, Mat4{1}, {}, &camera, blend);
        REQUIRE(device.draws.size() == expected.size());
        for (usize index = 0; index < expected.size(); ++index) {
            const auto& actual = device.draws[index];
            const auto& oracle = expected[index];
            CHECK(actual.vertices == oracle.vertices);
            CHECK(actual.transform == oracle.transform);
            compareTexture(actual.texture, oracle.texture);
            compareTexture(actual.state.lightmap, oracle.state.lightmap);
            compareTexture(actual.state.maskedTexture, oracle.state.maskedTexture);
            compareTexture(actual.state.nextTexture, oracle.state.nextTexture);
            auto normalized = actual.state;
            normalized.lightmap = oracle.state.lightmap;
            normalized.maskedTexture = oracle.state.maskedTexture;
            normalized.nextTexture = oracle.state.nextTexture;
            CHECK(normalized == oracle.state);
        }
    }
}

TEST_CASE("effect pool chains restore native morph timing particle birth and presentation cuts",
          "[effect-pool-checkpoint][effect-pool-morph]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const bool timedMorph = GENERATE(false, true);
    CAPTURE(seconds, timedMorph);
    MorphFixture host;
    MorphFixture guest;
    EffectTrees native;
    EffectTrees replay;
    EffectTrees::Setting setting;
    setting.seconds = 3;
    setting.then = "LOOP";
    setting.velocity = {2, 1, 0};
    setting.stretch = {2, 0.5f, 1};
    setting.alpha = 0.75f;
    setting.unlit = true;
    setting.depthWrite = false;
    if (timedMorph) {
        setting.morphIn = 0.137f;
        setting.holdForMorph = true;
    }
    const auto retired = native.startSet(host.device, host.archive, "LOOP", {}, {});
    REQUIRE(retired != 0);
    native.stop(retired);
    const auto id = native.startSet(host.device, host.archive, "ENTRANCE", {4, 5, 6}, setting);
    REQUIRE(id > retired);
    REQUIRE(native.tag(id, {3, 51, 2, 1}));
    native.capturePresentation();
    native.update(seconds * 0.5f);
    const auto baseline = native.checkpointPool(host.resources);
    REQUIRE(baseline);
    REQUIRE(baseline->effects[0].state.setting.then == "LOOP");
    const auto textures = guest.device.texturesCreated;
    const auto uploads = guest.device.textureUpdates;
    REQUIRE(replay.restorePool(*baseline, guest.resources));
    REQUIRE(native.effect(0).morphPrototype == nullptr);
    REQUIRE(replay.effect(0).morphPrototype != nullptr);
    CHECK(replay.checkpointPool(guest.resources) == baseline);
    compareMorphDraws(native, replay, guest.device);
    bool crossedMorph = false;
    bool emittedAfterMorph = false;
    for (s32 tick = 0; tick < 35; ++tick) {
        CAPTURE(tick);
        native.capturePresentation();
        replay.capturePresentation();
        native.update(seconds);
        replay.update(seconds);
        const auto expected = native.checkpointPool(host.resources);
        REQUIRE(expected);
        CHECK(replay.checkpointPool(guest.resources) == expected);
        REQUIRE(native.effect(0).morphPrototype == nullptr);
        compareMorphDraws(native, replay, guest.device);
        const auto& entry = expected->effects[0];
        if (entry.previousResource != 0) {
            crossedMorph = true;
            REQUIRE(entry.resource == 2);
            REQUIRE(entry.previousResource == 1);
            REQUIRE(entry.previousTree);
            REQUIRE(entry.state.setting.then.empty());
            // Restore at the exact topology-changing tick too: treating its
            // previous tree as the new one would interpolate a nonexistent pose.
            EffectTrees boundary;
            REQUIRE(boundary.restorePool(*expected, guest.resources));
            CHECK(boundary.checkpointPool(guest.resources) == expected);
            compareMorphDraws(native, boundary, guest.device);
        }
        if (entry.state.name == "LOOP") {
            emittedAfterMorph |= native.effect(0).particles.field().particleCount() > 0;
        }
    }
    REQUIRE(crossedMorph);
    REQUIRE(emittedAfterMorph);
    const auto final = native.checkpointPool(host.resources);
    REQUIRE(final);
    // Rewind both independent resource owners back before the pending morph.
    REQUIRE(native.restorePool(*baseline, host.resources));
    REQUIRE(replay.restorePool(*baseline, guest.resources));
    for (s32 tick = 0; tick < 35; ++tick) {
        native.capturePresentation();
        replay.capturePresentation();
        native.update(seconds);
        replay.update(seconds);
    }
    CHECK(native.checkpointPool(host.resources) == final);
    CHECK(replay.checkpointPool(guest.resources) == final);
    CHECK(guest.device.texturesCreated == textures);
    CHECK(guest.device.textureUpdates == uploads);
}

TEST_CASE("effect pool morph catalogs reject missing future resources and forged previous trees "
          "atomically",
          "[effect-pool-checkpoint][effect-pool-morph]") {
    MorphFixture fixture;
    MorphFixture other;
    EffectTrees native;
    EffectTrees replay;
    EffectTrees::Setting setting;
    setting.persistent = true;
    setting.then = "LOOP";
    const auto id = native.startSet(fixture.device, fixture.archive, "ENTRANCE", {}, setting);
    REQUIRE(id != 0);
    const auto saved = native.checkpointPool(fixture.resources);
    REQUIRE(saved);
    REQUIRE(replay.restorePool(*saved, fixture.resources));
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    const auto limited = std::span{fixture.resources}.first(1);
    CHECK_FALSE(native.checkpointPool(limited));
    CHECK_FALSE(replay.prepareRestorePool(*saved, limited));
    CHECK(replay.checkpointPool(fixture.resources) == saved);
    for (const s32 mutation : {0, 1, 2, 3, 4}) {
        CAPTURE(mutation);
        auto resources = fixture.resources;
        auto future = fixture.prototypes.effect(1);
        auto invalid = *saved;
        switch (mutation) {
        case 0: invalid.effects[0].state.setting.then = "ABSENT"; break;
        case 1: future.device = &other.device; break;
        case 2:
            future.archive = &other.archive;
            resources[1].archive = &other.archive;
            break;
        case 3:
            future.lenders.push_back(&other.archive.textures);
            resources[1].lenders = future.lenders;
            break;
        case 4: future.particles.step(1.0f / 30, Mat4{1}, future.pose.matrices()); break;
        default:
            break;
        }
        resources[1].prototype = &future;
        CHECK_FALSE(replay.prepareRestorePool(invalid, resources));
        CHECK(replay.checkpointPool(fixture.resources) == saved);
    }
    std::optional<EffectTrees::PoolCheckpoint> morphed;
    for (s32 tick = 0; tick < 30; ++tick) {
        native.capturePresentation();
        native.update(1.0f / 60);
        auto state = native.checkpointPool(fixture.resources);
        REQUIRE(state);
        if (state->effects[0].previousResource != 0) {
            morphed = std::move(state);
            break;
        }
    }
    REQUIRE(morphed);
    for (const s32 mutation : {0, 1, 2, 3, 4, 5}) {
        CAPTURE(mutation);
        auto invalid = *morphed;
        auto resources = fixture.resources;
        auto previous = fixture.prototypes.effect(0);
        switch (mutation) {
        case 0: invalid.effects[0].previousTree = false; break;
        case 1: invalid.effects[0].previousResource = 2; break;
        case 2: invalid.effects[0].previousResource = 999; break;
        case 3: previous.device = &other.device; break;
        case 4:
            previous.archive = &other.archive;
            resources[0].archive = &other.archive;
            break;
        case 5:
            previous.lenders.push_back(&other.archive.textures);
            resources[0].lenders = previous.lenders;
            break;
        default:
            break;
        }
        resources[0].prototype = &previous;
        CHECK_FALSE(replay.prepareRestorePool(invalid, resources));
        CHECK(replay.checkpointPool(fixture.resources) == saved);
    }
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
}
} // namespace
