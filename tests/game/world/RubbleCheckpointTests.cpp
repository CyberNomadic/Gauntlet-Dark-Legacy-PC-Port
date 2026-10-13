#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/world/RubbleCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive empty, items;
    std::array<ItemArchive*, 2> archives{&empty, &items};
    Rubble rubble;
    RubbleCheckpoint owner{rubble, archives};
    Fixture() {
        const auto root = test::scratchDirectory("rubble-checkpoint");
        writeTextFile(root / "tri.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(root / "objects.json", R"({"objects":[
          {"index":0,"name":"BAREXP0","file":"tri.obj","meshTriangles":1},
          {"index":1,"name":"BARPOI0","file":"tri.obj","meshTriangles":1},
          {"index":2,"name":"UNKNOWN","file":"tri.obj","meshTriangles":1}]})");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "textures.json", R"({"bitmaps":[
          {"index":0,"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({"trees":[{"name":"BARREL",
          "nodes":[{"name":"ROOT","object":"BAREXP0","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"ACTIVE","frames":1,"frameRate":30}]}]})");
        test::convertModelFixture(root);
        REQUIRE(items.load(root));
        REQUIRE(owner.bindResources(device));
    }
};
TEST_CASE(
    "debris checkpoints restore dynamic births across independently bound owners without uploads",
    "[rollback][rubble-checkpoint]") {
    Fixture first, second;
    const auto empty = first.owner.checkpoint();
    REQUIRE(empty);
    REQUIRE(empty->resources.size() == 2);
    CHECK(empty->resources[0].archive == 1);
    const auto where = glm::translate(Mat4{1}, Vec3{4, 2, 8});
    REQUIRE(first.rubble.leave(first.device, first.archives, Rubble::kBlownBarrel, where));
    REQUIRE(first.rubble.leave(first.device, first.archives, Rubble::kGasBarrel, Mat4{1}));
    const auto state = first.owner.checkpoint();
    REQUIRE(state);
    const auto textures = second.device.texturesCreated;
    const auto uploads = second.device.textureUpdates;
    auto rewind = first.owner.prepareRestore(*empty);
    REQUIRE(rewind);
    REQUIRE(second.owner.restore(*state));
    CHECK(second.owner.checkpoint() == state);
    CHECK(second.rubble.transform(0) == where);
    second.rubble.draw(second.device, Mat4{1}, {});
    REQUIRE(second.device.draws.size() == 2);
    CHECK(second.device.draws[0].vertices[0].position.x >= 4);
    CHECK(second.device.texturesCreated == textures);
    CHECK(second.device.textureUpdates == uploads);
    REQUIRE(first.owner.commitRestore(std::move(*rewind)));
    CHECK(first.owner.checkpoint() == empty);
    REQUIRE(first.rubble.leave(first.device, first.archives, Rubble::kBlownBarrel, where));
    REQUIRE(first.rubble.leave(first.device, first.archives, Rubble::kGasBarrel, Mat4{1}));
    CHECK(first.owner.checkpoint() == state);
}
TEST_CASE(
    "debris preparation refuses wrong resources malformed matrices and stale plans atomically",
    "[rollback][rubble-checkpoint]") {
    Fixture fixture;
    REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, Rubble::kBlownBarrel, Mat4{1}));
    const auto state = fixture.owner.checkpoint();
    REQUIRE(state);
    for (const int mutation : {0, 1, 2, 3, 4}) {
        auto invalid = *state;
        switch (mutation) {
        case 0: invalid.resources[0].archive = 0; break;
        case 1: invalid.resources[0].object = "UNKNOWN"; break;
        case 2: invalid.pieces[0].resource = 50; break;
        case 3: invalid.pieces[0].transform[3].x = std::numeric_limits<f32>::quiet_NaN(); break;
        case 4: invalid.resources.push_back(invalid.resources[0]); break;
        }
        CHECK_FALSE(fixture.owner.restore(invalid));
        CHECK(fixture.owner.checkpoint() == state);
    }
    auto stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    RubbleCheckpoint sibling{fixture.rubble, fixture.archives};
    REQUIRE(sibling.bindResources(fixture.device));
    REQUIRE(sibling.restore(*state));
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    REQUIRE(fixture.owner.bindResources(fixture.device));
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    fixture.rubble.clear();
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    CHECK_FALSE(fixture.owner.checkpoint());
    REQUIRE(fixture.owner.bindResources(fixture.device));
    REQUIRE(fixture.owner.restore(*state));
    Fixture other;
    auto foreign = other.owner.prepareRestore(*state);
    REQUIRE(foreign);
    CHECK_FALSE(fixture.owner.commitRestore(std::move(*foreign)));
    CHECK(other.owner.canCommit(*foreign));
}
TEST_CASE("debris never drops uncatalogued live meshes from a full checkpoint",
          "[rollback][rubble-checkpoint]") {
    Fixture fixture;
    REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, "UNKNOWN", Mat4{1}));
    CHECK_FALSE(fixture.owner.checkpoint());
    const std::array<std::string_view, 3> names{Rubble::kBlownBarrel, Rubble::kGasBarrel,
                                                "UNKNOWN"};
    REQUIRE(fixture.owner.bindResources(fixture.device, names));
    const auto state = fixture.owner.checkpoint();
    REQUIRE(state);
    REQUIRE(state->pieces.size() == 1);
    CHECK(state->resources[state->pieces[0].resource].object == "UNKNOWN");
    REQUIRE(fixture.owner.restore(*state));
    CHECK(fixture.owner.checkpoint() == state);
}
} // namespace
