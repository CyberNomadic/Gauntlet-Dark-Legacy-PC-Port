#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/world/LevelWorld.h"
#include "game/world/StaticSceneryCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive archive;
    WorldCollision collision;
    StaticScenery scenery;
    StaticSceneryCheckpoint owner{scenery};

    explicit Fixture(std::string_view name, bool missing = false) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"ANIM","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"ACTIVE","frames":12,"rate":30,
             "tracks":[{"node":0,"flags":2,"frames":[0,11],"values":[0,1.3]}]}]}]})");
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0],"flags":4100}],
          "itemInfos":[
            {"type":10,"subtype":0,"name":"WALL","collisionType":1,
             "radius":1,"height":5,"collisionOffset":[0.25,0,0]},
            {"type":10,"subtype":0,"name":"ANIM","collisionType":1,
             "radius":1,"height":3},
            {"type":10,"subtype":0,"name":"WALL","collisionType":1,
             "radius":1,"height":2,"collisionFlags":1}],
          "itemInstances":[
            {"info":0,"position":[0,2,0],"minPlayers":1},
            {"info":1,"position":[20,0,0],"minPlayers":2},
            {"info":2,"position":[3,2,0],"minPlayers":12,"flags":2},
            {"info":0,"position":[6,2,0],"params":[41,0]}]})");
        REQUIRE(layout.load(directory));
        if (!missing) {
            REQUIRE(archive.load(directory));
        }
        collision.build({{{0, 1, 0}, {Vec3{-8, 0, -8}, Vec3{8, 0, -8}, Vec3{0, 0, 8}}, 0, 4100}});
        collision.setMovingObjects(std::array<s32, 1>{0});
        collision.setObjectTransform(0, Mat4{1});
        scenery.bind(device, layout, AssetScope(archive), &collision);
        REQUIRE(scenery.size() == 3);
        REQUIRE(scenery.prop(0).figure.hasFigure() == !missing);
        REQUIRE(scenery.prop(0).support.object() == 0);
        REQUIRE_FALSE(scenery.prop(2).figure.hasFigure());
        REQUIRE(scenery.prop(2).support.object() == -1);
    }
    StaticSceneryCheckpoint::State state() const {
        const auto value = owner.checkpoint();
        REQUIRE(value);
        REQUIRE(value->valid());
        return *value;
    }
    void step(s32 tick, f32 seconds) {
        const f32 angle = static_cast<f32>(tick) * 0.013f;
        const Mat4 placement = glm::translate(Mat4{1}, Vec3{angle, -angle, 0}) *
                               glm::rotate(Mat4{1}, angle, Vec3{0, 1, 0});
        collision.setObjectTransform(0, placement);
        collision.setSolid(0, tick % 9 != 0);
        scenery.setPlayerCount(tick % 3 + 1);
        scenery.capturePresentation();
        if (tick % 7 == 0) {
            scenery.syncFloors(); // Camera-only frame: attachments move, clocks do not.
        } else {
            scenery.update(seconds);
        }
    }
};

TEST_CASE("scenery checkpoints replay native static and animated figures and floor contacts",
          "[scenery-checkpoint]") {
    const bool missing = GENERATE(false, true);
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    Fixture host("scenery-checkpoint-host", missing);
    Fixture guest("scenery-checkpoint-guest", missing);
    const auto uploads = guest.device.texturesCreated;
    bool hiddenClockAdvanced = false;
    bool supportMoved = false;
    bool invisibleSolid = false;
    for (s32 tick = 0; tick < 90; ++tick) {
        CAPTURE(tick, missing, seconds);
        const auto before = host.state();
        REQUIRE(guest.owner.restore(before));
        auto undo = host.owner.prepareRestore(before);
        REQUIRE(undo);
        host.step(tick, seconds);
        const auto expected = host.state();
        hiddenClockAdvanced |=
            !expected.props[1].shown &&
            expected.props[1].figure.state.animation != before.props[1].figure.state.animation;
        supportMoved |=
            expected.props[0].figure.state.position != before.props[0].figure.state.position;
        invisibleSolid |= expected.props[2].shown && expected.props[2].box.solid &&
                          !expected.props[2].figure.bound;
        guest.step(tick, seconds);
        CHECK(guest.state() == expected);
        CHECK(guest.scenery.obstacles().size() == host.scenery.obstacles().size());
        REQUIRE(host.owner.commitRestore(std::move(*undo)));
        CHECK(host.state() == before);
        host.step(tick, seconds);
        CHECK(host.state() == expected);
    }
    CHECK(hiddenClockAdvanced == !missing);
    CHECK(supportMoved);
    CHECK(invisibleSolid);
    CHECK(guest.device.texturesCreated == uploads);
    CHECK(host.scenery.prop(2).figure.position().y == 2); // NoFloorPlacement is retained.
}

TEST_CASE("scenery undo retains detached floor residue without rewinding external collision",
          "[scenery-checkpoint]") {
    const bool missing = GENERATE(false, true);
    Fixture fixture("scenery-checkpoint-detach", missing);
    fixture.step(5, 1.0f / 60);
    const auto before = fixture.state();
    auto undo = fixture.owner.prepareRestore(before);
    REQUIRE(undo);
    fixture.collision.clear();
    fixture.scenery.syncFloors();
    const auto detached = fixture.state();
    CHECK(detached.props[0].support.object == -1);
    CHECK(detached.props[0].support.local == before.props[0].support.local);
    CHECK(detached.props[0].figure.state.transform == before.props[0].figure.state.transform);
    REQUIRE(fixture.owner.commitRestore(std::move(*undo)));
    CHECK(fixture.state() == before);
    CHECK_FALSE(fixture.collision.objectTransform(0));
    fixture.scenery.syncFloors();
    CHECK(fixture.state() == detached);
}

TEST_CASE("scenery snapshot plans refuse malformed foreign or rebound owners atomically",
          "[scenery-checkpoint]") {
    Fixture fixture("scenery-checkpoint-fences");
    const auto before = fixture.state();
    auto invalid = before;
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5, 6);
    switch (variant) {
    case 0: invalid.props[0].placement.position.x += 1; break;
    case 1: invalid.props[0].definition.name = "FOREIGN"; break;
    case 2: invalid.props[0].support.local[0].x = std::numeric_limits<f32>::infinity(); break;
    case 3: invalid.props[0].box.height = -1; break;
    case 4: invalid.props[0].figure.state.pose.tree = "FOREIGN"; break;
    case 5: invalid.props[2].figure.bound = true; break;
    case 6: invalid.props[1].instance = invalid.props[0].instance; break;
    default:
        break;
    }
    CHECK_FALSE(fixture.owner.restore(invalid));
    CHECK(fixture.state() == before);
    auto first = fixture.owner.prepareRestore(before);
    auto sibling = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    REQUIRE(sibling);
    StaticSceneryCheckpoint other(fixture.scenery);
    CHECK_FALSE(other.commitRestore(std::move(*first)));
    REQUIRE(fixture.owner.commitRestore(std::move(*first)));
    CHECK_FALSE(fixture.owner.canCommit(*sibling));
    first = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    REQUIRE(other.restore(before));
    CHECK_FALSE(fixture.owner.canCommit(*first));
    first = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    fixture.scenery.bind(fixture.device, fixture.layout, AssetScope(fixture.archive),
                         &fixture.collision);
    CHECK_FALSE(fixture.owner.canCommit(*first));
    first = fixture.owner.prepareRestore(fixture.state());
    REQUIRE(first);
    fixture.scenery.clear();
    CHECK_FALSE(fixture.owner.canCommit(*first));
}

TEST_CASE("native Ghost Town scenery checkpoint restores trusted static exit sign artwork",
          "[scenery-checkpoint][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G1")));
    StaticScenery scenery;
    scenery.bind(device, world.layout(), world.itemScope(), &world.collision());
    REQUIRE(scenery.size() == 1);
    REQUIRE(scenery.prop(0).instance == 107);
    StaticSceneryCheckpoint owner(scenery);
    const auto before = owner.checkpoint();
    REQUIRE(before);
    REQUIRE(before->props[0].figure.bound);
    device.draws.clear();
    scenery.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 1);
    const auto expected = device.draws;
    REQUIRE_FALSE(expected.empty());
    const auto uploads = device.texturesCreated;
    auto undo = owner.prepareRestore(*before);
    REQUIRE(undo);
    scenery.setPlayerCount(0);
    scenery.capturePresentation();
    scenery.update(0.137f);
    REQUIRE(owner.commitRestore(std::move(*undo)));
    CHECK(owner.checkpoint() == before);
    device.draws.clear();
    scenery.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 1);
    REQUIRE(device.draws.size() == expected.size());
    for (usize index = 0; index < expected.size(); ++index) {
        CHECK(device.draws[index].texture == expected[index].texture);
        CHECK(device.draws[index].vertices == expected[index].vertices);
        CHECK(device.draws[index].transform == expected[index].transform);
        CHECK(device.draws[index].state == expected[index].state);
    }
    CHECK(device.texturesCreated == uploads);
}
TEST_CASE("scenery local values rewind supported figures hidden clocks and disabled geometry",
          "[local-rewind][fixture-local-rewind][scenery-local-rewind]") {
    const bool missing = GENERATE(false, true);
    Fixture native("scenery-local-native", missing);
    Fixture replay("scenery-local-replay", missing);
    std::array<StaticScenery::LocalSnapshot, 16> history;
    std::array<StaticScenery::LocalSnapshot, 8> expectedLocal;
    StaticScenery::LocalSnapshot observed;
    WorldCollision::LocalSnapshot floor;
    const auto uploads = replay.device.texturesCreated;
    s32 tick = 0;
    for (usize window = 0; window < 96; ++window) {
        const auto distance = static_cast<s32>(window % 8 + 1);
        CAPTURE(window, distance, missing);
        const auto before = replay.state();
        auto& saved = history[window % history.size()];
        replay.scenery.saveLocal(saved);
        replay.collision.saveLocal(floor);
        CHECK_FALSE(native.scenery.canRestoreLocal(saved));
        CHECK_FALSE(native.scenery.equalLocal(saved, saved));
        for (s32 i = 0; i < distance; ++i) {
            replay.step(tick + i, 1.0f / 60);
            replay.scenery.saveLocal(expectedLocal[static_cast<usize>(i)]);
        }
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.scenery.restoreLocal(saved));
        for (s32 i = 0; i < distance; ++i) {
            replay.step(tick + i + 100, 0.25f);
        }
        replay.scenery.draw(replay.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.3f);
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.scenery.restoreLocal(saved));
        CHECK(replay.state() == before);
        for (s32 i = 0; i < distance; ++i) {
            native.step(tick + i, 1.0f / 60);
            replay.step(tick + i, 1.0f / 60);
            CHECK(replay.state() == native.state());
            replay.scenery.saveLocal(observed);
            CHECK(replay.scenery.equalLocal(observed, expectedLocal[static_cast<usize>(i)]));
            CHECK(replay.scenery.obstacles().size() == native.scenery.obstacles().size());
        }
        tick += distance;
    }
    CHECK(replay.device.texturesCreated == uploads);
    auto& saved = history.front();
    replay.scenery.saveLocal(saved);
    auto stale = replay.owner.prepareRestore(replay.state());
    REQUIRE(stale);
    REQUIRE(replay.scenery.restoreLocal(saved));
    CHECK_FALSE(replay.owner.canCommit(*stale));
    replay.scenery.clear();
    CHECK_FALSE(replay.scenery.restoreLocal(saved));
    CHECK_FALSE(replay.scenery.equalLocal(saved, saved));
}

TEST_CASE("native scenery equality includes detached support with unchanged figure and obstacle",
          "[local-rewind][scenery-local-rewind][native-equality]") {
    Fixture fixture("scenery-local-equality");
    StaticScenery::LocalSnapshot attached;
    StaticScenery::LocalSnapshot detached;
    StaticScenery::LocalSnapshot restored;
    CHECK_FALSE(fixture.scenery.equalLocal(attached, attached));
    fixture.scenery.saveLocal(attached);
    const auto figure = fixture.scenery.prop(0).figure;
    const auto box = fixture.scenery.prop(0).box;
    fixture.collision.clear();
    fixture.scenery.syncFloors();
    REQUIRE(fixture.scenery.prop(0).support.object() == -1);
    fixture.scenery.saveLocal(detached);
    CHECK(fixture.scenery.prop(0).figure.equalLocal(figure));
    CHECK(fixture.scenery.prop(0).box == box);
    CHECK_FALSE(fixture.scenery.equalLocal(attached, detached));
    // The external collision owner is restored separately by a whole-scene rewind.
    REQUIRE(fixture.scenery.restoreLocal(attached));
    fixture.scenery.saveLocal(restored);
    CHECK(fixture.scenery.equalLocal(attached, restored));
}
} // namespace
