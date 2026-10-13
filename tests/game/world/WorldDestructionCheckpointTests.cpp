#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/LevelWorld.h"
#include "game/world/WorldDestructionCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    WorldScene scene;
    WorldCollision collision;
    WorldAnimator animator;
    WorldDestruction destruction;
    WorldDestructionCheckpoint owner{destruction};

    Fixture() {
        const auto directory = test::sampleLevel("world-destruction-checkpoint");
        writeTextFile(directory / "world.json", R"({"objects":[
          {"name":"WALL","flags":327680,"position":[10,0,0],"next":1},
          {"name":"WINDOW","position":[0,0,0]}],"animations":[{"object":0,"frames":4,
          "track":{"flags":16,"frames":[0,3],"values":[0,6]}}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(models.load(directory));
        REQUIRE(textures.load(directory));
        destruction.bind(layout);
        REQUIRE(
            scene.build(layout, models, textures, device, {}, {}, destruction.controlledObjects()));
        animator.bind(layout);
    }
    void step(f32 seconds, bool held = false) {
        animator.step(seconds, scene, held);
        destruction.update(animator.cycleEvents(), scene, collision);
    }
};

TEST_CASE("destruction checkpoints rewind pending cart blasts without dispatching world work",
          "[rollback][world-destruction-checkpoint]") {
    const auto rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    Fixture original, restored;
    for (int tick = 0; tick < rate / 10; ++tick) {
        original.step(seconds);
    }
    const auto state = original.owner.checkpoint();
    REQUIRE(state);
    REQUIRE_FALSE(state->explosions.empty());
    REQUIRE(state->destroyed[0]);
    const auto animations = original.animator.checkpoint();
    const auto geometry = original.scene.geometry();
    const auto textures = restored.device.texturesCreated;
    const auto uploads = restored.device.textureUpdates;
    REQUIRE(restored.owner.restore(*state));
    CHECK(restored.scene.objectVisible(0));
    CHECK(restored.collision.solid(0));
    // These collaborators deliberately remain external to the ledger. Restore
    // their matching boundary explicitly before comparing a subsequent phase.
    REQUIRE(restored.animator.restore(animations));
    REQUIRE(restored.scene.applyGeometry(geometry));
    restored.collision.setSolid(0, original.collision.solid(0));
    CHECK(restored.owner.checkpoint() == state);
    CHECK(restored.destruction.takeExplosions() == original.destruction.takeExplosions());
    for (int tick = 0; tick < 24; ++tick) {
        const bool held = tick >= 8 && tick < 12;
        original.step(seconds, held);
        restored.step(seconds, held);
        CHECK(restored.owner.checkpoint() == original.owner.checkpoint());
        CHECK(restored.scene.objectVisible(0) == original.scene.objectVisible(0));
        CHECK(restored.collision.solid(0) == original.collision.solid(0));
        CHECK(restored.animator.checkpoint() == original.animator.checkpoint());
    }
    CHECK(restored.device.texturesCreated == textures);
    CHECK(restored.device.textureUpdates == uploads);
}

TEST_CASE("destruction restore refuses malformed rosters and stale sibling plans atomically",
          "[rollback][world-destruction-checkpoint]") {
    Fixture fixture;
    const auto state = fixture.owner.checkpoint();
    REQUIRE(state);
    for (const int mutation : {0, 1, 2, 3, 4, 5}) {
        auto invalid = *state;
        switch (mutation) {
        case 0: invalid.destroyed.pop_back(); break;
        case 1: invalid.objects[0].name += "different"; break;
        case 2: invalid.objects[0].parent = 500; break;
        case 3: invalid.controlled.push_back(0); break;
        case 4: invalid.explosions.push_back({std::numeric_limits<f32>::quiet_NaN(), 0, 0}); break;
        case 5: invalid.destroyed[1] = true; break;
        }
        CHECK_FALSE(fixture.owner.restore(invalid));
        CHECK(fixture.owner.checkpoint() == state);
    }
    auto rewind = fixture.owner.prepareRestore(*state);
    REQUIRE(rewind);
    REQUIRE(fixture.destruction.explode(0, {10, 0, 1}, fixture.scene, fixture.collision));
    CHECK(fixture.owner.canCommit(*rewind));
    REQUIRE(fixture.owner.commitRestore(std::move(*rewind)));
    CHECK(fixture.owner.checkpoint() == state);
    CHECK_FALSE(fixture.scene.objectVisible(0));
    CHECK_FALSE(fixture.collision.solid(0));
    CHECK_FALSE(fixture.owner.commitRestore(std::move(*rewind)));
    auto stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    WorldDestructionCheckpoint sibling{fixture.destruction};
    REQUIRE(sibling.restore(*state));
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    fixture.destruction.bind(fixture.layout);
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    Fixture other;
    auto foreign = other.owner.prepareRestore(*state);
    REQUIRE(foreign);
    CHECK_FALSE(fixture.owner.commitRestore(std::move(*foreign)));
    CHECK(other.owner.canCommit(*foreign));
}

TEST_CASE("native destruction rosters restore between independently loaded stage owners",
          "[rollback][world-destruction-checkpoint][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto stage = GENERATE("G2", "D1", "J4");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(stage);
    REQUIRE(level);
    LevelWorld first, second;
    REQUIRE(first.load(device, root, *level));
    REQUIRE(second.load(device, root, *level));
    WorldDestruction firstLedger, secondLedger;
    firstLedger.bind(first.layout());
    secondLedger.bind(second.layout());
    WorldDestructionCheckpoint source{firstLedger}, target{secondLedger};
    const auto state = source.checkpoint();
    REQUIRE(state);
    REQUIRE(target.restore(*state));
    CHECK(target.checkpoint() == state);
    CHECK(state->objects.size() == first.layout().objects().size());
}
} // namespace
