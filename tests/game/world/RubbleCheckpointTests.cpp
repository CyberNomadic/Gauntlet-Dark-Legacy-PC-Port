#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/netplay/LocalStateHistory.h"
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

TEST_CASE("native debris history removes predicted births and reuses bound models during replay",
          "[local-rewind][rubble-local-rewind]") {
    Fixture reference, corrected;
    LocalStateHistory<Rubble::LocalSnapshot> history;
    std::array<Rubble::LocalSnapshot, 8> expectedLocal;
    Rubble::LocalSnapshot observed;
    const auto textures = corrected.device.texturesCreated;
    const auto updates = corrected.device.textureUpdates;
    auto spawn = [](Fixture& fixture, u64 tick) {
        if (tick % 3 != 0) {
            return;
        }
        const auto object = tick % 2 == 0 ? Rubble::kBlownBarrel : Rubble::kGasBarrel;
        const auto placement = glm::translate(Mat4{1}, Vec3{static_cast<f32>(tick), 2, 8});
        REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, object, placement));
    };
    u64 tick = 0;
    for (u64 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        REQUIRE(history.capture(tick, [&](auto& saved) {
            corrected.rubble.saveLocal(saved);
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
            spawn(corrected, tick + offset);
            corrected.rubble.saveLocal(expectedLocal[static_cast<usize>(offset)]);
        }
        REQUIRE(corrected.rubble.restoreLocal(*saved));
        for (u64 offset = 0; offset < distance; ++offset) {
            spawn(reference, tick + offset);
            REQUIRE(corrected.rubble.leave(corrected.device, corrected.archives, "UNKNOWN",
                                           glm::translate(Mat4{1}, Vec3{-40, 10, -20})));
        }
        // Native history has no catalogue admission step, even for a first-time birth.
        CHECK_FALSE(corrected.owner.checkpoint());
        REQUIRE(corrected.rubble.restoreLocal(*saved));
        CHECK_FALSE(corrected.owner.canCommit(*stale));
        CHECK(corrected.owner.checkpoint() == before);
        for (u64 offset = 0; offset < distance; ++offset) {
            spawn(corrected, tick + offset);
            corrected.rubble.saveLocal(observed);
            CHECK(corrected.rubble.equalLocal(observed, expectedLocal[static_cast<usize>(offset)]));
        }
        CHECK(corrected.owner.checkpoint() == reference.owner.checkpoint());
        REQUIRE(corrected.rubble.size() == reference.rubble.size());
        for (usize index = 0; index < reference.rubble.size(); ++index) {
            CHECK(corrected.rubble.transform(index) == reference.rubble.transform(index));
        }
        reference.device.draws.clear();
        corrected.device.draws.clear();
        reference.rubble.draw(reference.device, Mat4{1}, {});
        corrected.rubble.draw(corrected.device, Mat4{1}, {});
        REQUIRE(corrected.device.draws.size() == reference.device.draws.size());
        for (usize index = 0; index < reference.device.draws.size(); ++index) {
            CHECK(corrected.device.draws[index].vertices == reference.device.draws[index].vertices);
            CHECK(corrected.device.draws[index].transform ==
                  reference.device.draws[index].transform);
            CHECK(corrected.device.draws[index].state == reference.device.draws[index].state);
        }
        tick += distance;
    }
    CHECK(corrected.device.texturesCreated == textures);
    CHECK(corrected.device.textureUpdates == updates);
}

TEST_CASE("native debris snapshots retain uncatalogued values but reject replaced owners",
          "[local-rewind][rubble-local-rewind]") {
    Fixture first, second;
    Rubble::LocalSnapshot saved;
    CHECK_FALSE(first.rubble.restoreLocal(saved));
    CHECK_FALSE(first.rubble.equalLocal(saved, saved));
    REQUIRE(first.rubble.leave(first.device, first.archives, "UNKNOWN", Mat4{1}));
    first.rubble.saveLocal(saved);
    CHECK_FALSE(second.rubble.restoreLocal(saved));
    CHECK_FALSE(second.rubble.equalLocal(saved, saved));
    CHECK(second.rubble.size() == 0);
    REQUIRE(first.rubble.leave(first.device, first.archives, Rubble::kGasBarrel, Mat4{1}));
    REQUIRE(first.rubble.restoreLocal(saved));
    REQUIRE(first.rubble.size() == 1);
    CHECK(first.rubble.presentation()[0].object == "UNKNOWN");
    CHECK_FALSE(first.owner.checkpoint());
    first.rubble.clear();
    CHECK_FALSE(first.rubble.restoreLocal(saved));
    CHECK_FALSE(first.rubble.equalLocal(saved, saved));
    REQUIRE(first.rubble.leave(first.device, first.archives, "UNKNOWN", Mat4{1}));
    CHECK_FALSE(first.rubble.restoreLocal(saved));
}

TEST_CASE("native debris equality distinguishes same-count mesh identities and placement",
          "[local-rewind][rubble-local-rewind][native-equality]") {
    Fixture fixture;
    Rubble::LocalSnapshot empty, barrel, gas, moved, replay;
    fixture.rubble.saveLocal(empty);
    REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, Rubble::kBlownBarrel, Mat4{1}));
    fixture.rubble.saveLocal(barrel);
    REQUIRE(fixture.rubble.restoreLocal(empty));
    REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, Rubble::kGasBarrel, Mat4{1}));
    fixture.rubble.saveLocal(gas);
    CHECK_FALSE(fixture.rubble.equalLocal(barrel, gas));
    REQUIRE(fixture.rubble.restoreLocal(empty));
    REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, Rubble::kBlownBarrel,
                                 glm::translate(Mat4{1}, Vec3{0, 1, 0})));
    fixture.rubble.saveLocal(moved);
    CHECK_FALSE(fixture.rubble.equalLocal(barrel, moved));
    REQUIRE(fixture.rubble.restoreLocal(empty));
    REQUIRE(fixture.rubble.leave(fixture.device, fixture.archives, Rubble::kBlownBarrel, Mat4{1}));
    fixture.rubble.saveLocal(replay);
    CHECK(fixture.rubble.equalLocal(barrel, replay)); // retained immutable model cache
}
} // namespace
