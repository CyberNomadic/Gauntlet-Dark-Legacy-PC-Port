#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/level/SkorneArena.h"
#include "game/rollback/LocalStateHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Skorne masonry waits for cues, lifts twice, scatters and retires", "[skorne]") {
    const auto directory = test::scratchDirectory("skorne-arena");
    writeTextFile(directory / "world.json", R"({"objects":[{"name":"ROOT","position":[0,0,0]}],
      "bounds":{"min":[-20,-10,-20],"max":[20,20,20]},
      "itemInfos":[{"type":10,"subtype":51}],
      "itemInstances":[{"info":0,"name":"ROCK","position":[10,0,0]}]})");
    writeTextFile(directory / "objects.json", R"({"objects":[
      {"index":0,"name":"ROCK","file":"rock.obj","meshTriangles":1}]})");
    writeTextFile(directory / "rock.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl tex0\nf 1 2 3\n");
    writeFile(directory / "skin.png", test::kTinyPng);
    writeTextFile(directory / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    test::FakeRenderDevice device;
    test::convertModelFixture(directory);
    REQUIRE(layout.load(directory));
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    SkorneArena arena;
    arena.bind(device, layout, models, textures);
    REQUIRE(arena.size() == 1);
    arena.update(2);
    REQUIRE(arena.position(0) == Vec3{10, 0, 0});
    arena.cue(Vec3{0});
    arena.update(1.0f / 30);
    const f32 first = arena.position(0).y;
    REQUIRE(first >= 18.0f / 30);
    REQUIRE(first < 28.0f / 30);
    device.draws.clear();
    arena.draw(device, Mat4{1}, {}, 0.5f);
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].vertices[0].position.y == Catch::Approx(first * 0.5f));
    CHECK(arena.position(0).y == first);
    arena.cue(Vec3{0});
    arena.update(1.0f / 30);
    REQUIRE(arena.position(0).y >= first + 28.0f / 30);
    REQUIRE(arena.position(0).x == 10);
    arena.cue(Vec3{0});
    arena.update(1.0f / 30);
    REQUIRE(arena.position(0).x > 10);
    REQUIRE(arena.phase() == 3);
    arena.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    arena.update(20);
    REQUIRE_FALSE(arena.visible(0));
    arena.cue(Vec3{0});
    REQUIRE_FALSE(arena.visible(0));
    device.draws.clear();
    arena.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.empty());
    arena.clear();
    REQUIRE(arena.size() == 0);
    REQUIRE(arena.phase() == 0);
}

TEST_CASE("altar entrance loads all twenty-two loose masonry meshes", "[skorne][assets]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELE2/WORLDS.PS2").parent_path();
    test::assetOrSkip("LEVELS/LEVELE2/objects.ngc");
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(directory));
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    SkorneArena arena;
    arena.bind(device, layout, models, textures);
    REQUIRE(arena.size() == 22);
    arena.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() >= 22);
}
struct RewindFixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    SkorneArena arena;

    explicit RewindFixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0]}],
          "bounds":{"min":[-20,-10,-20],"max":[20,20,20]},
          "itemInfos":[{"type":10,"subtype":51}],
          "itemInstances":[
            {"info":0,"name":"WALL","position":[10,0,0]},
            {"info":0,"name":"WALL","position":[-10,2,5]},
            {"info":0,"name":"WALL","position":[0,-1,0]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(models.load(directory));
        REQUIRE(textures.load(directory));
        arena.bind(device, layout, models, textures);
        REQUIRE(arena.size() == 3);
    }
    void step(u64 tick) {
        if (tick == 3 || tick == 12 || tick == 40) {
            arena.cue({0, 0, 0});
        }
        arena.update(tick % 13 == 0 ? 0.0f : 1.0f / 60);
    }
};

TEST_CASE("native altar rewind retains random lifts cues fractional clocks and falling poses",
          "[local-rewind][skorne-arena-local-rewind]") {
    RewindFixture reference("skorne-native-reference");
    RewindFixture corrected("skorne-native-corrected");
    LocalStateHistory<SkorneArena::LocalSnapshot> history;
    std::array<SkorneArena::LocalSnapshot, 8> expected;
    SkorneArena::LocalSnapshot observed;
    const auto uploads = corrected.device.texturesCreated;
    const auto updates = corrected.device.textureUpdates;
    u64 tick = 0;
    for (u64 window = 0; window < 160; ++window) {
        CAPTURE(window, tick);
        REQUIRE(history.capture(tick, [&](auto& saved) {
            corrected.arena.saveLocal(saved);
            return true;
        }));
        const u64 distance = 1 + window % 8;
        const auto* saved = history.find(tick);
        REQUIRE(saved);
        for (u64 offset = 0; offset < distance; ++offset) {
            corrected.step(tick + offset);
            corrected.arena.saveLocal(expected[offset]);
        }
        REQUIRE(corrected.arena.restoreLocal(*saved));
        for (u64 offset = 0; offset < distance; ++offset) {
            reference.step(tick + offset);
            corrected.arena.cue({-20, 10, 20});
            corrected.arena.update(0.13f);
        }
        REQUIRE(corrected.arena.restoreLocal(*saved));
        for (u64 offset = 0; offset < distance; ++offset) {
            corrected.step(tick + offset);
            corrected.arena.saveLocal(observed);
            CHECK(corrected.arena.equalLocal(observed, expected[offset]));
        }
        CHECK(corrected.arena.phase() == reference.arena.phase());
        for (usize index = 0; index < reference.arena.size(); ++index) {
            CHECK(corrected.arena.position(index) == reference.arena.position(index));
            CHECK(corrected.arena.visible(index) == reference.arena.visible(index));
        }
        for (const f32 alpha : {-1.0f, 0.0f, 0.5f, 1.0f}) {
            reference.device.draws.clear();
            corrected.device.draws.clear();
            reference.arena.draw(reference.device, Mat4{1}, {}, alpha);
            corrected.arena.draw(corrected.device, Mat4{1}, {}, alpha);
            REQUIRE(corrected.device.draws.size() == reference.device.draws.size());
            for (usize draw = 0; draw < reference.device.draws.size(); ++draw) {
                CHECK(corrected.device.draws[draw].vertices ==
                      reference.device.draws[draw].vertices);
                CHECK(corrected.device.draws[draw].transform ==
                      reference.device.draws[draw].transform);
                CHECK(corrected.device.draws[draw].state == reference.device.draws[draw].state);
            }
        }
        tick += distance;
    }
    CHECK(corrected.arena.phase() == 3);
    CHECK_FALSE(corrected.arena.visible(0));
    CHECK_FALSE(corrected.arena.visible(1));
    CHECK_FALSE(corrected.arena.visible(2));
    CHECK(corrected.device.texturesCreated == uploads);
    CHECK(corrected.device.textureUpdates == updates);
    SkorneArena::LocalSnapshot saved;
    corrected.arena.saveLocal(saved);
    CHECK_FALSE(reference.arena.restoreLocal(saved));
    CHECK_FALSE(reference.arena.equalLocal(saved, saved));
    corrected.arena.clear();
    CHECK_FALSE(corrected.arena.restoreLocal(saved));
    CHECK_FALSE(corrected.arena.equalLocal(saved, saved));
    corrected.arena.bind(corrected.device, corrected.layout, corrected.models, corrected.textures);
    CHECK_FALSE(corrected.arena.restoreLocal(saved));
}

TEST_CASE("native altar equality retains lift randomness and subframe clocks before movement",
          "[local-rewind][skorne-arena-local-rewind]") {
    RewindFixture fixture("skorne-native-hidden-values");
    SkorneArena::LocalSnapshot before;
    SkorneArena::LocalSnapshot lifted;
    SkorneArena::LocalSnapshot fractional;
    SkorneArena::LocalSnapshot restored;
    fixture.arena.saveLocal(before);
    const auto position = fixture.arena.position(0);
    fixture.arena.cue({0, 0, 0});
    fixture.arena.saveLocal(lifted);
    CHECK(fixture.arena.position(0) == position);
    CHECK_FALSE(fixture.arena.equalLocal(before, lifted));
    fixture.arena.update(1.0f / 240);
    fixture.arena.saveLocal(fractional);
    CHECK(fixture.arena.position(0) == position);
    CHECK_FALSE(fixture.arena.equalLocal(lifted, fractional));
    REQUIRE(fixture.arena.restoreLocal(before));
    fixture.arena.cue({0, 0, 0});
    fixture.arena.update(1.0f / 240);
    fixture.arena.saveLocal(restored);
    CHECK(fixture.arena.equalLocal(fractional, restored));
}
} // namespace
