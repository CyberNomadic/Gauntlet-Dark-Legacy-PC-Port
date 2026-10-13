#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/world/LevelWorld.h"
#include "game/world/TrapsCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive items;
    WorldLayout layout;
    WorldCollision collision;
    Traps traps;
    TrapsCheckpoint owner{traps};
    explicit Fixture(bool moving = false) {
        const auto root = test::scratchDirectory("traps-checkpoint");
        writeTextFile(root / "tri.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(
            root / "objects.json",
            R"({"objects":[{"index":0,"name":"TRI","file":"tri.obj","meshTriangles":1}]})");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(
            root / "textures.json",
            R"({"bitmaps":[{"index":0,"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({"trees":[
          {"name":"SPIKES","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"IDLE","frames":1,"frameRate":30},
                        {"name":"ONA","frames":4,"frameRate":30},
                        {"name":"ONB","frames":4,"frameRate":30}]},
          {"name":"SPIKES_D","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"IDLE","frames":2,"frameRate":30}]},
          {"name":"BARE","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"IDLE","frames":1,"frameRate":30},
                        {"name":"ONA","frames":4,"frameRate":30},
                        {"name":"ONB","frames":4,"frameRate":30}]}]})");
        writeTextFile(root / "world.json",
                      R"({"objects":[{"name":"TRI","position":[0,0,0],"flags":4100}],"itemInfos":[
          {"type":8,"subtype":0,"name":"SPIKES","radius":2,"height":3,"value":20,"activeOff":-4},
          {"type":8,"subtype":3,"name":"BARE","radius":2,"height":3,"value":10,"activeOff":-3}],
          "itemInstances":[{"info":0,"position":[0,0,0]},{"info":1,"position":[20,0,0]}]})");
        test::convertModelFixture(root);
        REQUIRE(items.load(root));
        REQUIRE(layout.load(root));
        if (moving) {
            collision.build(
                {{{0, 1, 0}, {Vec3{-32, 0, -32}, Vec3{32, 0, 32}, Vec3{32, 0, -32}}, 0, 4100},
                 {{0, 1, 0}, {Vec3{-32, 0, -32}, Vec3{-32, 0, 32}, Vec3{32, 0, 32}}, 0, 4100}});
            collision.setMovingObjects(std::array<s32, 1>{0});
            collision.setObjectTransform(0, Mat4{1});
        }
        REQUIRE(
            traps.bind(device, layout, items, moving ? &collision : nullptr, 7123, 1.25f, 1.5f));
        REQUIRE(owner.bindResources(device));
    }
};
void compareHits(const std::vector<TrapHit>& a, const std::vector<TrapHit>& b) {
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(a[i].trap == b[i].trap);
        CHECK(a[i].victim == b[i].victim);
        CHECK(a[i].damage == b[i].damage);
        CHECK(a[i].subtype == b[i].subtype);
        CHECK(a[i].pierces == b[i].pierces);
        CHECK(a[i].position == b[i].position);
        CHECK(a[i].impact.flags == b[i].impact.flags);
        CHECK(a[i].impact.direction == b[i].impact.direction);
    }
}
TEST_CASE("trap rewind preserves native randomized cycles victim gaps wakes and time-stop holds",
          "[rollback][traps-checkpoint]") {
    const auto rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    const s32 ticks = 60 / rate;
    Fixture first, second;
    const std::array<TrapVictim, 2> victims{{{{0, 1, 0}, 0.75f, 1.5f}, {{20, 1, 0}, 0.75f, 1.5f}}};
    for (s32 tick = 0; tick < 17; ++tick) {
        first.traps.update(ticks, seconds, victims);
    }
    const auto state = first.owner.checkpoint();
    REQUIRE(state);
    REQUIRE(second.owner.restore(*state));
    CHECK(second.owner.checkpoint() == state);
    const auto textures = second.device.texturesCreated, uploads = second.device.textureUpdates;
    usize hits = 0, wakes = 0;
    for (s32 tick = 0; tick < 400; ++tick) {
        first.traps.capturePresentation();
        second.traps.capturePresentation();
        const bool held = tick >= 100 && tick < 120;
        const auto a = first.traps.update(ticks, seconds, victims, held);
        const auto b = second.traps.update(ticks, seconds, victims, held);
        compareHits(a, b);
        hits += a.size();
        wakes += first.traps.wakes().size();
        CHECK(second.owner.checkpoint() == first.owner.checkpoint());
        if (tick == 200) {
            CHECK(first.traps.stop(0) == second.traps.stop(0));
        }
    }
    CHECK(hits > 0);
    CHECK(wakes > 0);
    CHECK(second.device.texturesCreated == textures);
    CHECK(second.device.textureUpdates == uploads);
}
TEST_CASE("trap checkpoints switch between original disarmed and vanished figures without uploads",
          "[rollback][traps-checkpoint]") {
    Fixture first, second;
    const auto initial = first.owner.checkpoint();
    REQUIRE(initial);
    for (usize i = 0; i < first.traps.size(); ++i) {
        REQUIRE(first.traps.disarm(i, first.device, first.layout, first.items, nullptr));
    }
    CHECK(first.traps.trap(0).disarmed);
    CHECK_FALSE(first.traps.trap(0).gone);
    CHECK(first.traps.trap(1).gone);
    CHECK_FALSE(first.traps.trap(1).shown);
    const auto state = first.owner.checkpoint();
    REQUIRE(state);
    const auto textures = second.device.texturesCreated, uploads = second.device.textureUpdates;
    const auto* address = &second.traps.trap(0);
    REQUIRE(second.owner.restore(*state));
    CHECK(second.owner.checkpoint() == state);
    CHECK(&second.traps.trap(0) == address);
    REQUIRE(second.owner.restore(*initial));
    CHECK(second.owner.checkpoint() == initial);
    CHECK_FALSE(second.traps.trap(0).disarmed);
    CHECK(second.traps.trap(1).shown);
    CHECK(second.device.texturesCreated == textures);
    CHECK(second.device.textureUpdates == uploads);
}
TEST_CASE("trap plans reject malformed state wrong authored rows and changed bindings atomically",
          "[rollback][traps-checkpoint]") {
    Fixture fixture;
    const auto state = fixture.owner.checkpoint();
    REQUIRE(state);
    for (const int change : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        auto invalid = *state;
        switch (change) {
        case 0: invalid.random = 0; break;
        case 1: invalid.traps[0].placement.name = "WRONG"; break;
        case 2: invalid.traps[0].damage += 1; break;
        case 3: invalid.gaps.push_back(std::numeric_limits<f32>::quiet_NaN()); break;
        case 4: invalid.wakes.push_back(999); break;
        case 5: invalid.traps[0].support.object = 20; break;
        case 6: invalid.timeScale += 1; break;
        case 7: invalid.traps[0].ticksLeft = std::numeric_limits<s32>::min(); break;
        case 8: invalid.traps[0].action = 100; break;
        }
        CHECK_FALSE(fixture.owner.restore(invalid));
        CHECK(fixture.owner.checkpoint() == state);
    }
    auto undo = fixture.owner.prepareRestore(*state);
    REQUIRE(undo);
    fixture.traps.update(2, 1.0f / 30, {});
    REQUIRE(fixture.owner.commitRestore(std::move(*undo)));
    CHECK(fixture.owner.checkpoint() == state);
    auto stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    TrapsCheckpoint sibling{fixture.traps};
    REQUIRE(sibling.bindResources(fixture.device));
    REQUIRE(sibling.restore(*state));
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    REQUIRE(fixture.traps.bind(fixture.device, fixture.layout, fixture.items, nullptr, 7123, 1.25f,
                               1.5f));
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    CHECK_FALSE(fixture.owner.checkpoint());
    REQUIRE(fixture.owner.bindResources(fixture.device));
    REQUIRE(fixture.owner.restore(*state));
}
TEST_CASE("disarming a striking trap without alternate art retains its frozen native pose",
          "[rollback][traps-checkpoint]") {
    Fixture first, second;
    for (s32 tick = 0; tick < 1000 && first.traps.trap(1).action != 2; ++tick) {
        first.traps.update(1, 1.0f / 60, {});
    }
    REQUIRE(first.traps.trap(1).action == 2);
    first.traps.update(1, 1.0f / 60, {});
    REQUIRE(first.traps.trap(1).action == 2);
    const auto striking = first.owner.checkpoint();
    REQUIRE(striking);
    const auto textures = second.device.texturesCreated;
    REQUIRE(first.traps.disarm(1, first.device, first.layout, first.items, nullptr));
    const auto vanished = first.owner.checkpoint();
    REQUIRE(vanished);
    CHECK(vanished->traps[1].action == Traps::kResting);
    CHECK(vanished->traps[1].gone);
    CHECK(vanished->traps[1].figure == striking->traps[1].figure);
    REQUIRE(second.owner.restore(*vanished));
    CHECK(second.owner.checkpoint() == vanished);
    const std::array<TrapVictim, 1> victims{{{{20, 1, 0}, 0.75f, 1.5f}}};
    for (s32 tick = 0; tick < 60; ++tick) {
        compareHits(first.traps.update(1, 1.0f / 60, victims),
                    second.traps.update(1, 1.0f / 60, victims));
        CHECK(second.owner.checkpoint() == first.owner.checkpoint());
    }
    REQUIRE(first.owner.restore(*striking));
    REQUIRE(second.owner.restore(*striking));
    CHECK_FALSE(second.traps.trap(1).disarmed);
    for (s32 tick = 0; tick < 60; ++tick) {
        compareHits(first.traps.update(1, 1.0f / 60, victims),
                    second.traps.update(1, 1.0f / 60, victims));
        CHECK(second.owner.checkpoint() == first.owner.checkpoint());
    }
    CHECK(second.device.texturesCreated == textures);
}
TEST_CASE(
    "native trap rosters restore every authored and disarmed figure on independent stage loads",
    "[rollback][traps-checkpoint][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto stage = GENERATE("F1", "J4", "G2");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(stage);
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    Traps a, b;
    a.bind(device, world.layout(), world.items(), &world.collision(), 42, 1, 1,
           &world.realmItems());
    b.bind(device, world.layout(), world.items(), &world.collision(), 42, 1, 1,
           &world.realmItems());
    TrapsCheckpoint first{a}, second{b};
    REQUIRE(first.bindResources(device));
    REQUIRE(second.bindResources(device));
    const auto state = first.checkpoint();
    REQUIRE(state);
    REQUIRE(second.restore(*state));
    CHECK(second.checkpoint() == state);
    const auto textures = device.texturesCreated;
    for (usize i = 0; i < a.size(); ++i) {
        a.disarm(i, device, world.layout(), world.items(), &world.collision(), &world.realmItems());
    }
    const auto disarmed = first.checkpoint();
    REQUIRE(disarmed);
    REQUIRE(second.restore(*disarmed));
    CHECK(second.checkpoint() == disarmed);
    CHECK(device.texturesCreated == textures);
}
TEST_CASE("trap floor attachments rewind with native nonblocking moving support",
          "[rollback][traps-checkpoint]") {
    const auto rate = GENERATE(30, 60);
    Fixture first(true), second(true);
    REQUIRE(first.traps.trap(0).support.object() == 0);
    REQUIRE_FALSE(first.traps.trap(0).box.solid);
    const auto initial = first.owner.checkpoint();
    REQUIRE(initial);
    const auto textures = second.device.texturesCreated;
    for (s32 tick = 0; tick < 90; ++tick) {
        const auto before = first.owner.checkpoint();
        REQUIRE(before);
        REQUIRE(second.owner.restore(*before));
        const f32 phase = static_cast<f32>(tick) * 0.02f;
        const auto position = glm::translate(Mat4{1}, Vec3{phase, -phase, phase}) *
                              glm::rotate(Mat4{1}, phase, Vec3{0, 1, 0});
        first.collision.setObjectTransform(0, position);
        second.collision.setObjectTransform(0, position);
        first.collision.setSolid(0, false);
        second.collision.setSolid(0, false);
        first.traps.capturePresentation();
        second.traps.capturePresentation();
        first.traps.update(60 / rate, 1.0f / static_cast<f32>(rate), {});
        second.traps.update(60 / rate, 1.0f / static_cast<f32>(rate), {});
        const auto after = first.owner.checkpoint();
        REQUIRE(after);
        CHECK(second.owner.checkpoint() == after);
        CHECK(after->traps[0].support.object == 0);
        CHECK_FALSE(after->traps[0].box.solid);
    }
    CHECK(first.traps.trap(0).figure.position() != initial->traps[0].figure.state.position);
    const auto beforeDetach = second.owner.checkpoint();
    REQUIRE(beforeDetach);
    auto undo = second.owner.prepareRestore(*beforeDetach);
    REQUIRE(undo);
    second.collision.clear();
    second.traps.syncFloors();
    const auto detached = second.owner.checkpoint();
    REQUIRE(detached);
    CHECK(detached->traps[0].support.object == -1);
    CHECK(detached->traps[0].support.local == beforeDetach->traps[0].support.local);
    REQUIRE(second.owner.commitRestore(std::move(*undo)));
    CHECK(second.owner.checkpoint() == beforeDetach);
    CHECK_FALSE(second.collision.loaded()); // external collision belongs to the aggregate
    second.traps.syncFloors();
    CHECK(second.owner.checkpoint() == detached);
    CHECK(second.device.texturesCreated == textures);
}
} // namespace
