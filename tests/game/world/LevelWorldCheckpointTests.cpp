#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/LevelWorldCheckpoint.h"

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
            textures.push_back(std::make_unique<test::FakeTexture>(2, 2));
        }
        std::ranges::sort(textures, [](const auto& a, const auto& b) {
            return std::less<const Texture*>{}(a.get(), b.get());
        });
        if (reverse) {
            std::ranges::reverse(textures);
        }
    }
    bool beginFrame() override { return true; }
    void endFrame() override {}
    void setClearColor(const Vec4&) override {}
    Extent2D framebufferExtent() const override { return {640, 448}; }
    void setPresentation(bool, u32) override {}
    u32 presentationSampleCount() const override { return 1; }
    void setTextureFiltering(u32) override {}
    void setSmoothSprites(bool) override {}
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc,
                                           std::span<const u8> pixels) override {
        REQUIRE(desc.width == 2);
        REQUIRE(desc.height == 2);
        REQUIRE(next < textures.size());
        textures[next]->pixels.assign(pixels.begin(), pixels.end());
        return std::move(textures[next++]);
    }
    void updateTexture(Texture&, std::span<const u8>) override { FAIL("unexpected upload"); }
    const Texture& whiteTexture() const override { return white; }
    void draw(const ImmediateBatch&, const Texture&, const Mat4&, const DrawState&) override {}
    void waitIdle() override {}
    bool applyDepthOfField(const DepthOfField&) override { return false; }
    bool applyBloom() override { return false; }
    void addHeatSource(const HeatSource&) override {}
    bool applyAmbientOcclusion(const AmbientOcclusion&) override { return false; }

private:
    test::FakeTexture white{1, 1};
    std::vector<std::unique_ptr<test::FakeTexture>> textures;
    usize next = 0;
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

TEST_CASE("complete level checkpoints rewind native motion destruction pickups and cue queues",
          "[rollback][level-world-checkpoint]") {
    Fixture host("level-world-checkpoint-host");
    Fixture replay("level-world-checkpoint-replay");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto textures = replay.device.texturesCreated;
    const auto uploads = replay.device.textureUpdates;
    const auto* wall = &host.world.walls().wall(0);
    const auto* rotator = &host.world.rotators().rotator(0);
    bool particleBirth = false, burst = false, cartExplosion = false, wallBroken = false;
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
    }
    CAPTURE(mutation);
    CHECK_FALSE(fixture.owner.prepareRestore(invalid));
    CHECK_FALSE(fixture.owner.restore(invalid));
    CHECK(fixture.state() == before);
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
    TextureOrderDevice ascending(false), descending(true);
    TextureSet hostTextures, replayTextures;
    REQUIRE(hostTextures.load(root));
    REQUIRE(replayTextures.load(root));
    LevelWorld host, replay;
    const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
    REQUIRE(host.load(loadDevice, root, level));
    REQUIRE(replay.load(loadDevice, root, level));
    // Rebind the native particle owners against independent, immutable catalogs.
    // These catalogs and their devices outlive both worlds and every preparation.
    const_cast<ParticleField&>(host.particles()).bind(host.layout(), hostTextures, ascending);
    const_cast<ParticleField&>(replay.particles())
        .bind(replay.layout(), replayTextures, descending);
    REQUIRE(host.particles().size() == 2);
    REQUIRE(replay.particles().size() == 2);
    REQUIRE(host.particles().sourceObject(0) == 0);
    REQUIRE(replay.particles().sourceObject(0) == 1);
    LevelWorldCheckpoint authority(host), replica(replay);
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
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture.owner.prepareRestore(invalid));
        CHECK(fixture.state() == before);
    }

    // Fault injection into a live child must still fail its capture before the
    // parent's capture-only cross-link checks can accept anything.
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
    LevelWorld host, restored;
    REQUIRE(host.load(device, root, *level));
    REQUIRE(restored.load(device, root, *level));
    host.setPlayerCount(1);
    restored.setPlayerCount(1);
    LevelWorldCheckpoint authority{host}, replay{restored};
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
    for (s32 tick = 0; tick < 12; ++tick) {
        for (auto* world : {&host, &restored}) {
            world->capturePresentation();
            world->update(1.0f / 60, tick % 3 == 0);
            world->updateTriggers(1.0f / 60, {});
        }
        CHECK(replay.checkpoint() == authority.checkpoint());
    }
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == uploads);
}
} // namespace
