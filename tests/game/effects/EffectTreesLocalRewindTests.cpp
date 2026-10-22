#include <array>
#include <chrono>
#include <iostream>
#include <numeric>

#include <catch2/catch_test_macros.hpp>

#include "game/party/ProjectileCheckpointFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path localEffectArchive() {
    const auto root = test::projectileCheckpointArchive();
    writeTextFile(root / "animations.json", R"({
      "particles":[{"enables":16928,"texture":"WHITE","particleLife":[0.2,0.1],
        "rate":[30,30,30,30],"flags":128,"flagMask":128}],
      "trees":[
        {"name":"FLIGHT","nodes":[
          {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
          {"name":"EMITTER","parent":0,"position":[0,1,0],"particle":0}],
         "sequences":[{"name":"ACTIVE","frames":9,"frameRate":30,
           "tracks":[{"node":0,"flags":32,"frames":[0,8],"values":[0,2]}]}]},
        {"name":"NEXT","nodes":[
          {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
          {"name":"JOINT","parent":0,"position":[1,2,3]},
          {"name":"EMITTER","parent":1,"position":[0,1,0],"particle":0}],
         "sequences":[{"name":"ACTIVE","frames":5,"frameRate":30,
           "tracks":[{"node":1,"flags":16,"frames":[0,4],"values":[1,3]}]}]}]})");
    return root;
}

void compare(EffectTrees& actual, EffectTrees& expected, ItemArchive& archive,
             test::FakeRenderDevice& device) {
    REQUIRE(actual.count() == expected.count());
    CHECK(actual.textureCheckpoint(archive, {}) == expected.textureCheckpoint(archive, {}));
    CHECK(actual.presentationAdvanced() == expected.presentationAdvanced());
    for (usize i = 0; i < expected.count(); ++i) {
        const auto& a = actual.effect(i);
        const auto& b = expected.effect(i);
        CAPTURE(i, a.name, b.name);
        CHECK(a.id == b.id);
        CHECK(a.instance == b.instance);
        CHECK(a.provenance == b.provenance);
        CHECK(actual.checkpoint(a.id) == expected.checkpoint(b.id));
        CHECK(a.transform() == b.transform());
        CHECK(a.opacity() == b.opacity());
    }
    CameraFrame camera;
    camera.right = {1, 0, 0};
    camera.up = {0, 1, 0};
    device.draws.clear();
    expected.draw(device, Mat4{1}, {}, &camera, 0.37f);
    const auto draws = device.draws;
    device.draws.clear();
    actual.draw(device, Mat4{1}, {}, &camera, 0.37f);
    REQUIRE(device.draws.size() == draws.size());
    for (usize i = 0; i < draws.size(); ++i) {
        CHECK(device.draws[i].texture == draws[i].texture);
        CHECK(device.draws[i].state == draws[i].state);
        CHECK(device.draws[i].transform == draws[i].transform);
        CHECK(device.draws[i].vertices == draws[i].vertices);
    }
    device.draws.clear();
}

void tick(EffectTrees& effects, ItemArchive& archive, test::FakeRenderDevice& device, u32 frame,
          bool wrong) {
    effects.capturePresentation();
    if (frame % 3 == 0) {
        EffectTrees::Setting setting;
        setting.seconds = 0.15f + static_cast<f32>(frame % 5) / 30;
        setting.then = "NEXT";
        setting.morphIn = 0.035f;
        setting.holdForMorph = true;
        setting.shrinks = true;
        setting.scale = 1 + static_cast<f32>(frame % 4);
        setting.velocity = wrong ? Vec3{-10, 20, 0} : Vec3{3, 1, 4};
        setting.tint = frame % 2 == 0 ? Color{240, 50, 30, 210} : Color{30, 210, 50, 180};
        setting.unlit = frame % 2 == 0;
        setting.depthWrite = frame % 2 != 0;
        setting.additive = frame % 5 == 0;
        const u32 id = effects.startSet(device, archive, "FLIGHT", {1, 2, 3}, setting);
        REQUIRE(id != 0);
        REQUIRE(effects.tag(id, {1, frame + 1, 2, 3}));
    }
    if (frame % 7 == 0) {
        ParticleDescriptor descriptor;
        descriptor.emitFrames = 3;
        descriptor.fadeFrames = 2;
        descriptor.particleLife = 4;
        descriptor.rate = {1, 1, 1, 1};
        REQUIRE(effects.startParticles(device, archive, descriptor, 0,
                                       glm::translate(Mat4{1}, Vec3{5, 6, 7})) != 0);
    }
    if (effects.count() > 0 && frame % 9 == 0) {
        effects.finish(effects.effect(0).id);
    }
    if (effects.count() > 0 && frame % 11 == 0) {
        effects.stop(effects.effect(0).id);
    }
    if (effects.count() > 0 && frame % 4 == 0) {
        effects.redirect(effects.effect(0).id, wrong ? Vec3{90} : Vec3{1, 2, 3}, {4, 5, 6});
    }
    effects.update(1.0f / 60);
}

TEST_CASE("native effects rewind births morphs particle tails identities and pending clocks",
          "[local-rewind][effect-local-rewind]") {
    ItemArchive archive;
    REQUIRE(archive.load(localEffectArchive()));
    test::FakeRenderDevice device;
    EffectTrees expected;
    EffectTrees actual;
    std::array<EffectTrees::LocalSnapshot, 16> history;
    for (u32 frame = 0; frame < 128; ++frame) {
        CAPTURE(frame);
        const u32 distance = frame % 8 + 1;
        actual.saveLocal(history[frame % history.size()]);
        for (u32 i = 0; i < distance; ++i) {
            tick(actual, archive, device, frame + i, true);
            actual.draw(device, Mat4{1}, {}, nullptr, 0.6f);
            device.draws.clear();
        }
        REQUIRE(actual.restoreLocal(history[frame % history.size()]));
        compare(actual, expected, archive, device);
        for (u32 i = 0; i < distance; ++i) {
            tick(actual, archive, device, frame + i, false);
            tick(expected, archive, device, frame + i, false);
            compare(actual, expected, archive, device);
        }
    }
    CHECK(device.textureUpdates == 0);
}

TEST_CASE("native effect rewind retains artwork but not a bank's speculative first-use clock",
          "[local-rewind][effect-local-rewind]") {
    ItemArchive archive;
    REQUIRE(archive.load(localEffectArchive()));
    test::FakeRenderDevice device;
    EffectTrees effects;
    EffectTrees::LocalSnapshot cold;
    effects.saveLocal(cold);
    EffectTrees::Setting setting;
    setting.persistent = true;
    setting.settled = true;
    REQUIRE(effects.startSet(device, archive, "FLIGHT", {9, 8, 7}, setting) == 1);
    const auto model = effects.effect(0).model;
    effects.update(0.2f);
    const auto textures = device.texturesCreated;
    REQUIRE(effects.restoreLocal(cold));
    CHECK(effects.count() == 0);
    CHECK_FALSE(effects.textureCheckpoint(archive, {}));
    setting.settled = false;
    REQUIRE(effects.startSet(device, archive, "FLIGHT", {1, 2, 3}, setting) == 1);
    CHECK(effects.effect(0).model == model);
    CHECK(effects.effect(0).instance == 1);
    CHECK(effects.textureCheckpoint(archive, {}) == TextureAnimator::Clock{});
    EffectTrees reference;
    REQUIRE(reference.startSet(device, archive, "FLIGHT", {1, 2, 3}, setting) == 1);
    compare(effects, reference, archive, device);
    effects.update(1.0f / 30);
    reference.update(1.0f / 30);
    compare(effects, reference, archive, device);
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == 0);
}

TEST_CASE("native effect snapshots reject foreign closed moved and destroyed owners",
          "[local-rewind][effect-local-rewind]") {
    EffectTrees a;
    EffectTrees b;
    EffectTrees::LocalSnapshot state;
    CHECK_FALSE(a.restoreLocal(state));
    a.saveLocal(state);
    CHECK_FALSE(b.restoreLocal(state));
    a.clear();
    CHECK_FALSE(a.restoreLocal(state));
    a.saveLocal(state);
    b = std::move(a);
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(a.restoreLocal(state));
    CHECK_FALSE(b.restoreLocal(state));
    a = std::move(b);
    CHECK_FALSE(a.restoreLocal(state));
    {
        const EffectTrees temporary;
        temporary.saveLocal(state);
    }
    CHECK_FALSE(a.restoreLocal(state));
}

TEST_CASE("native crowded effect history reports copy and eight tick replay costs",
          "[.][local-rewind-timing][effect-local-rewind-timing]") {
    ItemArchive archive;
    REQUIRE(archive.load(localEffectArchive()));
    test::FakeRenderDevice device;
    device.recordDraws = false;
    EffectTrees effects;
    EffectTrees::Setting setting;
    setting.persistent = true;
    for (s32 i = 0; i < 128; ++i) {
        REQUIRE(effects.startSet(device, archive, "FLIGHT", Vec3{static_cast<f32>(i)}, setting));
    }
    effects.update(0.2f);
    std::array<EffectTrees::LocalSnapshot, 16> history;
    std::array<f64, 256> saves{};
    std::array<f64, 256> restores{};
    std::array<f64, 256> replays{};
    using Clock = std::chrono::steady_clock;
    const auto ms = [](auto start) {
        return std::chrono::duration<f64, std::milli>(Clock::now() - start).count();
    };
    for (auto& i : history) {
        effects.saveLocal(i);
    }
    for (usize i = 0; i < saves.size(); ++i) {
        auto start = Clock::now();
        effects.saveLocal(history[i % history.size()]);
        saves[i] = ms(start);
        start = Clock::now();
        REQUIRE(effects.restoreLocal(history[i % history.size()]));
        restores[i] = ms(start);
        start = Clock::now();
        for (u32 frame = 0; frame < 8; ++frame) {
            effects.capturePresentation();
            effects.update(1.0f / 60);
        }
        replays[i] = ms(start);
    }
    const auto report = [](std::string_view name, auto values) {
        std::ranges::sort(values);
        std::cout << name << " mean_ms="
                  << std::accumulate(values.begin(), values.end(), 0.0) / values.size()
                  << " p99_ms=" << values[values.size() * 99 / 100] << " max_ms=" << values.back()
                  << '\n';
    };
    report("effects128_save", saves);
    report("effects128_restore", restores);
    report("effects128_replay8", replays);
}
TEST_CASE("native effect reset retains reusable banks and rewind while clear retires them",
          "[local-rewind][effect-local-rewind]") {
    ItemArchive archive;
    REQUIRE(archive.load(localEffectArchive()));
    test::FakeRenderDevice device;
    EffectTrees effects;
    EffectTrees expected;
    EffectTrees::Setting setting;
    setting.persistent = true;
    REQUIRE(effects.startSet(device, archive, "FLIGHT", {1, 2, 3}, setting) == 1);
    REQUIRE(expected.startSet(device, archive, "FLIGHT", {1, 2, 3}, setting) == 1);
    effects.update(0.2f);
    expected.update(0.2f);
    EffectTrees::LocalSnapshot before;
    effects.saveLocal(before);
    const auto model = effects.effect(0).model;
    const auto instance = effects.effect(0).instance;
    const auto textures = device.texturesCreated;
    effects.reset();
    CHECK(effects.count() == 0);
    REQUIRE(effects.startSet(device, archive, "FLIGHT", {9, 8, 7}, setting) == 1);
    CHECK(effects.effect(0).instance > instance);
    CHECK(effects.effect(0).model == model);
    CHECK(effects.textureCheckpoint(archive, {}) == TextureAnimator::Clock{});
    effects.update(0.5f);
    REQUIRE(effects.restoreLocal(before));
    compare(effects, expected, archive, device);
    for (s32 tick = 0; tick < 8; ++tick) {
        effects.update(1.0f / 60);
        expected.update(1.0f / 60);
        compare(effects, expected, archive, device);
    }
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == 0);
    effects.clear();
    CHECK_FALSE(effects.restoreLocal(before));
}
} // namespace
