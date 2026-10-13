#include <array>
#include <limits>
#include <random>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/world/ParticleField.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeParticles.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"

namespace {
using namespace gdl;

ParticleDescriptor checkpointDescriptor() {
    ParticleDescriptor d;
    d.delay = 2;
    d.emitFrames = 31;
    d.fadeFrames = 13;
    d.forever = true;
    d.angle = 0.7f;
    d.direction = {0.1f, 0.9f, 0.4f};
    d.volume = {2, 3, 4};
    d.rate = {0.37f, 2.1f, 0.8f, 0.1f};
    d.rateRandom = 0.5f;
    d.gravity = -0.04f;
    d.speed = 0.35f;
    d.particleLife = 13;
    d.particleFade = 7;
    d.texture = "GLOW";
    d.alpha = {255, 190, 190, 0};
    d.width = {0.1f, 0.5f, 0.5f, 0};
    return d;
}

TEST_CASE("particle checkpoints resume all random branches and emission phases exactly",
          "[particle-checkpoint]") {
    const auto seed = GENERATE(0U, 1U, 891U, std::numeric_limits<u32>::max());
    const auto shape = GENERATE(0, 1, 2);
    auto descriptor = checkpointDescriptor();
    if (shape == 1) {
        descriptor.angle = ParticleDescriptor::kSphere;
    } else if (shape == 2) {
        descriptor.oneShot = true;
    }
    ParticleEmitter native;
    ParticleEmitter restored;
    native.start(descriptor, Mat4{1}, seed);
    restored.start(descriptor, Mat4{1}, seed + 2);
    std::minstd_rand expected{seed};
    auto initial = native.checkpoint();
    REQUIRE(initial.valid());
    std::minstd_rand fromCheckpoint{initial.random};
    CHECK(fromCheckpoint() == expected());
    for (u32 tick = 0; tick < 140; ++tick) {
        CAPTURE(seed, shape, tick);
        if (tick % 7 == 0) {
            const auto state = native.checkpoint();
            REQUIRE(state.valid());
            REQUIRE(restored.restore(state));
            CHECK(restored.checkpoint() == state);
        }
        const Mat4 node = glm::translate(Mat4{1}, Vec3{static_cast<f32>(tick) / 17, 2, 3});
        native.setNode(node);
        restored.setNode(node);
        native.setSpriteScale(0.75f);
        restored.setSpriteScale(0.75f);
        native.setEmitting(tick % 9 != 0);
        restored.setEmitting(tick % 9 != 0);
        if (tick == 112) {
            native.finish();
            restored.finish();
        }
        native.step(1);
        restored.step(1);
        REQUIRE(native.checkpoint().valid());
        CHECK(restored.checkpoint() == native.checkpoint());
    }
}

TEST_CASE("particle checkpoint validation rejects corruption atomically", "[particle-checkpoint]") {
    ParticleEmitter emitter;
    emitter.start(checkpointDescriptor(), Mat4{1}, 117);
    emitter.step(9);
    const auto before = emitter.checkpoint();
    REQUIRE(before.valid());
    REQUIRE_FALSE(before.particles.empty());
    for (const s32 mutation : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
        CAPTURE(mutation);
        auto bad = before;
        switch (mutation) {
        case 0: bad.random = 0; break;
        case 1: bad.random = static_cast<u32>(std::minstd_rand::modulus); break;
        case 2: bad.saved = std::numeric_limits<f32>::quiet_NaN(); break;
        case 3: bad.phase = static_cast<ParticleEmitter::Phase>(255); break;
        case 4: bad.particles[0].age = -1; break;
        case 5: bad.node[0][3] = 1; break;
        case 6: bad.descriptor.particleLife = std::numeric_limits<u32>::max(); break;
        case 7: bad.descriptor.rate[0] = std::numeric_limits<f32>::infinity(); break;
        case 8: bad.particles.resize(4097); break;
        case 9: bad.age = std::numeric_limits<u32>::max(); break;
        }
        CHECK_FALSE(bad.valid());
        CHECK_FALSE(emitter.restore(bad));
        CHECK(emitter.checkpoint() == before);
    }
    auto differentlyBound = before;
    differentlyBound.descriptor.speed += 0.1f;
    REQUIRE(differentlyBound.valid());
    CHECK_FALSE(emitter.restore(differentlyBound));
    CHECK(emitter.checkpoint() == before);
}

TEST_CASE("particle field checkpoints preserve fractional ticks and prebound draw resources",
          "[particle-checkpoint]") {
    test::FakeTexture texture{1, 1};
    ParticleField field;
    ParticleField replay;
    for (const u32 seed : {17U, 33U}) {
        field.start(checkpointDescriptor(), Mat4{1}, &texture, seed);
        replay.start(checkpointDescriptor(), Mat4{1}, &texture, seed + 7);
    }
    field.step(0.117f);
    const auto baseline = field.checkpoint();
    REQUIRE(baseline.valid());
    REQUIRE(baseline.frameRemainder != 0);
    REQUIRE(replay.restore(baseline));
    CHECK(replay.textureOf(0) == &texture);
    for (s32 tick = 0; tick < 70; ++tick) {
        const auto at = glm::translate(Mat4{1}, Vec3{static_cast<f32>(tick), 2, 3});
        field.setNode(0, at);
        replay.setNode(0, at);
        const f32 seconds = tick % 3 == 0 ? 0.013f : 0.02f;
        field.step(seconds);
        replay.step(seconds);
        CHECK(replay.checkpoint() == field.checkpoint());
    }
    const auto intact = replay.checkpoint();
    auto bad = baseline;
    bad.entries.back().emitter.random = 0;
    CHECK_FALSE(replay.restore(bad));
    CHECK(replay.checkpoint() == intact);
    bad = baseline;
    bad.entries.back().worldObject = 1;
    REQUIRE(bad.valid());
    CHECK_FALSE(replay.restore(bad));
    CHECK(replay.checkpoint() == intact);
    bad = baseline;
    bad.entries.pop_back();
    CHECK_FALSE(replay.restore(bad));
    CHECK(replay.checkpoint() == intact);
}

TEST_CASE("tree particle checkpoints retain trusted node bindings across moved poses",
          "[particle-checkpoint]") {
    const auto root = test::scratchDirectory("tree-particle-checkpoint");
    writeFile(root / "glow.png", test::kTinyPng);
    writeTextFile(root / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"GLOW","file":"glow.png","width":2,"height":2}]})");
    writeTextFile(root / "animations.json", R"({
      "particles":[{"enables":16928,"texture":"GLOW","particleLife":[1,0],
                    "rate":[30,30,30,30],"flags":128,"flagMask":128}],
      "trees":[{"name":"TEST","nodes":[
        {"name":"EMITTER","parent":-1,"position":[0,0,0],"particle":0}]}]})");
    ItemArchive archive;
    REQUIRE(archive.trees.load(root));
    REQUIRE(archive.textures.load(root));
    test::FakeRenderDevice device;
    TreeParticles native;
    TreeParticles replay;
    const std::array pose{Mat4{1}};
    native.bind(archive.trees.tree(0), archive, device, Mat4{1}, pose);
    replay.bind(archive.trees.tree(0), archive, device, Mat4{1}, pose);
    native.step(0.15f, Mat4{1}, pose);
    const auto state = native.checkpoint();
    REQUIRE(state.valid());
    const auto uploads = device.texturesCreated;
    REQUIRE(replay.restore(state));
    for (s32 tick = 0; tick < 20; ++tick) {
        const auto at = glm::translate(Mat4{1}, Vec3{static_cast<f32>(tick), 1, 3});
        native.step(1.0f / 60, at, pose);
        replay.step(1.0f / 60, at, pose);
        CHECK(replay.checkpoint() == native.checkpoint());
    }
    const auto before = replay.checkpoint();
    auto bad = before;
    bad.nodes[0].node = 1;
    REQUIRE(bad.valid());
    CHECK_FALSE(replay.restore(bad));
    CHECK(replay.checkpoint() == before);
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("texture bank checkpoint rewinds fractional presentation without changing offsets",
          "[particle-checkpoint][texture-checkpoint]") {
    test::FakeRenderDevice device;
    TextureSet textures;
    TextureAnimationInfo scroll;
    scroll.texture = 0;
    scroll.source = TextureAnimationInfo::kScrollU;
    scroll.frames = -7;
    scroll.start = 2;
    scroll.rate = 3;
    TextureAnimator native;
    TextureAnimator replay;
    const std::array animations{scroll};
    native.bind(animations, textures, device);
    replay.bind(animations, textures, device);
    REQUIRE(native.size() == 1);
    native.advance(0.117f);
    const auto state = native.checkpoint();
    REQUIRE(state.valid());
    replay.advance(0.8f);
    REQUIRE(replay.restore(state));
    CHECK(replay.checkpoint() == state);
    for (s32 tick = 0; tick < 40; ++tick) {
        native.advance(0.012f);
        replay.advance(0.012f);
        CHECK(replay.checkpoint() == native.checkpoint());
        CHECK(replay.counter(0) == native.counter(0));
        CHECK(replay.motion(0, replay.presentationOffset(0.3f)).offset ==
              native.motion(0, native.presentationOffset(0.3f)).offset);
    }
    const auto before = replay.checkpoint();
    auto bad = before;
    bad.remainder = 1;
    CHECK_FALSE(replay.restore(bad));
    CHECK(replay.checkpoint() == before);
}
} // namespace
