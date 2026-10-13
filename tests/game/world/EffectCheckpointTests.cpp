#include <array>
#include <filesystem>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/world/EffectTrees.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path checkpointArchive() {
    const auto root = test::scratchDirectory("effect-checkpoint");
    writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
    writeFile(root / "white.png", test::kTinyPng);
    writeTextFile(root / "textures.json", R"({"bitmaps":[
      {"name":"WHITE","file":"white.png","width":2,"height":2}]})");
    writeTextFile(root / "animations.json", R"({
      "particles":[{"enables":16928,"texture":"WHITE","particleLife":[1,0],
                    "rate":[30,30,30,30],"flags":128,"flagMask":128}],
      "trees":[{"name":"MOVING","nodes":[
        {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
        {"name":"EMITTER","parent":0,"position":[0,1,0],"particle":0}],
        "sequences":[{"name":"ACTIVE","frames":9,"frameRate":30,
          "tracks":[{"node":0,"flags":32,"frames":[0,8],"values":[0,2]}]}]},
        {"name":"STILL","nodes":[
          {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]}]})");
    test::convertModelFixture(root);
    return root;
}

ParticleDescriptor trailDescriptor() {
    ParticleDescriptor descriptor;
    descriptor.emitFrames = ParticleDescriptor::kEndless;
    descriptor.rate = {0.71f, 0.71f, 0.71f, 0.71f};
    descriptor.rateRandom = 0.4f;
    descriptor.volume = {1, 2, 3};
    descriptor.angle = 0.7f;
    descriptor.speed = 0.3f;
    descriptor.particleLife = 18;
    descriptor.particleFade = 12;
    descriptor.alpha = {255, 180, 180, 0};
    descriptor.width = {0.5f, 0.25f, 0.25f, 0};
    descriptor.texture = "WHITE";
    return descriptor;
}

void sameDraws(EffectTrees& native, EffectTrees& replay, test::FakeRenderDevice& device) {
    CameraFrame camera;
    camera.right = {1, 0, 0};
    camera.up = {0, 1, 0};
    device.draws.clear();
    native.draw(device, Mat4{1}, {}, &camera, 1);
    const auto expected = device.draws;
    device.draws.clear();
    replay.draw(device, Mat4{1}, {}, &camera, 1);
    REQUIRE(device.draws.size() == expected.size());
    for (usize i = 0; i < expected.size(); ++i) {
        CHECK(device.draws[i].vertices == expected[i].vertices);
        CHECK(device.draws[i].texture == expected[i].texture);
        CHECK(device.draws[i].state == expected[i].state);
    }
}

TEST_CASE("effect checkpoints replay native poses particles and detached flight without rebinding",
          "[effect-checkpoint]") {
    const bool attached = GENERATE(false, true);
    const bool repeat = GENERATE(false, true);
    ItemArchive archive;
    REQUIRE(archive.load(checkpointArchive()));
    test::FakeRenderDevice device;
    EffectTrees native;
    EffectTrees replay;
    EffectTrees::Setting setting;
    setting.seconds = 4;
    setting.loop = repeat;
    setting.scale = 1.2f;
    setting.stretch = {0.7f, 1.1f, 1.3f};
    setting.fadeSeconds = 0.3f;
    setting.yaw = 0.7f;
    setting.velocity = {0.1f, 0.2f, 0.3f};
    setting.alpha = 0.7f;
    setting.playbackRate = 1.3f;
    setting.additive = true;
    setting.depthWrite = false;
    setting.unlit = true;
    setting.shrinks = true;
    setting.morphIn = 0.2f;
    setting.holdForMorph = true;
    setting.tint = Color::rgba(90, 130, 180, 210);
    setting.light = EffectTrees::Light{{0.1f, 0.2f, 0.7f}, 3, true};
    const u32 source = native.startSet(device, archive, "MOVING", {1, 2, 3}, setting);
    const u32 destination = replay.startSet(device, archive, "MOVING", {}, setting);
    REQUIRE(source != 0);
    REQUIRE(destination != 0);
    native.attachTrail(source, trailDescriptor(), device.whiteTexture());
    replay.attachTrail(destination, trailDescriptor(), device.whiteTexture());
    native.update(0.117f);
    REQUIRE(native.effect(0).particles.field().particleCount() > 0);
    const auto sourceState = native.checkpoint(source);
    const auto sourceClock = native.textureCheckpoint(archive, {});
    REQUIRE(sourceState);
    REQUIRE(sourceState->valid());
    REQUIRE(sourceClock);
    const u32 uploads = device.texturesCreated;
    const auto instance = replay.effect(0).instance;
    REQUIRE(replay.restoreTextureClock(archive, {}, *sourceClock));
    REQUIRE(replay.restore(destination, *sourceState));
    CHECK(replay.checkpoint(destination) == sourceState);
    CHECK(replay.effect(0).instance == instance);
    CHECK_FALSE(replay.effect(0).presentationCaptured);
    for (s32 step = 0; step < 80; ++step) {
        CAPTURE(attached, repeat, step);
        native.capturePresentation();
        replay.capturePresentation();
        if (attached) {
            const Mat4 at = glm::translate(glm::rotate(Mat4{1}, 0.13f * step, Vec3{0, 1, 0}),
                                           Vec3{0.01f * step, 2, 3});
            native.placeAt(source, at, Vec3{1, 2, 3});
            replay.placeAt(destination, at, Vec3{1, 2, 3});
        }
        if (step == 20) {
            native.redirect(source, {5, 6, 7}, {-0.1f, 0.3f, 0.7f});
            replay.redirect(destination, {5, 6, 7}, {-0.1f, 0.3f, 0.7f});
        }
        const f32 seconds = step % 4 == 0 ? 0.0f : 0.0167f;
        native.update(seconds);
        replay.update(seconds);
        REQUIRE(native.playing(source));
        CHECK(native.checkpoint(source) == replay.checkpoint(destination));
        CHECK(native.textureCheckpoint(archive, {}) == replay.textureCheckpoint(archive, {}));
        sameDraws(native, replay, device);
    }
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("effect checkpoint failures are atomic and reject resource topology changes",
          "[effect-checkpoint]") {
    ItemArchive archive;
    REQUIRE(archive.load(checkpointArchive()));
    test::FakeRenderDevice device;
    EffectTrees effects;
    EffectTrees::Setting setting;
    setting.persistent = true;
    const auto id = effects.startSet(device, archive, "MOVING", {}, setting);
    REQUIRE(id != 0);
    effects.update(0.117f);
    const auto state = effects.checkpoint(id);
    REQUIRE(state);
    REQUIRE(state->valid());
    for (const s32 mutation : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
        CAPTURE(mutation);
        auto bad = *state;
        switch (mutation) {
        case 0: bad.position.x = std::numeric_limits<f32>::quiet_NaN(); break;
        case 1: bad.animation.sequence = 10000; break;
        case 2: bad.particles.field.entries[0].emitter.random = 0; break;
        case 3: bad.particles.nodes[0].node = 10; break;
        case 4: bad.name = "STILL"; break;
        case 5: bad.setting.then = "STILL"; break;
        case 6: bad.setting.missingObjectsAreEmpty = true; break;
        case 7: bad.particles.field.entries[0].emitter.descriptor.speed += 0.5f; break;
        case 8: bad.setting.playbackRate = std::numeric_limits<f32>::infinity(); break;
        case 9: bad.setting.emitParticles = false; break;
        }
        CHECK_FALSE(effects.restore(id, bad));
        CHECK(effects.checkpoint(id) == state);
    }
    CHECK_FALSE(effects.restore(id + 1, *state));
    CHECK_FALSE(effects.checkpoint(id + 1));
    const auto clock = effects.textureCheckpoint(archive, {});
    REQUIRE(clock);
    auto badClock = *clock;
    badClock.remainder = 1;
    CHECK_FALSE(effects.restoreTextureClock(archive, {}, badClock));
    CHECK(effects.textureCheckpoint(archive, {}) == clock);
    TextureSet other;
    const std::array<TextureSet*, 1> wrongLender{&other};
    CHECK_FALSE(effects.restoreTextureClock(archive, wrongLender, *clock));
    CHECK_FALSE(effects.textureCheckpoint(archive, wrongLender));
}

TEST_CASE("retired and standalone particle effects resume exact native tails",
          "[effect-checkpoint]") {
    const bool standalone = GENERATE(false, true);
    ItemArchive archive;
    REQUIRE(archive.load(checkpointArchive()));
    test::FakeRenderDevice device;
    EffectTrees native;
    EffectTrees replay;
    const auto start = [&](EffectTrees& pool) {
        if (standalone) {
            return pool.startParticles(device, archive, trailDescriptor(), 0, Mat4{1});
        }
        EffectTrees::Setting setting;
        setting.persistent = true;
        return pool.startSet(device, archive, "MOVING", {}, setting);
    };
    const auto source = start(native);
    const auto destination = start(replay);
    REQUIRE(source != 0);
    REQUIRE(destination != 0);
    native.update(0.117f);
    native.finish(source);
    const auto state = native.checkpoint(source);
    REQUIRE(state);
    REQUIRE(state->valid());
    REQUIRE(state->retiring);
    REQUIRE(replay.restoreTextureClock(archive, {}, *native.textureCheckpoint(archive, {})));
    REQUIRE(replay.restore(destination, *state));
    for (s32 tick = 0; tick < 90; ++tick) {
        native.update(1.0f / 60);
        replay.update(1.0f / 60);
        CHECK(native.checkpoint(source) == replay.checkpoint(destination));
        sameDraws(native, replay, device);
    }
    CHECK_FALSE(native.playing(source));
    CHECK_FALSE(replay.playing(destination));
}

TEST_CASE("effect restore refuses rewind across a native tree morph", "[effect-checkpoint]") {
    ItemArchive archive;
    REQUIRE(archive.load(checkpointArchive()));
    test::FakeRenderDevice device;
    EffectTrees effects;
    EffectTrees::Setting setting;
    setting.seconds = 3;
    setting.then = "STILL";
    setting.morphIn = 0.1f;
    const auto id = effects.startSet(device, archive, "MOVING", {}, setting);
    REQUIRE(id != 0);
    const auto birth = effects.checkpoint(id);
    REQUIRE(birth);
    effects.update(0.15f);
    const auto changed = effects.checkpoint(id);
    REQUIRE(changed);
    REQUIRE(changed->name == "STILL");
    CHECK_FALSE(effects.restore(id, *birth));
    CHECK(effects.checkpoint(id) == changed);
}
} // namespace
