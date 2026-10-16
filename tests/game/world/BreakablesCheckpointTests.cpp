#include <array>
#include <limits>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/world/BreakablesCheckpoint.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void checkStrike(const std::optional<BreakableStrike>& actual,
                 const std::optional<BreakableStrike>& expected) {
    REQUIRE(actual.has_value() == expected.has_value());
    if (actual) {
        CHECK((std::tie(actual->index, actual->kind, actual->broken, actual->position,
                        actual->contents, actual->count) ==
               std::tie(expected->index, expected->kind, expected->broken, expected->position,
                        expected->contents, expected->count)));
    }
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    WorldCollision collision;
    Breakables barrels;
    BreakablesCheckpoint owner{barrels};

    explicit Fixture(std::string_view name, bool moving = false, bool unseen = false) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0],"flags":4100}],
          "itemInfos":[
            {"type":10,"subtype":43,"name":"BARREL","radius":1,"height":3,"armor":1,"hitPoints":5},
            {"type":2,"subtype":43,"name":"BARREL","radius":1,"height":3,"armor":1,"hitPoints":5},
            {"type":10,"subtype":44,"name":"BARREL","radius":1,"height":3,"armor":1,"hitPoints":5},
            {"type":10,"subtype":45,"name":"BARREL","radius":1,"height":3,"armor":1,"hitPoints":5},
            {"type":10,"subtype":43,"name":"BARREL","radius":1,"height":3,"armor":-1,"hitPoints":5},
            {"type":-1,"choices":[8,7]},
            {"type":1,"subtype":2,"name":"KEY","value":1},
            {"type":1,"subtype":1,"name":"GOLD","value":200},
            {"type":-1,"choices":[6,7]}],
          "itemInstances":[
            {"info":0,"position":[0,0,0]},
            {"info":1,"position":[20,0,0],"params":[5,0,0,0,3,0,0,0,0,0,0,0]},
            {"info":2,"position":[40,0,0]},
            {"info":3,"position":[60,0,0]},
            {"info":4,"position":[80,0,0]},
            {"info":1,"position":[100,0,0],"minPlayers":2,
             "params":[5,0,0,0,2,0,0,0,0,0,0,0]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"BARREL","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"WHOLE","frames":2,"rate":30},
             {"name":"ACTIVE","frames":45,"rate":30,
              "tracks":[{"node":0,"flags":2,"frames":[0,44],"values":[0,1.3]}]},
             {"name":"DONE","frames":0}]}]})");
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
        REQUIRE(barrels.bind(device, layout, AssetScope(items), moving ? &collision : nullptr));
        barrels.setPlayerCount(2);
        REQUIRE(barrels.size() == 6);
    }
    BreakablesCheckpoint::State state() const {
        const auto result = owner.checkpoint();
        REQUIRE(result);
        return *result;
    }
    std::optional<BreakableStrike> strike(s32 tick) {
        switch (tick) {
        case 0: return barrels.strike(4, 9999);
        case 1: return barrels.strike(0, 0.1f);
        case 2: return barrels.strike(1, 99);
        case 3: return barrels.strike(2, 99);
        case 4: return barrels.strike(3, 99);
        case 5: return barrels.strike(0, 99);
        case 6: barrels.changeContents(5, 6); break;
        case 7: barrels.changeContents(5, 5); return barrels.strike(5, 99);
        default: break;
        }
        return std::nullopt;
    }
};

TEST_CASE("barrel checkpoints reproduce native random drops fade staves and retirement",
          "[breakables-checkpoint]") {
    Fixture host("barrel-checkpoint-host");
    Fixture guest("barrel-checkpoint-guest");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto hostUploads = host.device.texturesCreated;
    const auto guestUploads = guest.device.texturesCreated;
    bool faded = false;
    bool retainedStaves = false;
    usize drops = 0;
    usize retirements = 0;
    for (s32 tick = 0; tick < 180; ++tick) {
        CAPTURE(tick, seconds);
        const auto before = host.state();
        REQUIRE(guest.owner.restore(before));
        auto undo = host.owner.prepareRestore(before);
        REQUIRE(undo);
        host.barrels.capturePresentation();
        const auto strike = host.strike(tick);
        const auto retired = host.barrels.update(seconds);
        const auto expected = host.state();
        if (strike && strike->kind == BreakableStrike::Kind::Holding && strike->broken) {
            REQUIRE(strike->contents >= 6);
            REQUIRE(strike->contents <= 7);
            ++drops;
        }
        retirements += retired.size();
        const f32 opacity = host.barrels.opacityOf(2);
        faded |= opacity > 0 && opacity < 1;
        retainedStaves |=
            expected.barrels[0].state == Breakables::kBroken && !expected.barrels[0].gone;
        REQUIRE(expected.barrels[4].health == 5);
        if (tick == 1) {
            REQUIRE(expected.barrels[0].health == 4);
        }
        guest.barrels.capturePresentation();
        checkStrike(guest.strike(tick), strike);
        CHECK(guest.barrels.update(seconds) == retired);
        CHECK(guest.state() == expected);
        CHECK(guest.barrels.opacityOf(2) == opacity);
        CHECK(guest.barrels.within(Vec3{0}, 200) == host.barrels.within(Vec3{0}, 200));
        CHECK(guest.barrels.struckBy(Vec3{-5, 1, 0}, Vec3{105, 1, 0}, 1) ==
              host.barrels.struckBy(Vec3{-5, 1, 0}, Vec3{105, 1, 0}, 1));
        REQUIRE(host.owner.commitRestore(std::move(*undo)));
        CHECK(host.state() == before);
        host.barrels.capturePresentation();
        checkStrike(host.strike(tick), strike);
        CHECK(host.barrels.update(seconds) == retired);
        CHECK(host.state() == expected);
    }
    CHECK(faded);
    CHECK(retainedStaves);
    CHECK(drops == 2);
    CHECK(retirements == 2);
    CHECK(host.state().seed != Breakables::kSeedStart);
    CHECK(host.device.texturesCreated == hostUploads);
    CHECK(guest.device.texturesCreated == guestUploads);
}

TEST_CASE("barrel snapshots retain moving support and deliberately absent figure resources",
          "[breakables-checkpoint]") {
    const bool unseen = GENERATE(false, true);
    Fixture fixture("barrel-checkpoint-support", true, unseen);
    REQUIRE(fixture.barrels.barrel(0).support.object() == 0);
    fixture.barrels.capturePresentation();
    const auto before = fixture.state();
    REQUIRE(before.barrels[0].figure.bound == !unseen);
    auto undo = fixture.owner.prepareRestore(before);
    REQUIRE(undo);
    const Mat4 moved =
        glm::translate(Mat4{1}, Vec3{0, -10, 3}) * glm::rotate(Mat4{1}, 0.4f, Vec3{0, 1, 0});
    fixture.collision.setObjectTransform(0, moved);
    fixture.collision.setSolid(0, false);
    fixture.barrels.syncFloors();
    fixture.barrels.setPlayerCount(1);
    fixture.barrels.capturePresentation();
    const auto expected = fixture.state();
    REQUIRE(expected != before);
    REQUIRE_FALSE(expected.barrels[5].shown);
    REQUIRE(fixture.owner.commitRestore(std::move(*undo)));
    CHECK(fixture.state() == before);
    CHECK(fixture.collision.objectTransform(0) == moved);
    fixture.barrels.syncFloors();
    fixture.barrels.setPlayerCount(1);
    fixture.barrels.capturePresentation();
    CHECK(fixture.state() == expected);
    undo = fixture.owner.prepareRestore(expected);
    REQUIRE(undo);
    fixture.collision.clear();
    fixture.barrels.syncFloors();
    REQUIRE(fixture.barrels.barrel(0).support.object() == -1);
    REQUIRE(fixture.owner.commitRestore(std::move(*undo)));
    CHECK(fixture.state() == expected);
}

TEST_CASE("barrel plans reject malformed rosters and fence foreign or stale resources",
          "[breakables-checkpoint]") {
    Fixture fixture("barrel-checkpoint-fences");
    const auto before = fixture.state();
    auto invalid = before;
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    switch (variant) {
    case 0: invalid.infos[5].choices = {7, 8}; break;
    case 1: invalid.barrels[0].placement.position.x += 1; break;
    case 2: invalid.barrels[0].instance = 100; break;
    case 3: invalid.barrels[0].kind = BreakableStrike::Kind::Poison; break;
    case 4: invalid.barrels[0].health = -1; break;
    case 5: invalid.barrels[0].breakingSeconds = std::numeric_limits<f32>::quiet_NaN(); break;
    case 6: invalid.barrels[0].support.local[0].x = std::numeric_limits<f32>::infinity(); break;
    case 7: invalid.barrels[0].figure.state.pose.tree = "FOREIGN"; break;
    case 8: invalid.barrels[0].box.cylinderRadius = -1; break;
    case 9: invalid.barrels[0].radius += 1; break;
    case 10: invalid.barrels[0].support.object = 1; break;
    }
    const auto uploads = fixture.device.texturesCreated;
    CHECK_FALSE(fixture.owner.restore(invalid));
    CHECK(fixture.state() == before);
    CHECK(fixture.device.texturesCreated == uploads);
    auto first = fixture.owner.prepareRestore(before);
    auto sibling = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    REQUIRE(sibling);
    BreakablesCheckpoint otherWrapper(fixture.barrels);
    CHECK_FALSE(otherWrapper.commitRestore(std::move(*first)));
    REQUIRE(fixture.owner.commitRestore(std::move(*first)));
    CHECK_FALSE(fixture.owner.canCommit(*sibling));
    first = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    REQUIRE(otherWrapper.restore(before));
    CHECK_FALSE(fixture.owner.canCommit(*first));
    first = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    fixture.barrels.clear();
    CHECK_FALSE(fixture.owner.canCommit(*first));
}

TEST_CASE("native campaign barrel snapshots preserve their full original placement roster",
          "[breakables-checkpoint][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto name = GENERATE("G1", "G3");
    CAPTURE(name);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName(name)));
    Breakables barrels;
    REQUIRE(barrels.bind(device, world.layout(), world.itemScope(), &world.collision()));
    BreakablesCheckpoint owner(barrels);
    const auto before = owner.checkpoint();
    REQUIRE(before);
    REQUIRE(before->barrels.size() == barrels.size());
    const auto uploads = device.texturesCreated;
    auto undo = owner.prepareRestore(*before);
    REQUIRE(undo);
    barrels.setPlayerCount(4);
    barrels.capturePresentation();
    barrels.strike(0, 1);
    barrels.update(1.0f / 60);
    REQUIRE(owner.commitRestore(std::move(*undo)));
    CHECK(owner.checkpoint() == before);
    CHECK(device.texturesCreated == uploads);
}
TEST_CASE("barrel local values replay destruction random contents and retirement",
          "[local-rewind][fixture-local-rewind][breakables-local-rewind]") {
    const bool unseen = GENERATE(false, true);
    Fixture native("barrel-local-native", true, unseen);
    Fixture replay("barrel-local-replay", true, unseen);
    std::array<Breakables::LocalSnapshot, 16> history;
    WorldCollision::LocalSnapshot floor;
    const auto uploads = replay.device.texturesCreated;
    usize drops = 0, retired = 0;
    s32 tick = 0;
    for (usize window = 0; window < 96; ++window) {
        const auto distance = static_cast<s32>(window % 8 + 1);
        CAPTURE(window, distance, unseen);
        const auto before = replay.state();
        auto& saved = history[window % history.size()];
        replay.barrels.saveLocal(saved);
        replay.collision.saveLocal(floor);
        CHECK_FALSE(native.barrels.restoreLocal(saved));
        CHECK_FALSE(native.barrels.equalLocal(saved, saved));
        std::vector<Breakables::LocalSnapshot> nativeStates(static_cast<usize>(distance));
        for (s32 i = 0; i < distance; ++i) {
            replay.collision.setObjectTransform(
                0, glm::translate(Mat4{1}, Vec3{0, -0.01f * static_cast<f32>(tick + i), 0}));
            replay.barrels.capturePresentation();
            (void)replay.strike(tick + i);
            replay.barrels.update(1.0f / 60);
            replay.barrels.saveLocal(nativeStates[static_cast<usize>(i)]);
        }
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.barrels.restoreLocal(saved));
        for (s32 i = 0; i < distance; ++i) {
            replay.collision.setObjectTransform(
                0, glm::translate(Mat4{1}, Vec3{7, static_cast<f32>(i), 3}));
            replay.barrels.capturePresentation();
            replay.barrels.setPlayerCount(4);
            replay.barrels.changeContents(1, 7);
            replay.barrels.strike(static_cast<usize>(i) % replay.barrels.size(), 9999);
            replay.barrels.update(0.125f);
        }
        replay.barrels.draw(replay.device, Mat4{1}, {}, 0.5f);
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.barrels.restoreLocal(saved));
        CHECK(replay.state() == before);
        for (s32 i = 0; i < distance; ++i) {
            const auto frame = tick + i;
            const Mat4 placement =
                glm::translate(Mat4{1}, Vec3{0, -0.01f * static_cast<f32>(frame), 0});
            native.collision.setObjectTransform(0, placement);
            replay.collision.setObjectTransform(0, placement);
            native.barrels.capturePresentation();
            replay.barrels.capturePresentation();
            const auto strike = native.strike(frame);
            checkStrike(replay.strike(frame), strike);
            if (strike && strike->broken && strike->kind == BreakableStrike::Kind::Holding) {
                ++drops;
            }
            const auto gone = native.barrels.update(1.0f / 60);
            CHECK(replay.barrels.update(1.0f / 60) == gone);
            retired += gone.size();
            CHECK(replay.state() == native.state());
            Breakables::LocalSnapshot actual;
            replay.barrels.saveLocal(actual);
            CHECK(replay.barrels.equalLocal(actual, nativeStates[static_cast<usize>(i)]));
        }
        tick += distance;
    }
    CHECK(drops == 2);
    CHECK(retired == 2);
    CHECK(replay.device.texturesCreated == uploads);
    auto& saved = history.front();
    replay.barrels.saveLocal(saved);
    auto stale = replay.owner.prepareRestore(replay.state());
    REQUIRE(stale);
    REQUIRE(replay.barrels.restoreLocal(saved));
    CHECK_FALSE(replay.owner.canCommit(*stale));
    replay.barrels.clear();
    CHECK_FALSE(replay.barrels.restoreLocal(saved));
    CHECK_FALSE(replay.barrels.equalLocal(saved, saved));
}

TEST_CASE("native barrel equality detects hidden loot and support binding changes",
          "[local-rewind][fixture-local-rewind][breakables-local-rewind]") {
    Fixture fixture("barrel-local-hidden-equality", true);
    Breakables::LocalSnapshot initial, changed, actual, empty;
    fixture.barrels.saveLocal(initial);
    REQUIRE(fixture.barrels.equalLocal(initial, initial));
    CHECK_FALSE(fixture.barrels.equalLocal(initial, empty));
    CHECK_FALSE(fixture.barrels.equalLocal(empty, empty));
    const auto before = fixture.barrels.barrel(1);
    fixture.barrels.changeContents(1, 6);
    fixture.barrels.saveLocal(changed);
    REQUIRE(fixture.barrels.barrel(1).figure == before.figure);
    REQUIRE(fixture.barrels.barrel(1).box == before.box);
    CHECK_FALSE(fixture.barrels.equalLocal(initial, changed));
    CHECK_FALSE(fixture.barrels.equalLocal(changed, initial));
    REQUIRE(fixture.barrels.restoreLocal(initial));
    fixture.barrels.saveLocal(actual);
    CHECK(fixture.barrels.equalLocal(initial, actual));

    const auto rider = fixture.barrels.barrel(0);
    REQUIRE(rider.support.object() == 0);
    fixture.collision.clear();
    fixture.barrels.syncFloors();
    REQUIRE(fixture.barrels.barrel(0).support.object() == -1);
    CHECK(fixture.barrels.barrel(0).figure == rider.figure);
    CHECK(fixture.barrels.barrel(0).box == rider.box);
    fixture.barrels.saveLocal(changed);
    CHECK_FALSE(fixture.barrels.equalLocal(initial, changed));
    // The collision owner is external. Comparing/restoring barrel values alone
    // must still include the now-detached support, without mutating that owner.
    REQUIRE(fixture.barrels.restoreLocal(initial));
    fixture.barrels.saveLocal(actual);
    CHECK(fixture.barrels.equalLocal(initial, actual));

    REQUIRE(fixture.barrels.bind(fixture.device, fixture.layout, AssetScope(fixture.items),
                                 &fixture.collision));
    fixture.barrels.saveLocal(actual);
    CHECK_FALSE(fixture.barrels.equalLocal(initial, actual));
    CHECK_FALSE(fixture.barrels.equalLocal(initial, initial));
}

TEST_CASE("native barrel rewind keeps bound definitions but restores nested loot choices",
          "[local-rewind][fixture-local-rewind][breakables-local-rewind]") {
    Fixture fixture("barrel-local-bound-definitions");
    const auto before = fixture.state();
    Breakables::LocalSnapshot saved;
    fixture.barrels.saveLocal(saved);
    const auto expectedDrop = fixture.barrels.strike(1, 99);
    REQUIRE(expectedDrop);
    REQUIRE(expectedDrop->broken);
    REQUIRE(expectedDrop->contents >= 6);
    REQUIRE(expectedDrop->contents <= 7);
    REQUIRE(expectedDrop->count == 3);
    const auto expected = fixture.state();
    REQUIRE(expected.seed != before.seed);

    REQUIRE(fixture.barrels.restoreLocal(saved));
    fixture.barrels.changeContents(1, 6);
    (void)fixture.barrels.strike(1, 99);
    (void)fixture.barrels.strike(5, 99);
    const auto uploads = fixture.device.texturesCreated;
    REQUIRE(fixture.barrels.restoreLocal(saved));
    CHECK(fixture.state() == before);
    checkStrike(fixture.barrels.strike(1, 99), expectedDrop);
    CHECK(fixture.state() == expected);
    CHECK(fixture.device.texturesCreated == uploads);

    REQUIRE(
        fixture.barrels.bind(fixture.device, fixture.layout, AssetScope(fixture.items), nullptr));
    fixture.barrels.setPlayerCount(2);
    CHECK_FALSE(fixture.barrels.restoreLocal(saved));
    const auto rebound = fixture.state();
    CHECK(rebound.infos.size() == before.infos.size());
    CHECK(rebound.infos[5].choices == before.infos[5].choices);
    CHECK(rebound.infos[8].choices == before.infos[8].choices);
    checkStrike(fixture.barrels.strike(1, 99), expectedDrop);
    fixture.barrels.saveLocal(saved);
    fixture.barrels.clear();
    CHECK_FALSE(fixture.barrels.restoreLocal(saved));
    CHECK(fixture.state().infos.empty());
}
} // namespace
