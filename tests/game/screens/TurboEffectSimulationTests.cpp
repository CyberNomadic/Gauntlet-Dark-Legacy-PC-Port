#include <array>
#include <filesystem>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/screens/TurboEffectSimulation.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path simulationArchive() {
    const auto root = test::scratchDirectory("turbo-effect-simulation");
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
        {"name":"STILL","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]},
        {"name":"EMPTY","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"ACTIVE","frames":0,"frameRate":30}]}]})");
    test::convertModelFixture(root);
    return root;
}

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    ClassStats stats;
    TurboEffectResources resources;
    Fixture() {
        REQUIRE(archive.load(simulationArchive()));
        stats.moves.turboB = 0;
        stats.moveStrikes.resize(3);
        stats.moveEffects.resize(5);
        stats.moveStrikes[0].effect = 0;
        stats.moveStrikes[1].effect = 3;
        stats.moveStrikes[2].effect = 4;
        stats.moves.turboC1 = 1;
        stats.moves.turboC2 = 2;
        stats.moveEffects[0].tree = "MOVING";
        stats.moveEffects[0].next = 1;
        stats.moveEffects[0].offset = {0.5f, 0.3f, -0.2f};
        stats.moveEffects[0].lifetime = 0.9f;
        stats.moveEffects[0].scale = 1.2f;
        stats.moveEffects[1].tree = "WHITE";
        stats.moveEffects[1].flags = 0x01000000;
        stats.moveEffects[1].radius = 0.75f;
        stats.moveEffects[1].scale = 0.5f;
        stats.moveEffects[1].alphaMod = 250;
        stats.moveEffects[1].lifetime = 0.25f;
        stats.moveEffects[1].offset = {0.1f, 0.2f, 0.3f};
        stats.moveEffects[1].sound = "HAND";
        stats.moveEffects[1].next = 2;
        stats.moveEffects[2].tree = "STILL";
        stats.moveEffects[2].flags = 1;
        stats.moveEffects[2].lifetime = 0.6f;
        stats.moveEffects[3].tree = "MOVING";
        stats.moveEffects[3].lifetime = 0.25f;
        stats.moveEffects[4].tree = "EMPTY";
        REQUIRE(resources.bind(device, 0, stats, archive, archive));
    }

    TurboEmission::Strike strike(s32 row = 0) const {
        const auto result = resources.prepare(row, {0, {1, 2, 3}, {0, 0, 1}}, 10);
        REQUIRE(result);
        return *result;
    }
};

TurboEffectSimulation::Pose pose(s32 tick) {
    TurboEffectSimulation::Pose result;
    result.body = glm::translate(Mat4{1}, Vec3{0.01f * tick, 2, 3});
    result.body = glm::rotate(result.body, 0.07f * tick, Vec3{0, 1, 0});
    result.root = glm::translate(result.body, Vec3{0, 0.2f, 0});
    result.nodes.emplace("HAND", glm::translate(result.body, Vec3{1, 1, 0}));
    return result;
}

std::vector<u32> nativeStart(Fixture& fixture, EffectTrees& native,
                             const TurboEmission::Strike& strike) {
    std::vector<u32> ids;
    for (const auto& emission : strike.effects) {
        const u32 id =
            emission.particle
                ? native.startParticles(fixture.device, fixture.archive,
                                        emission.particle->descriptor, 0, Mat4{1})
                : native.startSet(fixture.device, fixture.archive, emission.authored.tree,
                                  emission.position, emission.setting);
        REQUIRE(id != 0);
        ids.push_back(id);
    }
    return ids;
}

void nativePlace(EffectTrees& native, const TurboEmission::Strike& strike, std::span<const u32> ids,
                 const TurboEffectSimulation::Pose& at) {
    for (usize i = 0; i < ids.size(); ++i) {
        const auto& emission = strike.effects[i];
        if (emission.particle) {
            const auto& particle = *emission.particle;
            Mat4 parent = at.body;
            if (!particle.node.empty()) {
                parent = at.nodes.at(particle.node).value_or(parent);
            }
            native.placeAt(ids[i], glm::translate(parent, particle.offset));
        } else if (emission.attachment) {
            const auto& attachment = *emission.attachment;
            native.placeAt(ids[i], (attachment.bodyParent ? at.body : at.root) * attachment.local);
        }
    }
}

void same(Fixture& fixture, const EffectTrees& native, const TurboEffectSimulation& replay,
          std::span<const u32> ids, const EffectIdentity& base) {
    REQUIRE(native.count() == replay.effects().count());
    for (usize ordinal = 0; ordinal < ids.size(); ++ordinal) {
        auto origin = base;
        origin.effectOrdinal = static_cast<u16>(ordinal);
        const auto expected = native.checkpoint(ids[ordinal]);
        std::optional<EffectTrees::Checkpoint> actual;
        for (const auto& entry : replay.entries()) {
            if (entry.origin == origin) {
                actual = replay.effects().checkpoint(entry.id);
            }
        }
        CHECK(actual == expected);
    }
    CHECK(native.textureCheckpoint(fixture.archive, {}) ==
          replay.effects().textureCheckpoint(fixture.archive, {}));
    CameraFrame camera;
    fixture.device.draws.clear();
    native.draw(fixture.device, Mat4{1}, {}, &camera);
    const auto draws = fixture.device.draws;
    fixture.device.draws.clear();
    replay.effects().draw(fixture.device, Mat4{1}, {}, &camera);
    REQUIRE(fixture.device.draws.size() == draws.size());
    for (usize i = 0; i < draws.size(); ++i) {
        CHECK(fixture.device.draws[i].vertices == draws[i].vertices);
        CHECK(fixture.device.draws[i].state == draws[i].state);
        CHECK(fixture.device.draws[i].transform == draws[i].transform);
        CHECK(fixture.device.draws[i].texture == draws[i].texture);
    }
}

TEST_CASE("prepared turbo effects match native clocks particle RNG and moving attachments",
          "[turbo-effect-simulation]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    Fixture fixture;
    const auto strike = fixture.strike();
    EffectTrees native;
    const auto ids = nativeStart(fixture, native, strike);
    std::vector<std::optional<u32>> seeds(ids.begin(), ids.end());
    const EffectIdentity origin{1, 10, 5, 2, 0, 3, 0};
    TurboEffectSimulation replay;
    replay.bind(fixture.resources);
    const auto uploads = fixture.device.texturesCreated;
    REQUIRE(replay.start(origin, strike, pose(0), seeds));
    nativePlace(native, strike, ids, pose(0));
    same(fixture, native, replay, ids, origin);
    for (s32 tick = 0; tick < 100; ++tick) {
        CAPTURE(tick, seconds);
        native.capturePresentation();
        nativePlace(native, strike, ids, pose(tick));
        native.update(seconds);
        REQUIRE(replay.step(seconds, pose(tick)));
        same(fixture, native, replay, ids, origin);
        REQUIRE(replay.start(origin, strike, pose(tick), seeds)); // replay is not a second event
    }
    CHECK(replay.entries().empty());
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.device.textureUpdates == 0);
}

TEST_CASE("turbo effects restore exact native state and retained shared bank clocks",
          "[turbo-effect-simulation]") {
    Fixture fixture;
    const auto strike = fixture.strike();
    EffectTrees native;
    const auto ids = nativeStart(fixture, native, strike);
    std::vector<std::optional<u32>> seeds(ids.begin(), ids.end());
    const EffectIdentity origin{1, 10, 5, 2, 0, 3, 0};
    TurboEffectSimulation replay;
    replay.bind(fixture.resources);
    REQUIRE(replay.start(origin, strike, pose(0), seeds));
    for (s32 tick = 0; tick < 7; ++tick) {
        nativePlace(native, strike, ids, pose(tick));
        native.update(1.0f / 60);
        REQUIRE(replay.step(1.0f / 60, pose(tick)));
    }
    const auto saved = replay.checkpoint();
    REQUIRE(saved.effects.size() == 3);
    REQUIRE(saved.banks.size() == 1);
    REQUIRE(saved.effects[0].effect.particles.field.entries[0].emitter.particles.size() > 0);
    for (s32 tick = 7; tick < 50; ++tick) {
        REQUIRE(replay.step(1.0f / 60, pose(tick)));
    }
    const auto uploads = fixture.device.texturesCreated;
    REQUIRE(replay.restore(saved));
    same(fixture, native, replay, ids, origin);
    for (s32 tick = 7; tick < 130; ++tick) {
        nativePlace(native, strike, ids, pose(tick));
        native.update(1.0f / 60);
        REQUIRE(replay.step(1.0f / 60, pose(tick)));
        same(fixture, native, replay, ids, origin);
    }
    REQUIRE(replay.entries().empty());
    const auto empty = replay.checkpoint();
    REQUIRE(empty.banks.size() == 1);
    REQUIRE(empty.effects.empty());
    TurboEffectSimulation restored;
    restored.bind(fixture.resources);
    REQUIRE(restored.restore(empty));
    CHECK(restored.effects().textureCheckpoint(fixture.archive, {}) ==
          native.textureCheckpoint(fixture.archive, {}));
    const auto second = fixture.strike(1);
    auto nextOrigin = origin;
    ++nextOrigin.tick;
    ++nextOrigin.generation;
    const auto nextIds = nativeStart(fixture, native, second);
    REQUIRE(restored.start(nextOrigin, second, pose(130)));
    nativePlace(native, second, nextIds, pose(130));
    same(fixture, native, restored, nextIds, nextOrigin);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("turbo effect replay rejects unavailable seeds unsampled attachments and foreign epochs",
          "[turbo-effect-simulation]") {
    Fixture fixture;
    TurboEffectSimulation replay;
    replay.bind(fixture.resources);
    const auto strike = fixture.strike();
    const EffectIdentity origin{1, 10, 5, 2, 0, 3, 0};
    const std::array<std::optional<u32>, 3> seeds{1, 2, 3};
    CHECK_FALSE(replay.start(origin, strike, pose(0)));
    CHECK(replay.entries().empty());
    CHECK(replay.checkpoint().banks.empty());
    auto unsampled = pose(0);
    unsampled.nodes.clear();
    CHECK_FALSE(replay.start(origin, strike, unsampled, seeds));
    unsampled.nodes.emplace("HAND", std::nullopt);
    REQUIRE(replay.start(origin, strike, unsampled, seeds));
    auto foreign = origin;
    ++foreign.epoch;
    CHECK_FALSE(replay.start(foreign, strike, pose(0), seeds));
    REQUIRE(replay.step(0.1f, pose(0)));
    const auto before = replay.checkpoint();
    CHECK_FALSE(replay.step(std::numeric_limits<f32>::quiet_NaN(), pose(0)));
    CHECK_FALSE(replay.step(-0.1f, pose(0)));
    auto bad = before;
    bad.effects[0].effect.particles.field.entries[0].emitter.random = 0;
    CHECK_FALSE(replay.restore(bad));
    CHECK(replay.checkpoint().effects[0].effect == before.effects[0].effect);
    bad = before;
    bad.banks.push_back(bad.banks[0]);
    CHECK_FALSE(replay.restore(bad));
    CHECK(replay.checkpoint().effects[0].effect == before.effects[0].effect);
    replay.clear();
    CHECK_FALSE(replay.start(origin, strike, pose(0), seeds));
    replay.bind(fixture.resources);
    REQUIRE(replay.start(foreign, strike, pose(0), seeds));
}

TEST_CASE("prepared tree initialization retains native empty sequence and still lifetimes",
          "[turbo-effect-simulation]") {
    Fixture fixture;
    const auto strike = fixture.strike(2);
    EffectTrees native;
    const auto ids = nativeStart(fixture, native, strike);
    TurboEffectSimulation replay;
    replay.bind(fixture.resources);
    const EffectIdentity origin{1, 10, 5, 2, 0, 3, 0};
    REQUIRE(replay.start(origin, strike, pose(0)));
    nativePlace(native, strike, ids, pose(0));
    REQUIRE(native.effect(0).timed);
    for (s32 tick = 0; tick < 70; ++tick) {
        nativePlace(native, strike, ids, pose(tick));
        native.update(1.0f / 60);
        REQUIRE(replay.step(1.0f / 60, pose(tick)));
        same(fixture, native, replay, ids, origin);
    }
    CHECK(replay.entries().empty());
}
} // namespace
