#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/level/LevelWorld.h"
#include "game/level/WorldDestructionCheckpoint.h"
#include "game/rollback/LocalStateHistory.h"

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
    Fixture original;
    Fixture restored;
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
        case 4: invalid.explosions.emplace_back(std::numeric_limits<f32>::quiet_NaN(), 0, 0); break;
        case 5: invalid.destroyed[1] = true; break;
        default: break;
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
    const Fixture other;
    auto foreign = other.owner.prepareRestore(*state);
    REQUIRE(foreign);
    CHECK_FALSE(fixture.owner.commitRestore(std::move(*foreign)));
    CHECK(other.owner.canCommit(*foreign));
}

TEST_CASE("native destruction rosters restore between independently loaded stage owners",
          "[rollback][world-destruction-checkpoint][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto* const stage = GENERATE("G2", "D1", "J4");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(stage);
    REQUIRE(level);
    LevelWorld first;
    LevelWorld second;
    REQUIRE(first.load(device, root, *level));
    REQUIRE(second.load(device, root, *level));
    WorldDestruction firstLedger;
    WorldDestruction secondLedger;
    firstLedger.bind(first.layout());
    secondLedger.bind(second.layout());
    const WorldDestructionCheckpoint source{firstLedger};
    WorldDestructionCheckpoint target{secondLedger};
    const auto state = source.checkpoint();
    REQUIRE(state);
    REQUIRE(target.restore(*state));
    CHECK(target.checkpoint() == state);
    CHECK(state->objects.size() == first.layout().objects().size());
}
TEST_CASE("native destruction history rewinds pending events without emitting collaborator work",
          "[local-rewind][world-destruction-local-rewind]") {
    Fixture reference;
    Fixture corrected;
    struct Snapshot {
        WorldDestruction::LocalSnapshot destruction;
        WorldAnimator::LocalSnapshot animator;
        WorldScene::LocalSnapshot scene;
        WorldCollision::LocalSnapshot collision;
    };
    LocalStateHistory<Snapshot> history;
    std::array<WorldDestruction::LocalSnapshot, 8> expected;
    WorldDestruction::LocalSnapshot observed;
    u64 tick = 0;
    for (u64 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        REQUIRE(history.capture(tick, [&](auto& saved) {
            corrected.destruction.saveLocal(saved.destruction);
            corrected.animator.saveLocal(saved.animator);
            corrected.scene.saveLocal(saved.scene);
            corrected.collision.saveLocal(saved.collision);
            return true;
        }));
        const auto before = corrected.owner.checkpoint();
        REQUIRE(before);
        auto stale = corrected.owner.prepareRestore(*before);
        REQUIRE(stale);
        const u64 distance = 1 + window % 8;
        const auto* saved = history.find(tick);
        REQUIRE(saved);
        for (u64 offset = 0; offset < distance; ++offset) {
            corrected.step(1.0f / 60, (tick + offset) % 11 == 0);
            corrected.destruction.saveLocal(expected[offset]);
        }
        REQUIRE(corrected.destruction.restoreLocal(saved->destruction));
        REQUIRE(corrected.animator.restoreLocal(saved->animator));
        REQUIRE(corrected.scene.restoreLocal(saved->scene));
        REQUIRE(corrected.collision.restoreLocal(saved->collision));
        for (u64 offset = 0; offset < distance; ++offset) {
            reference.step(1.0f / 60, (tick + offset) % 11 == 0);
            corrected.step(0.13f);
            corrected.destruction.explode(0, {30, 4, 10}, corrected.scene, corrected.collision);
        }
        const bool visible = corrected.scene.objectVisible(0);
        const bool solid = corrected.collision.solid(0);
        REQUIRE(corrected.destruction.restoreLocal(saved->destruction));
        CHECK(corrected.scene.objectVisible(0) == visible);
        CHECK(corrected.collision.solid(0) == solid);
        CHECK(corrected.owner.checkpoint() == before);
        CHECK_FALSE(corrected.owner.canCommit(*stale));
        REQUIRE(corrected.animator.restoreLocal(saved->animator));
        REQUIRE(corrected.scene.restoreLocal(saved->scene));
        REQUIRE(corrected.collision.restoreLocal(saved->collision));
        for (u64 offset = 0; offset < distance; ++offset) {
            corrected.step(1.0f / 60, (tick + offset) % 11 == 0);
            corrected.destruction.saveLocal(observed);
            CHECK(corrected.destruction.equalLocal(observed, expected[offset]));
        }
        CHECK(corrected.owner.checkpoint() == reference.owner.checkpoint());
        CHECK(corrected.scene.objectVisible(0) == reference.scene.objectVisible(0));
        CHECK(corrected.scene.worldTransform(0) == reference.scene.worldTransform(0));
        CHECK(corrected.collision.solid(0) == reference.collision.solid(0));
        CHECK(corrected.animator.checkpoint() == reference.animator.checkpoint());
        if (window % 2 == 0) {
            CHECK(corrected.destruction.takeExplosions() == reference.destruction.takeExplosions());
        }
        tick += distance;
    }
    WorldDestruction::LocalSnapshot saved;
    corrected.destruction.saveLocal(saved);
    CHECK_FALSE(reference.destruction.restoreLocal(saved));
    CHECK_FALSE(reference.destruction.equalLocal(saved, saved));
    corrected.destruction.clear();
    CHECK_FALSE(corrected.destruction.restoreLocal(saved));
    CHECK_FALSE(corrected.destruction.equalLocal(saved, saved));
    corrected.destruction.bind(corrected.layout);
    CHECK_FALSE(corrected.destruction.restoreLocal(saved));
}
TEST_CASE("native destruction equality includes undelivered explosions with identical geometry",
          "[local-rewind][world-destruction-local-rewind]") {
    Fixture fixture;
    REQUIRE(fixture.destruction.explode(0, {10, 2, 3}, fixture.scene, fixture.collision));
    WorldDestruction::LocalSnapshot queued;
    WorldDestruction::LocalSnapshot drained;
    WorldDestruction::LocalSnapshot restored;
    fixture.destruction.saveLocal(queued);
    const auto visible = fixture.scene.objectVisible(0);
    const auto solid = fixture.collision.solid(0);
    const auto explosions = fixture.destruction.takeExplosions();
    REQUIRE_FALSE(explosions.empty());
    fixture.destruction.saveLocal(drained);
    CHECK(fixture.scene.objectVisible(0) == visible);
    CHECK(fixture.collision.solid(0) == solid);
    CHECK_FALSE(fixture.destruction.equalLocal(queued, drained));
    REQUIRE(fixture.destruction.restoreLocal(queued));
    fixture.destruction.saveLocal(restored);
    CHECK(fixture.destruction.equalLocal(queued, restored));
    CHECK(fixture.destruction.takeExplosions() == explosions);
}
} // namespace
