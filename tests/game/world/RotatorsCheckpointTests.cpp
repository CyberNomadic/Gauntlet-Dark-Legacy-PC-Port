#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/world/RotatorsCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    ItemArchive items;
    WorldScene scene;
    Rotators rotators;
    RotatorsCheckpoint owner{rotators};
    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "animations.json", R"({"trees":[{"name":"BRIDGEPAD",
          "nodes":[{"name":"PAD","object":"WALL","parent":-1,"position":[0,0,0]}]}]})");
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0],"flags":4096},
            {"name":"WALL","position":[10,0,0],"flags":4096},
            {"name":"WALL","position":[20,0,0],"flags":4096},
            {"name":"WALL","position":[30,0,0],"flags":4096}],
          "itemInfos":[{"type":12,"subtype":0,"radius":2,"height":5},
            {"type":12,"subtype":2,"name":"BRIDGEPAD","radius":2,"height":5}],
          "itemInstances":[
            {"info":0,"position":[0,0,0],"params":[0,0,0,0,10,215,35,60,0,0,128,63]},
            {"info":1,"position":[10,0,0],"params":[1,0,0,0,10,215,35,60,0,0,128,63]},
            {"info":1,"position":[20,0,0],"params":[2,0,0,0,10,215,35,188,0,0,128,63]},
            {"info":1,"position":[30,0,0],"flags":2,
              "params":[3,0,0,0,10,215,35,60,0,0,128,63]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(models.load(directory));
        REQUIRE(textures.load(directory));
        REQUIRE(items.load(directory));
        REQUIRE(scene.build(layout, models, textures, device));
        rotators.bind(layout);
        rotators.bindFigures(device, layout, AssetScope(items));
        REQUIRE(rotators.size() == 4);
        REQUIRE(rotators.rotator(1).pad);
        REQUIRE(rotators.rotator(2).pad);
        REQUIRE_FALSE(rotators.rotator(3).pad);
    }
    RotatorsCheckpoint::State state() const {
        const auto result = owner.checkpoint();
        REQUIRE(result);
        return *result;
    }
};

TEST_CASE("rotator checkpoint refuses malformed rosters and stale preparations atomically",
          "[rotators-checkpoint]") {
    Fixture f("rotators-checkpoint-rejections");
    Fixture other("rotators-checkpoint-other");
    const auto baseline = f.state();
    auto bad = baseline;
    bad.rotators[0].cursor.turned = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(f.owner.restore(bad));
    bad = baseline;
    bad.rotators[1].cursor.done = true;
    CHECK_FALSE(f.owner.restore(bad));
    bad = baseline;
    bad.rotators[1].target.position.x += 1;
    bad.rotators[1].origin.x += 1;
    REQUIRE(bad.valid());
    CHECK_FALSE(f.owner.restore(bad));
    bad = baseline;
    bad.rotators[1].pad.reset();
    REQUIRE(bad.valid());
    CHECK_FALSE(f.owner.restore(bad));
    CHECK(f.state() == baseline);

    auto first = f.owner.prepareRestore(baseline);
    auto sibling = f.owner.prepareRestore(baseline);
    REQUIRE(first);
    REQUIRE(sibling);
    CHECK_FALSE(other.owner.canCommit(*first));
    REQUIRE(f.owner.commitRestore(std::move(*first)));
    CHECK_FALSE(f.owner.commitRestore(std::move(*first)));
    CHECK_FALSE(f.owner.canCommit(*sibling));

    auto beforeCursorRestore = f.owner.prepareRestore(baseline);
    REQUIRE(beforeCursorRestore);
    REQUIRE(f.rotators.restore(f.rotators.checkpoint()));
    CHECK_FALSE(f.owner.canCommit(*beforeCursorRestore));
    auto beforeFigures = f.owner.prepareRestore(baseline);
    REQUIRE(beforeFigures);
    f.rotators.bindFigures(f.device, f.layout, AssetScope(f.items));
    CHECK_FALSE(f.owner.canCommit(*beforeFigures));
    auto beforeBind = f.owner.prepareRestore(f.state());
    REQUIRE(beforeBind);
    f.rotators.bind(f.layout);
    CHECK_FALSE(f.owner.canCommit(*beforeBind));
    REQUIRE_FALSE(f.rotators.rotator(1).pad);
    CHECK_FALSE(f.owner.restore(baseline));
    Rotators empty;
    RotatorsCheckpoint emptyOwner{empty};
    const auto emptyState = emptyOwner.checkpoint();
    REQUIRE(emptyState);
    REQUIRE(emptyOwner.restore(*emptyState));
}

TEST_CASE("archived sequence-free pad figures restore rest poses and reject nonzero cursors",
          "[rotators-checkpoint][item-figure-checkpoint]") {
    Fixture fixture("rotators-checkpoint-static-tree");
    auto figure = *fixture.rotators.rotator(1).pad;
    REQUIRE(figure.presentation().tree);
    REQUIRE(figure.presentation().tree->sequences.empty());
    const auto state = ItemFigureCheckpoint::capture(figure);
    REQUIRE(state);
    REQUIRE(state->bound);
    REQUIRE(state->archive);
    REQUIRE_FALSE(state->state.animation.playing);
    auto restored = ItemFigureCheckpoint::prepare(figure, *state);
    REQUIRE(restored);
    CHECK(ItemFigureCheckpoint::capture(*restored) == state);
    const auto uploads = fixture.device.texturesCreated;
    figure.capturePresentation();
    restored->capturePresentation();
    figure.update(1.0f / 60);
    restored->update(1.0f / 60);
    CHECK(ItemFigureCheckpoint::capture(*restored) == ItemFigureCheckpoint::capture(figure));
    for (const s32 mutation : {0, 1, 2, 3, 4, 5, 6}) {
        auto invalid = *state;
        switch (mutation) {
        case 0: invalid.state.animation.sequence = 1; break;
        case 1: invalid.state.animation.frame = 1; break;
        case 2: invalid.state.animation.time = 1; break;
        case 3: invalid.state.poseSequence = 1; break;
        case 4: invalid.state.poseFrame = 1; break;
        case 5: invalid.state.textureSequence = 1; break;
        case 6: invalid.state.textureFrame = 1; break;
        }
        REQUIRE(invalid.valid());
        CHECK_FALSE(ItemFigureCheckpoint::prepare(figure, invalid));
    }
    CHECK(fixture.device.texturesCreated == uploads);
}

} // namespace
