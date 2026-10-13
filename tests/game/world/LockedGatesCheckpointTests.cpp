#include <array>
#include <limits>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/world/LevelWorld.h"
#include "game/world/LockedGatesCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void checkEvents(std::span<const GateEvent> actual, std::span<const GateEvent> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize index = 0; index < actual.size(); ++index) {
        const auto& a = actual[index];
        const auto& b = expected[index];
        CHECK(std::tie(a.kind, a.gate, a.visitor, a.position) ==
              std::tie(b.kind, b.gate, b.visitor, b.position));
    }
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    WorldCollision collision;
    LockedGates gates;
    LockedGatesCheckpoint owner{gates};

    explicit Fixture(std::string_view name, bool moving = false, bool unseen = false) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0],"flags":4100}],
          "itemInfos":[
            {"type":7,"subtype":1,"name":"GATE","radius":1,"height":3},
            {"type":7,"subtype":2,"name":"GATE","radius":3,"height":1,"collisionType":4}],
          "itemInstances":[
            {"info":0,"position":[0,0,0]},
            {"info":0,"position":[20,0,0],"minPlayers":2},
            {"info":1,"position":[40,0,0],"collision":[
              {"normal":[0,1,0],"vertices":[[-3,0,-3],[3,0,-3],[0,0,3]]}]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"GATE","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"rate":30},
             {"name":"ACTIVE","frames":45,"rate":30,
              "tracks":[{"node":0,"flags":2,"frames":[0,44],"values":[0,1.3]}]},
             {"name":"OPEN","frames":0}]}]})");
        REQUIRE(layout.load(directory));
        if (!unseen) {
            REQUIRE(items.load(directory));
        }
        if (moving) {
            collision.build(
                {{{0, 1, 0}, {Vec3{-8, 0, -8}, Vec3{8, 0, -8}, Vec3{0, 0, 8}}, 0, 4100}});
            collision.setMovingObjects(std::array<s32, 1>{0});
            collision.setObjectTransform(0, Mat4{1});
        }
        REQUIRE(gates.bind(device, layout, items, moving ? &collision : nullptr));
        gates.setPlayerCount(2);
        REQUIRE(gates.size() == 3);
        REQUIRE_FALSE(gates.gate(2).blocksPassage);
    }
    LockedGatesCheckpoint::State state() const {
        const auto result = owner.checkpoint();
        REQUIRE(result);
        return *result;
    }
};

TEST_CASE("gate checkpoints resume native ordered refusal opening and passability clocks",
          "[gates-checkpoint]") {
    Fixture host("gate-checkpoint-host");
    Fixture guest("gate-checkpoint-guest");
    const s32 ticks = GENERATE(1, 2);
    const f32 seconds = static_cast<f32>(ticks) / 60;
    const auto uploads = guest.device.texturesCreated;
    bool refused = false;
    bool unlocked = false;
    bool passingWhileOpening = false;
    bool opened = false;
    for (s32 tick = 0; tick < 180; ++tick) {
        // Sparse party positions still yield native visitor indices; the second
        // visitor opens gate zero while the first approaches the other gate.
        const std::array party{ChestVisitor{Vec3{20, 0, 0}, 1, tick < 8 ? 0 : 1},
                               ChestVisitor{Vec3{0}, 1, tick < 4 ? 0 : 1},
                               ChestVisitor{Vec3{40, 0, 0}, 1, 1}};
        const auto before = host.state();
        REQUIRE(guest.owner.restore(before));
        auto rewind = host.owner.prepareRestore(before);
        REQUIRE(rewind);
        host.gates.capturePresentation();
        const auto events = host.gates.update(ticks, seconds, party);
        const auto expected = host.state();
        for (const auto& event : events) {
            refused |= event.kind == GateEvent::Kind::Refused;
            unlocked |= event.kind == GateEvent::Kind::Unlocked;
            CHECK(event.gate != 2);
        }
        passingWhileOpening |=
            expected.gates[0].state == LockedGates::kOpening && !expected.gates[0].box.solid;
        opened |= expected.gates[0].state == LockedGates::kOpen;
        REQUIRE(expected.gates[2].state == LockedGates::kShut);
        guest.gates.capturePresentation();
        checkEvents(guest.gates.update(ticks, seconds, party), events);
        CHECK(guest.state() == expected);
        REQUIRE(host.owner.commitRestore(std::move(*rewind)));
        CHECK(host.state() == before);
        host.gates.capturePresentation();
        checkEvents(host.gates.update(ticks, seconds, party), events);
        CHECK(host.state() == expected);
    }
    CHECK(refused);
    CHECK(unlocked);
    CHECK(passingWhileOpening);
    CHECK(opened);
    CHECK(guest.device.texturesCreated == uploads);
}

TEST_CASE("gate support checkpoint preserves all local offsets even without figure art",
          "[gates-checkpoint]") {
    const bool unseen = GENERATE(false, true);
    Fixture fixture("gate-checkpoint-support", true, unseen);
    REQUIRE(fixture.gates.gate(0).support.object() == 0);
    fixture.gates.capturePresentation();
    const auto before = fixture.state();
    REQUIRE(before.gates[0].figure.bound == !unseen);
    auto rewind = fixture.owner.prepareRestore(before);
    REQUIRE(rewind);
    const Mat4 moved =
        glm::translate(Mat4{1}, Vec3{0, -10, 3}) * glm::rotate(Mat4{1}, 0.4f, Vec3{0, 1, 0});
    fixture.collision.setObjectTransform(0, moved);
    fixture.collision.setSolid(0, false);
    fixture.gates.syncFloors();
    fixture.gates.capturePresentation();
    const auto expected = fixture.state();
    REQUIRE(expected != before);
    REQUIRE(fixture.owner.commitRestore(std::move(*rewind)));
    CHECK(fixture.state() == before);
    CHECK(fixture.collision.objectTransform(0) == moved);
    fixture.gates.syncFloors();
    fixture.gates.capturePresentation();
    CHECK(fixture.state() == expected);
    rewind = fixture.owner.prepareRestore(expected);
    REQUIRE(rewind);
    fixture.collision.clear();
    fixture.gates.syncFloors();
    REQUIRE(fixture.gates.gate(0).support.object() == -1);
    REQUIRE(fixture.owner.commitRestore(std::move(*rewind)));
    CHECK(fixture.state() == expected);
}

TEST_CASE("gate snapshot plans refuse malformed or foreign rosters and stale siblings",
          "[gates-checkpoint]") {
    Fixture fixture("gate-checkpoint-fences");
    const auto before = fixture.state();
    auto invalid = before;
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    switch (variant) {
    case 0: invalid.gates[0].placement.position.x += 1; break;
    case 1: invalid.gates[0].definition.name = "FOREIGN"; break;
    case 2: invalid.gates[0].openingTicks = -1; break;
    case 3: invalid.gates[0].support.local[0].x = std::numeric_limits<f32>::infinity(); break;
    case 4: invalid.gates[0].figure.state.pose.tree = "FOREIGN"; break;
    case 5: invalid.gates[2].blocksPassage = true; break;
    case 6: invalid.gates[0].refusalLeft = -1; break;
    case 7: invalid.gates[0].box.height = -1; break;
    }
    CHECK_FALSE(fixture.owner.restore(invalid));
    CHECK(fixture.state() == before);
    auto first = fixture.owner.prepareRestore(before);
    auto sibling = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    REQUIRE(sibling);
    LockedGatesCheckpoint otherWrapper(fixture.gates);
    CHECK_FALSE(otherWrapper.commitRestore(std::move(*first)));
    REQUIRE(fixture.owner.commitRestore(std::move(*first)));
    CHECK_FALSE(fixture.owner.canCommit(*sibling));
    first = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    REQUIRE(otherWrapper.restore(before));
    CHECK_FALSE(fixture.owner.canCommit(*first));
    first = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    fixture.gates.clear();
    CHECK_FALSE(fixture.owner.canCommit(*first));
}

TEST_CASE("native campaign gates retain the complete trusted geometry and figure roster",
          "[gates-checkpoint][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto name = GENERATE("G1", "G3", "E1");
    CAPTURE(name);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName(name)));
    LockedGates gates;
    REQUIRE(
        gates.bind(device, world.layout(), world.items(), &world.collision(), &world.realmItems()));
    LockedGatesCheckpoint owner(gates);
    const auto before = owner.checkpoint();
    REQUIRE(before);
    REQUIRE(before->gates.size() == gates.size());
    const auto uploads = device.texturesCreated;
    auto rewind = owner.prepareRestore(*before);
    REQUIRE(rewind);
    gates.setPlayerCount(4);
    gates.capturePresentation();
    gates.update(1, 1.0f / 60, {});
    REQUIRE(owner.commitRestore(std::move(*rewind)));
    CHECK(owner.checkpoint() == before);
    CHECK(device.texturesCreated == uploads);
}
} // namespace
