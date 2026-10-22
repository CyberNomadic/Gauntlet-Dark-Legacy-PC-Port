#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "game/level/LevelWorldCheckpoint.h"
#include "game/rollback/NativeProbeTiming.h"

namespace {
using namespace gdl;
using namespace gdl::game;
static_assert(std::is_aggregate_v<LevelWorldCheckpoint::State>);

// Supplies the same two native textures in deliberately opposite address order.
// The real ParticleField batching sort still runs; no checkpoint row is shuffled
// by the test to manufacture the cross-owner ordering difference.
class TextureOrderDevice final : public RenderDevice {
public:
    explicit TextureOrderDevice(bool reverse) {
        for (s32 i = 0; i < 2; ++i) {
            m_textures.push_back(std::make_unique<test::FakeTexture>(2, 2));
        }
        std::ranges::sort(m_textures, [](const auto& a, const auto& b) {
            return std::less<const Texture*>{}(a.get(), b.get());
        });
        if (reverse) {
            std::ranges::reverse(m_textures);
        }
    }
    bool beginFrame() override { return true; }
    void endFrame() override {}
    void setClearColor(const Vec4& /*rgba*/) override {}
    Extent2D framebufferExtent() const override { return {640, 448}; }
    void setPresentation(bool /*vsync*/, u32 /*sampleCount*/) override {}
    u32 presentationSampleCount() const override { return 1; }
    void setTextureFiltering(u32 /*filtering*/) override {}
    void setSmoothSprites(bool /*enabled*/) override {}
    void setAuthoredSilhouettes(bool /*enabled*/) override {}
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc,
                                           std::span<const u8> pixels) override {
        REQUIRE(desc.width == 2);
        REQUIRE(desc.height == 2);
        REQUIRE(m_next < m_textures.size());
        m_textures[m_next]->pixels.assign(pixels.begin(), pixels.end());
        return std::move(m_textures[m_next++]);
    }
    void updateTexture(Texture& /*texture*/, std::span<const u8> /*rgba8Pixels*/) override {
        FAIL("unexpected upload");
    }
    const Texture& whiteTexture() const override { return m_white; }
    void draw(const ImmediateBatch& /*batch*/, const Texture& /*texture*/,
              const Mat4& /*transform*/, const DrawState& /*state*/) override {}
    void waitIdle() override {}
    bool applyDepthOfField(const DepthOfField& /*settings*/) override { return false; }
    bool applyBloom() override { return false; }
    void addHeatSource(const HeatSource& /*source*/) override {}
    bool applyAmbientOcclusion(const AmbientOcclusion& /*settings*/) override { return false; }

private:
    test::FakeTexture m_white{1, 1};
    std::vector<std::unique_ptr<test::FakeTexture>> m_textures;
    usize m_next = 0;
};

struct Fixture {
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelWorldCheckpoint owner{world};
    explicit Fixture(std::string_view tag) {
        const auto root = test::sampleLevel(tag);
        writeTextFile(root / "animations.json", R"({
          "particles":[{"preset":2,"enables":16929,"texture":"STONE",
            "particleLife":[0.2,0],"rate":[30,30,30,30]}],
          "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":2}],
          "trees":[
           {"name":"COIN","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
           {"name":"GEMORANGE","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
           {"name":"GETGEMORANGE","nodes":[{"name":"SPARK","particle":0,"parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"BURST","frames":6,"frameRate":30}]}]})");
        writeTextFile(root / "world.json", R"({
          "objects":[
            {"name":"FLOOR","position":[0,0,0],"child":1,"next":2,"flags":4100},
            {"name":"WALL","position":[0,3,0],"next":-1},
            {"name":"WINDOW","position":[20,0,0],"next":3},
            {"name":"WALL","position":[40,0,0],"next":4,"flags":327680},
            {"name":"WALL","position":[60,0,0],"next":5,"flags":4096},
            {"name":"L1PSYSE_BRAZIER","position":[70,1,0],"next":-1,"flags":2048}],
          "bounds":{"min":[-20,-10,-20],"max":[100,20,20]},
          "animations":[
            {"object":0,"frames":4,"state":257,"start":0,
             "track":{"flags":32,"frames":[0,3],"values":[0,4]}},
            {"object":3,"frames":4,"state":257,"start":0,
             "track":{"flags":16,"frames":[0,3],"values":[0,2]}}],
          "particles":[{"id":"E","preset":2,"enables":16929,"texture":"STONE",
            "particleLife":[0.2,0],"rate":[30,30,30,30]}],
          "itemInfos":[
            {"type":5,"subtype":24,"name":"PAD","radius":2,"height":1},
            {"type":12,"subtype":0,"name":"SPIN","radius":2,"height":1},
            {"type":10,"subtype":40,"name":"WALL","radius":2,"height":3},
            {"type":10,"subtype":42,"name":"WALL","hitPoints":25,"armor":1,"radius":2,"height":5},
            {"type":1,"subtype":1,"name":"COIN","value":500,"radius":1,"height":2},
            {"type":1,"subtype":15,"name":"GEMORANGE","value":4,"radius":1,"height":2}],
          "itemInstances":[
            {"info":0,"flags":2,"position":[0,0,0],"params":[0,0,2,0,4,255,1,0,0,0,0,0]},
            {"info":0,"flags":2,"position":[20,0,0],"params":[2,0,16,0,4,255,2,0,0,0,0,0]},
            {"info":1,"flags":2,"position":[60,0,0],"params":[4,0,0,0,10,215,35,60,0,0,128,63]},
            {"info":2,"name":"WALL","position":[30,0,0]},
            {"info":3,"name":"WALL","position":[50,0,5],
             "collision":[{"normal":[0,0,1],"vertices":[[-3,0,0],[3,0,0],[0,5,0]]}]},
            {"info":4,"position":[0,0,0]},
            {"info":5,"position":[80,0,0]}]})");
        writeTextFile(root / "collision.json", R"({"objects":[
          {"object":0,"normals":[0,1,0],"vertices":[-3,0,-3,3,0,-3,0,0,3]}]})");
        const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
        REQUIRE(world.load(device, root, level));
        world.setPlayerCount(1);
        REQUIRE(world.worldAnimator().size() == 2);
        REQUIRE(world.rotators().size() == 1);
        REQUIRE(world.fallingScenery().size() == 1);
        REQUIRE(world.walls().size() == 1);
        REQUIRE(world.placedItems().size() == 2);
        REQUIRE(world.particles().size() == 1);
        REQUIRE(owner.bindResources(device));
    }
    LevelWorldCheckpoint::State state() const {
        auto result = owner.checkpoint();
        REQUIRE(result);
        REQUIRE(
            result->valid()); // Independent public recursive validation of every native capture.
        return std::move(*result);
    }
    void step(s32 tick, f32 seconds) {
        world.capturePresentation();
        if (tick == 1) {
            REQUIRE_FALSE(world.fallingScenery().touch({30, 0, 0}, 1).empty());
            REQUIRE(world.throwItem(device, "COIN", {8, 5, 0}, {2, 3, 0}, 0.5f));
        }
        if (tick == 3) {
            REQUIRE(world.strikeWall(0, 10));
        }
        if (tick == 5) {
            REQUIRE(world.collect(device, std::array{Collector{{80, 0, 0}, 1, 2}}).size() == 1);
        }
        if (tick == 14) {
            REQUIRE(world.strikeWall(0, 30) == 0);
        }
        world.update(seconds, tick >= 9 && tick <= 11);
        const std::array visitors{TriggerVisitor{.position = Vec3{0, 0, 0}, .floorObject = 0},
                                  TriggerVisitor{.position = Vec3{20, 0, 0}}};
        world.updateTriggers(seconds, visitors);
        world.setAmbientOffset(-static_cast<f32>(tick % 6) / 10);
        const std::array<PointLight, 1> lights{{{{10, 2, 0}, {1, 0.25f, 0.5f}, 8, 2}}};
        world.setPointLights(lights);
    }
};

TEST_CASE("level support batches retain appended collision owners and reject stale resources",
          "[rollback][level-world-checkpoint]") {
    Fixture fixture("level-world-support-batch");
    const auto objects = fixture.owner.collisionObjects();
    REQUIRE_FALSE(objects.empty());
    CHECK(std::ranges::is_sorted(objects));
    CHECK(std::ranges::adjacent_find(objects) == objects.end());
    CHECK(std::ranges::binary_search(objects, 0));
    const auto wallObject = fixture.world.walls().wall(0).object;
    REQUIRE(wallObject >= static_cast<s32>(fixture.world.layout().objects().size()));
    CHECK(std::ranges::binary_search(objects, wallObject));
    for (const auto object : objects) {
        CHECK(fixture.owner.hasCollisionObject(object));
    }
    CHECK_FALSE(std::ranges::binary_search(objects, -1));
    CHECK_FALSE(std::ranges::binary_search(objects, 999999));
    const std::vector<s32> identities(objects.begin(), objects.end());
    REQUIRE(fixture.world.strikeWall(0, 1000) == 0);
    const auto after = fixture.owner.collisionObjects();
    CHECK(std::ranges::equal(after, identities));
    CHECK_FALSE(fixture.world.collision().solid(wallObject));
    fixture.owner.clearResources();
    CHECK(fixture.owner.collisionObjects().empty());
    CHECK_FALSE(fixture.owner.hasCollisionObject(wallObject));
    REQUIRE(fixture.owner.bindResources(fixture.device));
    CHECK(std::ranges::equal(fixture.owner.collisionObjects(), identities));
    fixture.world.clear();
    CHECK(fixture.owner.collisionObjects().empty());
    CHECK_FALSE(fixture.owner.hasCollisionObject(0));
}

TEST_CASE("native whole world history restores coupled platforms items destruction and triggers",
          "[local-rewind][level-world-local-rewind]") {
    Fixture native("world-local-native");
    Fixture replay("world-local-replay");
    std::array<LevelWorld::LocalSnapshot, 16> history;
    std::array<LevelWorld::LocalSnapshot, 8> expected;
    LevelWorld::LocalSnapshot observed;
    const auto uploads = replay.device.texturesCreated;
    s32 tick = 0;
    for (u32 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        auto& saved = history[window % history.size()];
        replay.world.saveLocal(saved);
        const u32 distance = window % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            replay.step(tick + static_cast<s32>(i), 1.0f / 60);
            replay.world.saveLocal(expected[i]);
        }
        REQUIRE(replay.world.restoreLocal(saved));
        for (u32 i = 0; i < distance; ++i) {
            replay.world.capturePresentation();
            replay.world.setPlayerCount(4);
            replay.world.activateTrigger(1, true);
            replay.world.strikeWall(0, 9999);
            replay.world.explodeObject(3, Vec3{40, 0, 0});
            replay.world.fallingScenery().touch({30, 0, 0}, 10);
            REQUIRE(replay.world.throwItem(replay.device, "COIN", {3, 10, 4}, {4, 2, 1}, 0.1f));
            replay.world.collect(replay.device, std::array{Collector{{80, 0, 0}, 1, 2}});
            replay.world.setAmbientOffset(-0.9f);
            replay.world.update(1.0f / 30);
            replay.world.updateTriggers(1.0f / 30, {});
            replay.world.takeTriggerOpenings();
            replay.world.takeWorldExplosions();
            replay.world.takeRotatorCues();
        }
        const auto legacyPlan = replay.owner.prepareRestore(replay.state());
        REQUIRE(legacyPlan);
        REQUIRE(replay.world.restoreLocal(saved));
        CHECK_FALSE(replay.owner.canCommit(*legacyPlan));
        CHECK(replay.state() == native.state());
        for (u32 i = 0; i < distance; ++i) {
            native.step(tick, 1.0f / 60);
            replay.step(tick, 1.0f / 60);
            replay.world.saveLocal(observed);
            CHECK(replay.world.equalLocal(observed, expected[i]));
            ++tick;
            CHECK(replay.state() == native.state());
            CHECK(replay.world.collision().objectTransform(0) ==
                  native.world.collision().objectTransform(0));
            CHECK(replay.world.placedItems().item(0).position ==
                  native.world.placedItems().item(0).position);
            for (f32 const alpha : {-1.0f, 0.5f, 1.0f}) {
                for (auto* fixture : {&native, &replay}) {
                    fixture->device.draws.clear();
                    fixture->world.drawOpaque(fixture->device, Mat4{1}, {}, alpha);
                    fixture->world.drawDeferred(fixture->device, Mat4{1}, {}, alpha);
                }
                REQUIRE(replay.device.draws.size() == native.device.draws.size());
                for (usize j = 0; j < native.device.draws.size(); ++j) {
                    test::checkRecordedDraw(replay.device.draws[j], native.device.draws[j]);
                }
            }
        }
    }
    CHECK(replay.device.texturesCreated == uploads);
    CHECK(replay.device.textureUpdates == 0);
    CHECK_FALSE(native.world.restoreLocal(history[0]));
    CHECK_FALSE(native.world.equalLocal(history[0], history[0]));
    replay.world.clear();
    CHECK_FALSE(replay.world.restoreLocal(history[0]));
    CHECK_FALSE(replay.world.equalLocal(history[0], history[0]));
}

TEST_CASE("native world equality includes pending child output and latent root controls",
          "[local-rewind][level-world-local-rewind]") {
    Fixture fixture("world-local-hidden-values");
    LevelWorld::LocalSnapshot before;
    LevelWorld::LocalSnapshot changed;
    LevelWorld::LocalSnapshot restored;
    fixture.world.saveLocal(before);
    const auto position = fixture.world.placedItems().item(0).position;
    fixture.world.setItemOpener(0, 3);
    fixture.world.setAmbientOffset(-0.25f);
    fixture.world.saveLocal(changed);
    CHECK(fixture.world.placedItems().item(0).position == position);
    CHECK_FALSE(fixture.world.equalLocal(before, changed));
    REQUIRE(fixture.world.restoreLocal(before));
    fixture.world.saveLocal(restored);
    CHECK(fixture.world.equalLocal(before, restored));

    REQUIRE(fixture.world.explodeObject(3, {40, 0, 0}));
    fixture.world.saveLocal(before);
    const auto explosions = fixture.world.takeWorldExplosions();
    REQUIRE_FALSE(explosions.empty());
    fixture.world.saveLocal(changed);
    CHECK_FALSE(fixture.world.equalLocal(before, changed));
    REQUIRE(fixture.world.restoreLocal(before));
    fixture.world.saveLocal(restored);
    CHECK(fixture.world.equalLocal(before, restored));
    CHECK(fixture.world.takeWorldExplosions() == explosions);
}

TEST_CASE(
    "loaded native worlds report complete owner history cost without diagnostic serialization",
    "[.][assets][level-world-local-timing]") {
    const auto* const name = GENERATE("G2", "G4", "L1", "D2");
    const auto root = test::assetOrSkip(std::string{"LEVELS/LEVEL"} + name + "/WORLDS.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(name);
    REQUIRE(level);
    test::FakeRenderDevice device;
    device.recordDraws = false;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    world.setPlayerCount(4);
    if (world.skorneArena().size() > 0) {
        world.bossArenaCue({0, 0, 0});
    }
    std::vector<TriggerVisitor> visitors(4);
    if (const auto* start = world.startPoint(0)) {
        for (usize i = 0; i < visitors.size(); ++i) {
            visitors[i].position = start->position + Vec3{static_cast<f32>(i), 0, 0};
        }
    }
    const auto step = [&] {
        world.capturePresentation();
        world.update(1.0f / 60);
        world.updateTriggers(1.0f / 60, visitors);
        world.takeRotatorCues();
        world.takeTriggerCameraCues();
        world.takeTriggerOpenings();
    };
    for (usize i = 0; i < 60; ++i) {
        step();
    }
    std::array<LevelWorld::LocalSnapshot, 16> history;
    for (auto& saved : history) {
        world.saveLocal(saved);
    }
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](auto start) {
        return std::chrono::duration<f64, std::milli>(Clock::now() - start).count();
    };
    std::array<f64, 256> saves{};
    std::array<f64, 256> restores{};
    std::array<f64, 256> replays{};
    for (usize i = 0; i < saves.size(); ++i) {
        auto& saved = history[i % history.size()];
        auto start = Clock::now();
        world.saveLocal(saved);
        saves[i] = elapsed(start);
        for (usize j = 0; j < 8; ++j) {
            step();
        }
        start = Clock::now();
        const bool restored = world.restoreLocal(saved);
        restores[i] = elapsed(start);
        REQUIRE(restored);
        start = Clock::now();
        for (usize j = 0; j < 8; ++j) {
            step();
        }
        replays[i] = elapsed(start);
    }
    const auto report = [&](std::string_view operation, const auto& samples) {
        const auto timing = test::summarizeNativeTimings(samples);
        REQUIRE(timing);
        std::cout << name << "_level_world_" << operation << " mean_ms=" << timing->mean
                  << " p99_ms=" << timing->p99 << " max_ms=" << timing->maximum << '\n';
    };
    report("save", saves);
    report("restore", restores);
    report("replay8", replays);
}

TEST_CASE("complete level checkpoints rewind native motion destruction pickups and cue queues",
          "[rollback][level-world-checkpoint]") {
    Fixture host("level-world-checkpoint-host");
    Fixture replay("level-world-checkpoint-replay");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto textures = replay.device.texturesCreated;
    const auto uploads = replay.device.textureUpdates;
    const auto* wall = &host.world.walls().wall(0);
    const auto* rotator = &host.world.rotators().rotator(0);
    bool particleBirth = false;
    bool burst = false;
    bool cartExplosion = false;
    bool wallBroken = false;
    bool carriedPickup = false;
    for (s32 tick = 0; tick < 50; ++tick) {
        CAPTURE(tick, seconds);
        const auto before = host.state();
        REQUIRE(before.pickups.items.front().floor);
        CHECK(before.pickups.items.front().floor->object == 0);
        const auto floorBefore = host.world.collision().objectTransform(0);
        const bool wallSolidBefore = host.world.collision().solid(wall->object);
        REQUIRE(replay.owner.restore(before));
        auto undo = host.owner.prepareRestore(before);
        REQUIRE(undo);
        host.step(tick, seconds);
        replay.step(tick, seconds);
        const auto expected = host.state();
        CHECK(replay.state() == expected);
        particleBirth |= host.world.particles().particleCount() > 0;
        burst |= !expected.pickups.effects.empty();
        cartExplosion |= !expected.destruction.explosions.empty();
        wallBroken |= expected.walls.walls[0].health == 0;
        carriedPickup |= expected.pickups.items.front().position.y > PlacedItems::kFloorLift;
        if (expected.walls.walls[0].health == 0) {
            CHECK_FALSE(host.world.collision().solid(wall->object));
            CHECK_FALSE(replay.world.collision().solid(wall->object));
        }
        REQUIRE(host.owner.commitRestore(std::move(*undo)));
        CHECK(host.state() == before);
        CHECK(host.world.collision().solid(wall->object) == wallSolidBefore);
        CHECK(host.world.collision().objectTransform(0) == floorBefore);
        CHECK(&host.world.walls().wall(0) == wall);
        CHECK(&host.world.rotators().rotator(0) == rotator);
        host.step(tick, seconds);
        CHECK(host.state() == expected);
        CHECK(host.world.takeWorldExplosions() == replay.world.takeWorldExplosions());
        CHECK(host.world.takeTriggerOpenings() == replay.world.takeTriggerOpenings());
        CHECK(host.world.takeTriggerSettled() == replay.world.takeTriggerSettled());
        CHECK(host.world.takeTriggerCameraCues() == replay.world.takeTriggerCameraCues());
        const auto a = host.world.takeRotatorCues();
        const auto b = replay.world.takeRotatorCues();
        REQUIRE(a.size() == b.size());
        for (usize i = 0; i < a.size(); ++i) {
            CHECK(a[i].kind == b[i].kind);
            CHECK(a[i].rotator == b[i].rotator);
            CHECK(a[i].position == b[i].position);
        }
    }
    CHECK(particleBirth);
    CHECK(burst);
    CHECK(cartExplosion);
    CHECK(wallBroken);
    CHECK(carriedPickup);
    CHECK(replay.device.texturesCreated == textures);
    CHECK(replay.device.textureUpdates == uploads);
}

TEST_CASE("complete level preparation is atomic across malformed child owners",
          "[rollback][level-world-checkpoint]") {
    Fixture fixture("level-world-checkpoint-invalid");
    fixture.step(0, 1.0f / 60);
    const auto before = fixture.state();
    auto invalid = before;
    const s32 mutation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13);
    switch (mutation) {
    case 0: invalid.core.moving.front().object = 9999; break;
    case 1: invalid.triggers.triggers.front().target = 300; break;
    case 2: invalid.destruction.objects.pop_back(); break;
    case 3: invalid.falling.pieces.front().gravityStep += 1; break;
    case 4: invalid.walls.walls.front().armor += 1; break;
    case 5: invalid.pickups.items.front().resource = 9999; break;
    case 6: invalid.rotators.rotators.front().speed += 1; break;
    case 7: invalid.textures.remainder = 1; break;
    case 8: invalid.particles.entries.front().emitter.descriptor.texture = "FOREIGN"; break;
    case 9: invalid.cues.push_back({RotatorCue::Kind::Turning, 99, Vec3{0}}); break;
    case 10: invalid.frameRemainder = std::numeric_limits<f32>::quiet_NaN(); break;
    case 11: invalid.playerCount = 2; break;
    case 12: invalid.particleSources.clear(); break;
    case 13: invalid.particleSources.front() = 0; break;
    default: break;
    }
    CAPTURE(mutation);
    CHECK_FALSE(fixture.owner.prepareRestore(invalid));
    CHECK_FALSE(fixture.owner.restore(invalid));
    CHECK(fixture.state() == before);
}

TEST_CASE("world scratch capture retains pickup catalog storage and every native postcondition",
          "[rollback][level-world-checkpoint][world-capture-reuse]") {
    Fixture fixture("level-world-capture-reuse");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto baseline = fixture.state();
    auto undo = fixture.owner.prepareRestore(baseline);
    REQUIRE(undo);
    auto scratch = baseline;
    const auto* definitions = scratch.pickups.definitions.data();
    const auto* placements = scratch.pickups.placements.data();
    REQUIRE_FALSE(scratch.pickups.placements[4].collision.empty());
    const auto* collision = scratch.pickups.placements[4].collision.data();
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    for (s32 tick = 0; tick < 30; ++tick) {
        CAPTURE(tick, seconds);
        fixture.step(tick, seconds);
        const auto expected = fixture.state();
        LevelWorldCheckpoint::CaptureTimings timings;
        auto refreshed = fixture.owner.checkpoint(std::move(scratch), &timings);
        REQUIRE(refreshed);
        CHECK(refreshed->valid());
        CHECK(*refreshed == expected);
        CHECK(refreshed->pickups.definitions.data() == definitions);
        CHECK(refreshed->pickups.placements.data() == placements);
        CHECK(refreshed->pickups.placements[4].collision.data() == collision);
        CHECK(std::ranges::all_of(timings, [](const auto& value) { return value.has_value(); }));
        scratch = std::move(*refreshed);
    }
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
    REQUIRE(fixture.owner.canCommit(*undo));
    REQUIRE(fixture.owner.commitRestore(std::move(*undo)));
    CHECK(fixture.state() == baseline);
}

TEST_CASE("world scratch recaptures live fields while rejecting catalog and native corruption",
          "[rollback][level-world-checkpoint][world-capture-reuse]") {
    Fixture fixture("level-world-capture-reuse-validation");
    const auto baseline = fixture.state();
    const auto nan = std::numeric_limits<f32>::quiet_NaN();
    auto scratch = baseline;
    scratch.core.placements.front().local[0][0] = nan;
    scratch.falling.remainder = nan;
    scratch.walls.walls.front().flash = nan;
    scratch.pickups.items.front().position.x = nan;
    scratch.rotators.rotators.front().cursor.turned = nan;
    scratch.particles.entries.front().emitter.saved = nan;
    scratch.textures.remainder = nan;
    scratch.frameRemainder = nan;
    scratch.ambientOffset = nan;
    scratch.playerCount = 4;
    CHECK_FALSE(scratch.valid());
    CHECK_FALSE(fixture.owner.prepareRestore(scratch));
    auto refreshed = fixture.owner.checkpoint(std::move(scratch));
    REQUIRE(refreshed);
    CHECK(*refreshed == baseline);

    for (s32 mutation = 0; mutation < 9; ++mutation) {
        CAPTURE(mutation);
        scratch = baseline;
        switch (mutation) {
        case 0: scratch.level.name += "_FOREIGN"; break;
        case 1: scratch.core.layout ^= 1; break;
        case 2: scratch.core.catalog[0] ^= 1; break;
        case 3: ++scratch.core.resourceCount; break;
        case 4: scratch.particleSources.clear(); break;
        case 5: ++scratch.pickups.definitions.back().value; break;
        case 6: scratch.pickups.placements[4].collision[0].normal.x = -0.0f; break;
        case 7: ++scratch.pickups.placements[4].params[0]; break;
        case 8: scratch.pickups.placements[4].collision[0].vertices[0].x = nan; break;
        default: break;
        }
        // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
        CHECK_FALSE(fixture.owner.checkpoint(std::move(scratch)));
        CHECK(fixture.state() == baseline);
    }

    auto undo = fixture.owner.prepareRestore(baseline);
    REQUIRE(undo);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& wall = const_cast<DestructibleWalls::Wall&>(fixture.world.walls().wall(0));
    const auto flash = std::exchange(wall.flash, nan);
    scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(fixture.owner.checkpoint(std::move(scratch)));
    wall.flash = flash;
    fixture.world.setAmbientOffset(1'000'001.0f);
    scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(fixture.owner.checkpoint(std::move(scratch)));
    REQUIRE(fixture.owner.canCommit(*undo));
    REQUIRE(fixture.owner.commitRestore(std::move(*undo)));
    CHECK(fixture.state() == baseline);
}

TEST_CASE("world scratch reuse follows current binding and exact value catalog identity",
          "[rollback][level-world-checkpoint][world-capture-reuse]") {
    Fixture first("level-world-capture-reuse-bind-a");
    const Fixture second("level-world-capture-reuse-bind-b");
    const auto baseline = first.state();
    auto scratch = baseline;
    auto independent = second.owner.checkpoint(std::move(scratch));
    REQUIRE(independent);
    CHECK(*independent == second.state());
    first.owner.clearResources();
    scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(first.owner.checkpoint(std::move(scratch)));
    REQUIRE(first.owner.bindResources(first.device));
    scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK(first.owner.checkpoint(std::move(scratch)) == std::optional{baseline});
    first.world.clear();
    scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(first.owner.checkpoint(std::move(scratch)));
    CHECK(second.state() == baseline);
}

TEST_CASE("world capture timings observe the same complete native checkpoint",
          "[rollback][level-world-checkpoint][world-capture-timing]") {
    using Owner = LevelWorldCheckpoint;
    Fixture fixture("level-world-capture-timing");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    CHECK(Owner::kCaptureOwners ==
          std::array<std::string_view, 9>{"core", "triggers", "destruction", "falling", "walls",
                                          "pickups", "rotators", "ambient", "aggregate"});
    for (s32 tick = 0; tick < 8; ++tick) {
        CAPTURE(tick, seconds);
        fixture.step(tick, seconds);
        const auto textures = fixture.device.texturesCreated;
        const auto uploads = fixture.device.textureUpdates;
        const auto expected = fixture.state();
        Owner::CaptureTimings timings;
        const auto measured = fixture.owner.checkpoint(&timings);
        REQUIRE(measured);
        CHECK(measured->valid());
        CHECK(*measured == expected);
        CHECK(fixture.state() == expected);
        for (const auto& duration : timings) {
            CHECK(duration.has_value()); // A valid zero-duration clock sample is still present.
        }
        CHECK(fixture.device.texturesCreated == textures);
        CHECK(fixture.device.textureUpdates == uploads);

        auto invalid = *measured;
        invalid.core.placements.front().local[0][0] = std::numeric_limits<f32>::quiet_NaN();
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture.owner.prepareRestore(invalid));
        CHECK(fixture.owner.checkpoint(&timings) == measured);
    }
}

TEST_CASE("world capture failure timings reset and retain only reached operations",
          "[rollback][level-world-checkpoint][world-capture-timing]") {
    using Owner = LevelWorldCheckpoint;
    Fixture fixture("level-world-capture-failure-timing");
    const auto before = fixture.state();
    Owner::CaptureTimings timings;
    timings.fill(std::numeric_limits<u64>::max());

    // All seven child captures run in native order before the existing joint
    // refusal. Ambient capture and aggregate validation must remain unreached.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& wall = const_cast<DestructibleWalls::Wall&>(fixture.world.walls().wall(0));
    const auto flash = std::exchange(wall.flash, std::numeric_limits<f32>::quiet_NaN());
    CHECK_FALSE(fixture.owner.checkpoint());
    CHECK_FALSE(fixture.owner.checkpoint(&timings));
    for (usize owner = 0; owner < timings.size(); ++owner) {
        CAPTURE(owner);
        CHECK(timings[owner].has_value() ==
              (owner < static_cast<usize>(Owner::CaptureOwner::Ambient)));
    }
    wall.flash = flash;
    CHECK(fixture.state() == before);

    // This out-of-envelope offset leaves native child lighting finite through
    // its normal clamp, but the aggregate scalar check must still reject it.
    fixture.world.setAmbientOffset(1'000'001.0f);
    CHECK_FALSE(fixture.owner.checkpoint());
    CHECK_FALSE(fixture.owner.checkpoint(&timings));
    for (const auto& duration : timings) {
        CHECK(duration.has_value());
    }
    REQUIRE(fixture.owner.restore(before));
    CHECK(fixture.state() == before);

    fixture.owner.clearResources();
    CHECK_FALSE(fixture.owner.checkpoint());
    CHECK_FALSE(fixture.owner.checkpoint(&timings));
    for (const auto& duration : timings) {
        CHECK_FALSE(duration.has_value());
    }
    REQUIRE(fixture.owner.bindResources(fixture.device));
    CHECK(fixture.owner.checkpoint(&timings) == std::optional{before});
    for (const auto& duration : timings) {
        CHECK(duration.has_value());
    }
}

TEST_CASE("level particle checkpoints use authored markers across opposite texture ordering",
          "[rollback][level-world-checkpoint][particle-attachment]") {
    const auto root = test::sampleLevel("level-world-particle-order");
    writeTextFile(root / "animations.json", R"({"trees":[]})");
    writeTextFile(root / "world.json", R"({
      "objects":[
        {"name":"PSYSA","position":[1,2,3],"flags":2048,"next":1},
        {"name":"PSYSB","position":[4,5,6],"flags":2048,"next":2},
        {"name":"FLOOR","position":[0,0,0],"flags":4}],
      "particles":[
        {"id":"A","preset":2,"enables":16929,"texture":"STONE",
         "particleLife":[0.2,0],"rate":[30,30,30,30]},
        {"id":"B","preset":2,"enables":16929,"texture":"GLASS",
         "particleLife":[0.3,0],"rate":[30,30,30,30]}]})");
    test::FakeRenderDevice loadDevice;
    TextureOrderDevice ascending(false);
    TextureOrderDevice descending(true);
    TextureSet hostTextures;
    TextureSet replayTextures;
    REQUIRE(hostTextures.load(root));
    REQUIRE(replayTextures.load(root));
    LevelWorld host;
    LevelWorld replay;
    const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
    REQUIRE(host.load(loadDevice, root, level));
    REQUIRE(replay.load(loadDevice, root, level));
    // Rebind the native particle owners against independent, immutable catalogs.
    // These catalogs and their devices outlive both worlds and every preparation.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<ParticleField&>(host.particles()).bind(host.layout(), hostTextures, ascending);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<ParticleField&>(replay.particles())
        .bind(replay.layout(), replayTextures, descending);
    REQUIRE(host.particles().size() == 2);
    REQUIRE(replay.particles().size() == 2);
    REQUIRE(host.particles().sourceObject(0) == 0);
    REQUIRE(replay.particles().sourceObject(0) == 1);
    LevelWorldCheckpoint authority(host);
    LevelWorldCheckpoint replica(replay);
    REQUIRE(authority.bindResources(loadDevice));
    REQUIRE(replica.bindResources(loadDevice));
    for (s32 tick = 0; tick < 11; ++tick) {
        host.update(1.0f / 60);
    }
    REQUIRE(host.particles().particleCount() > 0);
    const auto captured = authority.checkpoint();
    REQUIRE(captured);
    CHECK(captured->particleSources == std::vector<s32>{0, 1});
    REQUIRE(replica.restore(*captured));
    CHECK(replica.checkpoint() == captured);
    // Restore never changes native pointer-sorted rendering/update order.
    CHECK(replay.particles().sourceObject(0) == 1);
    for (s32 tick = 0; tick < 20; ++tick) {
        host.update(1.0f / 60);
        replay.update(1.0f / 60);
        CHECK(replica.checkpoint() == authority.checkpoint());
    }
}

TEST_CASE("world capture validation remains complete for raw values and nested children",
          "[rollback][level-world-checkpoint][checkpoint-capture-validation]") {
    Fixture fixture("level-world-capture-validation");
    const auto before = fixture.state();
    const auto nan = std::numeric_limits<f32>::quiet_NaN();
    for (s32 mutation = 0; mutation < 11; ++mutation) {
        CAPTURE(mutation);
        auto invalid = before;
        switch (mutation) {
        case 0: invalid.core.placements.front().local[0][0] = nan; break;
        case 1: invalid.triggers.parents.front() = 999999; break;
        case 2: invalid.destruction.objects.front().position.x = nan; break;
        case 3: invalid.falling.remainder = nan; break;
        case 4: invalid.walls.walls.front().flash = nan; break;
        case 5: invalid.pickups.items.front().position.x = nan; break;
        case 6: invalid.rotators.rotators.front().speed = nan; break;
        case 7: invalid.textures.remainder = nan; break;
        case 8: invalid.particles.entries.front().emitter.saved = nan; break;
        case 9: invalid.ambientOffset = nan; break;
        case 10: invalid.playerCount = 2; break; // Individually valid children, invalid relation.
        default: break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture.owner.prepareRestore(invalid));
        CHECK(fixture.state() == before);
    }

    // Fault injection into a live child must still fail its capture before the
    // parent's capture-only cross-link checks can accept anything.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& wall = const_cast<DestructibleWalls::Wall&>(fixture.world.walls().wall(0));
    const auto flash = std::exchange(wall.flash, nan);
    CHECK_FALSE(fixture.owner.checkpoint());
    wall.flash = flash;
    CHECK(fixture.state() == before);

    fixture.world.setAmbientOffset(nan); // Raw aggregate state has no optional child validator.
    CHECK_FALSE(fixture.owner.checkpoint());
    REQUIRE(fixture.owner.restore(before));
    CHECK(fixture.state() == before);
    auto stale = fixture.owner.prepareRestore(before);
    REQUIRE(stale);
    fixture.owner.clearResources();
    CHECK_FALSE(fixture.owner.checkpoint());
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    REQUIRE(fixture.owner.bindResources(fixture.device));
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    CHECK(fixture.state() == before);
}

TEST_CASE("complete level plans refuse foreign owners sibling restores and child clears",
          "[rollback][level-world-checkpoint]") {
    Fixture fixture("level-world-checkpoint-fences");
    Fixture other("level-world-checkpoint-other");
    const auto before = fixture.state();
    auto first = fixture.owner.prepareRestore(before);
    auto sibling = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    REQUIRE(sibling);
    CHECK_FALSE(other.owner.commitRestore(std::move(*first)));
    REQUIRE(fixture.owner.canCommit(*first));
    fixture.step(1, 1.0f / 60);
    REQUIRE(fixture.owner.commitRestore(std::move(*first)));
    CHECK(fixture.state() == before);
    CHECK_FALSE(fixture.owner.canCommit(*sibling));
    auto staleChild = fixture.owner.prepareRestore(before);
    REQUIRE(staleChild);
    const auto particles = fixture.world.particles().checkpoint();
    const auto clock = fixture.world.textureAnimator().checkpoint();
    const auto floor = fixture.world.collision().objectTransform(0);
    // Deliberate ownership fault injection: one participating native owner is
    // cleared after preparation. No other participant may be partially restored.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<LevelTriggers&>(fixture.world.triggers()).clear();
    CHECK_FALSE(fixture.owner.canCommit(*staleChild));
    CHECK_FALSE(fixture.owner.commitRestore(std::move(*staleChild)));
    CHECK(fixture.world.particles().checkpoint() == particles);
    CHECK(fixture.world.textureAnimator().checkpoint() == clock);
    CHECK(fixture.world.collision().objectTransform(0) == floor);
    auto staleWorld = other.owner.prepareRestore(other.state());
    REQUIRE(staleWorld);
    other.world.clear();
    CHECK_FALSE(other.owner.canCommit(*staleWorld));
    CHECK_FALSE(other.owner.checkpoint());
}

TEST_CASE("level aggregate cannot silently omit an activated Skorne arena",
          "[rollback][level-world-checkpoint]") {
    Fixture fixture("level-world-checkpoint-skorne");
    const auto before = fixture.state();
    auto prepared = fixture.owner.prepareRestore(before);
    REQUIRE(prepared);
    fixture.world.bossArenaCue({0, 0, 0});
    CHECK(fixture.world.skorneArena().phase() == 1);
    CHECK_FALSE(fixture.owner.checkpoint());
    CHECK_FALSE(fixture.owner.canCommit(*prepared));
    CHECK_FALSE(fixture.owner.restore(before));
    CHECK_FALSE(fixture.owner.bindResources(fixture.device));
}

TEST_CASE("native level checkpoint restores independently loaded world owners without uploads",
          "[rollback][level-world-checkpoint][assets]") {
    const std::string name = GENERATE(std::string{"L1"}, std::string{"G1"});
    CAPTURE(name);
    const auto root = test::assetOrSkip("WDATA/SECRET.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(name);
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld host;
    LevelWorld restored;
    REQUIRE(host.load(device, root, *level));
    REQUIRE(restored.load(device, root, *level));
    host.setPlayerCount(1);
    restored.setPlayerCount(1);
    LevelWorldCheckpoint authority{host};
    LevelWorldCheckpoint replay{restored};
    REQUIRE(authority.bindResources(device));
    REQUIRE(replay.bindResources(device));
    const auto textures = device.texturesCreated;
    const auto uploads = device.textureUpdates;
    for (s32 tick = 0; tick < 12; ++tick) {
        host.capturePresentation();
        host.update(1.0f / 60, tick % 4 == 0);
        host.updateTriggers(1.0f / 60, {});
    }
    const auto captured = authority.checkpoint();
    REQUIRE(captured);
    if (name == "L1") {
        for (const auto object : {1962, 1963}) {
            const auto moving = std::ranges::find(captured->core.moving, object,
                                                  &WorldStateCheckpoint::Moving::object);
            REQUIRE(moving != captured->core.moving.end());
            CHECK(Mat3{moving->transform} == Mat3{0});
        }
    }
    REQUIRE(replay.restore(*captured));
    CHECK(replay.checkpoint() == captured);
    auto scratch = *captured;
    const auto* definitions = scratch.pickups.definitions.data();
    const auto* placements = scratch.pickups.placements.data();
    for (s32 tick = 0; tick < 12; ++tick) {
        for (auto* world : {&host, &restored}) {
            world->capturePresentation();
            world->update(1.0f / 60, tick % 3 == 0);
            world->updateTriggers(1.0f / 60, {});
        }
        const auto fresh = authority.checkpoint();
        auto reused = authority.checkpoint(std::move(scratch));
        REQUIRE(reused);
        CHECK(reused == fresh);
        CHECK(reused->pickups.definitions.data() == definitions);
        CHECK(reused->pickups.placements.data() == placements);
        CHECK(replay.checkpoint() == reused);
        scratch = std::move(*reused);
    }
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == uploads);
}
} // namespace
