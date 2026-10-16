#include <algorithm>
#include <array>
#include <limits>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/world/ChestsCheckpoint.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

auto eventValues(const ChestEvent& value) {
    return std::tie(value.kind, value.chest, value.visitor, value.contents, value.gold,
                    value.explodes, value.position);
}

void checkEvents(std::span<const ChestEvent> actual, std::span<const ChestEvent> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize index = 0; index < actual.size(); ++index) {
        CHECK((eventValues(actual[index]) == eventValues(expected[index])));
    }
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    Chests chests;
    ChestsCheckpoint owner{chests};

    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0]}],
          "itemInfos":[
            {"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2,"activeType":16},
            {"type":2,"subtype":47,"name":"CHEST","radius":1,"height":2,"activeType":16},
            {"type":2,"subtype":44,"name":"CHEST","radius":1,"height":2,"activeType":16},
            {"type":-1,"choices":[4,5]},
            {"type":1,"subtype":2,"name":"KEY","radius":1,"value":1},
            {"type":1,"subtype":1,"name":"KEY","radius":1,"value":200}],
          "itemInstances":[
            {"info":0,"position":[0,0,0],"params":[3,0,0,0,1,0,0,0,0,0,0,0]},
            {"info":1,"position":[20,0,0],"params":[5,0,0,0,1,0,0,0,0,0,0,0]},
            {"info":2,"position":[40,0,0],"params":[4,0,0,0,1,0,0,0,0,0,0,0]},
            {"info":0,"position":[60,0,0],"minPlayers":2,
             "params":[3,0,0,0,1,0,0,0,0,0,0,0]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"CHEST","nodes":[
             {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
             {"name":"NULL1","parent":0,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"frameRate":30},
             {"name":"ACTIVE","frames":8,"frameRate":30,
              "tracks":[{"node":1,"flags":32,"frames":[0,7],"values":[0,2]}]},
             {"name":"OPEN","frames":0}]},
          {"name":"KEY","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":8,"frameRate":30}]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(items.load(directory));
        REQUIRE(chests.bind(device, layout, AssetScope(items), nullptr));
        chests.setPlayerCount(2);
        REQUIRE(chests.size() == 4);
        REQUIRE(chests.chest(0).figure.hasFigure());
    }
    ChestsCheckpoint::State state() const {
        const auto result = owner.checkpoint();
        REQUIRE(result);
        return *result;
    }
    void reveal() {
        const std::array party{ChestVisitor{Vec3{0}, 1, 0, true},
                               ChestVisitor{Vec3{60, 0, 0}, 1, 0, true}};
        REQUIRE(chests.updateXray(device, AssetScope(items), items, party) == 2);
        REQUIRE(chests.chest(0).preview.hasFigure());
        REQUIRE(chests.chest(3).preview.hasFigure());
    }
};

TEST_CASE("native chest history cancels opening rewards transmutation and first preview births",
          "[local-rewind][chest-local-rewind]") {
    Fixture pool("chest-local-history");
    Chests::LocalSnapshot initial;
    pool.chests.saveLocal(initial);
    std::array<Chests::LocalSnapshot, 16> history;
    const auto step = [&](s32 tick, bool wrong) {
        pool.chests.capturePresentation();
        const s32 phase = std::min(tick / 20, 3);
        const std::array party{
            ChestVisitor{Vec3{static_cast<f32>((wrong ? 3 - phase : phase) * 20), 0, 0}, 1,
                         wrong ? 0 : (tick % 20 < 3 ? 0 : 1), tick % 7 == 0}};
        (void)pool.chests.updateXray(pool.device, AssetScope(pool.items), pool.items, party);
        if (tick == 19 || wrong) {
            (void)pool.chests.transmute(3, wrong ? 5 : 4, wrong ? 60.0f : 9.0f);
        }
        if (tick == 39) {
            (void)pool.chests.changeContents(2, 5, pool.device, pool.layout, AssetScope(pool.items),
                                             nullptr, "KEY");
        }
        return pool.chests.update(1.0f / 60, party);
    };
    for (s32 window = 0; window < 96; ++window) {
        CAPTURE(window);
        REQUIRE(pool.chests.restoreLocal(initial));
        for (s32 tick = 0; tick < window; ++tick) {
            (void)step(tick, false);
        }
        auto& saved = history[static_cast<usize>(window) % history.size()];
        pool.chests.saveLocal(saved);
        const s32 distance = window % 8 + 1;
        std::vector<Chests::LocalSnapshot> nativeStates(static_cast<usize>(distance));
        std::vector<ChestsCheckpoint::State> states;
        std::vector<std::vector<ChestEvent>> events;
        for (s32 tick = window; tick < window + distance; ++tick) {
            events.push_back(step(tick, false));
            states.push_back(pool.state());
            pool.chests.saveLocal(nativeStates[static_cast<usize>(tick - window)]);
        }
        REQUIRE(pool.chests.restoreLocal(saved));
        for (s32 tick = window; tick < window + distance; ++tick) {
            (void)step(tick, true);
        }
        REQUIRE(pool.chests.restoreLocal(saved));
        for (s32 tick = window; tick < window + distance; ++tick) {
            const auto at = static_cast<usize>(tick - window);
            checkEvents(step(tick, false), events[at]);
            CHECK(pool.state() == states[at]);
            Chests::LocalSnapshot actual;
            pool.chests.saveLocal(actual);
            CHECK(pool.chests.equalLocal(actual, nativeStates[at]));
        }
    }
    CHECK(pool.device.textureUpdates == 0);
    auto diagnostic = pool.owner.prepareRestore(pool.state());
    REQUIRE(diagnostic);
    pool.chests.saveLocal(initial);
    REQUIRE(pool.chests.restoreLocal(initial));
    CHECK_FALSE(pool.owner.canCommit(*diagnostic));
    Chests other;
    CHECK_FALSE(other.restoreLocal(initial));
    CHECK_FALSE(other.equalLocal(initial, initial));
    pool.chests.clear();
    CHECK_FALSE(pool.chests.restoreLocal(initial));
    CHECK_FALSE(pool.chests.equalLocal(initial, initial));
}

TEST_CASE("native chest equality detects hidden pickup ownership and refusal clocks",
          "[local-rewind][chest-local-rewind]") {
    Fixture fixture("chest-local-hidden-equality");
    Chests::LocalSnapshot initial, changed, actual, empty;
    fixture.chests.saveLocal(initial);
    REQUIRE(fixture.chests.equalLocal(initial, initial));
    CHECK_FALSE(fixture.chests.equalLocal(initial, empty));
    CHECK_FALSE(fixture.chests.equalLocal(empty, empty));

    const auto first = fixture.chests.chest(0);
    fixture.chests.hold(0, 91);
    fixture.chests.saveLocal(changed);
    REQUIRE(fixture.chests.chest(0).figure == first.figure);
    REQUIRE(fixture.chests.chest(0).box == first.box);
    CHECK_FALSE(fixture.chests.equalLocal(initial, changed));
    CHECK_FALSE(fixture.chests.equalLocal(changed, initial));
    REQUIRE(fixture.chests.restoreLocal(initial));
    fixture.chests.saveLocal(actual);
    CHECK(fixture.chests.equalLocal(initial, actual));

    // Equal model clocks and collision do not imply equal refusal cooldowns.
    REQUIRE(fixture.chests.update(0, {}).empty());
    fixture.chests.saveLocal(initial);
    const auto before = fixture.chests.chest(0);
    const std::array party{ChestVisitor{Vec3{0}, 1, 0}};
    const auto events = fixture.chests.update(0, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events.front().kind == ChestEvent::Kind::Refused);
    fixture.chests.saveLocal(changed);
    CHECK(fixture.chests.chest(0).figure == before.figure);
    CHECK(fixture.chests.chest(0).box == before.box);
    CHECK_FALSE(fixture.chests.equalLocal(initial, changed));
    REQUIRE(fixture.chests.restoreLocal(initial));
    fixture.chests.saveLocal(actual);
    CHECK(fixture.chests.equalLocal(initial, actual));
    checkEvents(fixture.chests.update(0, party), events);
    fixture.chests.saveLocal(actual);
    CHECK(fixture.chests.equalLocal(changed, actual));

    REQUIRE(
        fixture.chests.bind(fixture.device, fixture.layout, AssetScope(fixture.items), nullptr));
    fixture.chests.saveLocal(actual);
    CHECK_FALSE(fixture.chests.equalLocal(initial, actual));
    CHECK_FALSE(fixture.chests.equalLocal(initial, initial));
}

TEST_CASE("container snapshots resume complete native opening gold and random-choice state",
          "[chests-checkpoint]") {
    Fixture host("chests-checkpoint-host");
    Fixture guest("chests-checkpoint-guest");
    host.reveal();
    guest.reveal();
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto hostUploads = host.device.texturesCreated;
    const auto guestUploads = guest.device.texturesCreated;
    bool refused = false;
    bool unlocked = false;
    bool opened = false;
    bool collected = false;
    bool exploded = false;
    bool emptyOpenPose = false;
    for (s32 tick = 0; tick < 180; ++tick) {
        const s32 phase = std::min(tick / 40, 3);
        const std::array party{
            ChestVisitor{Vec3{static_cast<f32>(phase * 20), 0, 0}, 1, tick % 40 < 4 ? 0 : 1}};
        if (tick == 120) {
            REQUIRE(host.chests.transmute(3, 4, 31));
        }
        const auto before = host.state();
        REQUIRE(guest.owner.restore(before));
        auto rewind = host.owner.prepareRestore(before);
        REQUIRE(rewind);
        host.chests.capturePresentation();
        const auto events = host.chests.update(seconds, party);
        const auto expected = host.state();
        for (const auto& event : events) {
            refused |= event.kind == ChestEvent::Kind::Refused;
            unlocked |= event.kind == ChestEvent::Kind::Unlocked;
            opened |= event.kind == ChestEvent::Kind::Opened;
            collected |= event.kind == ChestEvent::Kind::Collected;
            exploded |= event.explodes;
        }
        emptyOpenPose |= expected.chests[0].figure.state.holdPose;
        guest.chests.capturePresentation();
        checkEvents(guest.chests.update(seconds, party), events);
        CHECK(guest.state() == expected);
        REQUIRE(host.owner.canCommit(*rewind));
        REQUIRE(host.owner.commitRestore(std::move(*rewind)));
        CHECK(host.state() == before);
        host.chests.capturePresentation();
        checkEvents(host.chests.update(seconds, party), events);
        CHECK(host.state() == expected);
    }
    CHECK(refused);
    CHECK(unlocked);
    CHECK(opened);
    CHECK(collected);
    CHECK(exploded);
    CHECK(emptyOpenPose);
    CHECK(host.state().seed > 0);
    CHECK(host.device.texturesCreated == hostUploads);
    CHECK(guest.device.texturesCreated == guestUploads);

    const auto before = host.state();
    auto prepared = host.owner.prepareRestore(before);
    REQUIRE(prepared);
    host.chests.hold(0, 91);
    host.chests.remove(2);
    host.chests.setPlayerCount(1);
    REQUIRE(host.owner.commitRestore(std::move(*prepared)));
    CHECK(host.state() == before);
}

TEST_CASE("native chest rewind keeps bound definitions but restores choices and their seed",
          "[local-rewind][chest-local-rewind]") {
    Fixture fixture("chest-local-bound-definitions");
    const auto before = fixture.state();
    Chests::LocalSnapshot saved;
    fixture.chests.saveLocal(saved);
    const std::array party{ChestVisitor{Vec3{0}, 1, 1}};
    const auto expectedEvents = fixture.chests.update(1.0f / 60, party);
    REQUIRE(expectedEvents.size() == 1);
    REQUIRE(expectedEvents.front().kind == ChestEvent::Kind::Unlocked);
    REQUIRE(expectedEvents.front().contents >= 4);
    REQUIRE(expectedEvents.front().contents <= 5);
    const auto expected = fixture.state();
    REQUIRE(expected.seed != before.seed);

    REQUIRE(fixture.chests.restoreLocal(saved));
    REQUIRE(fixture.chests.transmute(0, 5, 10));
    (void)fixture.chests.update(1.0f / 60, party);
    const auto uploads = fixture.device.texturesCreated;
    REQUIRE(fixture.chests.restoreLocal(saved));
    CHECK(fixture.state() == before);
    checkEvents(fixture.chests.update(1.0f / 60, party), expectedEvents);
    CHECK(fixture.state() == expected);
    CHECK(fixture.device.texturesCreated == uploads);

    // Rebinding even the same definition table starts a new owner epoch. The
    // table stays owned by the fixture, never by an old native history slot.
    REQUIRE(
        fixture.chests.bind(fixture.device, fixture.layout, AssetScope(fixture.items), nullptr));
    fixture.chests.setPlayerCount(2);
    CHECK_FALSE(fixture.chests.restoreLocal(saved));
    const auto rebound = fixture.state();
    CHECK(rebound.infos.size() == before.infos.size());
    CHECK(rebound.infos[3].choices == before.infos[3].choices);
    checkEvents(fixture.chests.update(1.0f / 60, party), expectedEvents);
    fixture.chests.saveLocal(saved);
    fixture.chests.clear();
    CHECK_FALSE(fixture.chests.restoreLocal(saved));
    CHECK(fixture.state().infos.empty());
}

TEST_CASE("unbound container art and previews retain exact moving-floor presentation state",
          "[chests-checkpoint]") {
    const auto directory = test::scratchDirectory("chests-checkpoint-empty-rider");
    writeTextFile(directory / "world.json", R"({"objects":[
      {"name":"ROOT","position":[100,10,50],"child":1},
      {"name":"LIFT","position":[0,0,0],"flags":4100}],
      "itemInfos":[{"type":2,"subtype":46,"name":"MISSING","radius":1,"height":2}],
      "itemInstances":[{"info":0,"position":[101,10,50],"params":[255,255,0,0,1,0,0,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    WorldCollision collision;
    collision.build({{{0, 1, 0}, {Vec3{-4, 0, -4}, Vec3{4, 0, -4}, Vec3{0, 0, 4}}, 1, 4100}});
    collision.setMovingObjects(std::array<s32, 1>{1});
    collision.setObjectTransform(1, glm::translate(Mat4{1}, Vec3{100, 20, 50}));
    test::FakeRenderDevice device;
    ItemArchive empty;
    Chests chests;
    REQUIRE(chests.bind(device, layout, AssetScope(empty), &collision));
    REQUIRE(chests.chest(0).floor);
    ChestsCheckpoint owner(chests);
    chests.capturePresentation();
    const auto before = owner.checkpoint();
    REQUIRE(before);
    REQUIRE_FALSE(before->chests[0].figure.bound);
    REQUIRE_FALSE(before->chests[0].preview.bound);
    CHECK(before->chests[0].preview.state.transform == before->chests[0].figure.state.transform);
    auto rewind = owner.prepareRestore(*before);
    REQUIRE(rewind);
    const Mat4 moved =
        glm::translate(Mat4{1}, Vec3{100, -10, 50}) * glm::rotate(Mat4{1}, 0.7f, Vec3{0, 1, 0});
    collision.setObjectTransform(1, moved);
    chests.syncFloors();
    chests.capturePresentation();
    const auto movedState = owner.checkpoint();
    REQUIRE(movedState);
    CHECK(movedState != before);
    REQUIRE(owner.commitRestore(std::move(*rewind)));
    CHECK(owner.checkpoint() == before);
    // The support owner is external: undoing a chest must not move that floor.
    CHECK(collision.objectTransform(1) == moved);
    chests.syncFloors();
    chests.capturePresentation();
    CHECK(owner.checkpoint() == movedState);
    CHECK(device.texturesCreated == 0);
}

TEST_CASE("container plans reject foreign rosters resources and stale sibling restoration",
          "[chests-checkpoint]") {
    Fixture fixture("chests-checkpoint-fences");
    Fixture other("chests-checkpoint-foreign");
    const auto state = fixture.state();
    auto first = fixture.owner.prepareRestore(state);
    auto sibling = fixture.owner.prepareRestore(state);
    REQUIRE(first);
    REQUIRE(sibling);
    CHECK_FALSE(other.owner.commitRestore(std::move(*first)));
    REQUIRE(fixture.owner.canCommit(*first));
    REQUIRE(fixture.owner.commitRestore(std::move(*first)));
    CHECK_FALSE(fixture.owner.canCommit(*sibling));
    first = fixture.owner.prepareRestore(state);
    REQUIRE(first);
    ChestsCheckpoint secondWrapper(fixture.chests);
    REQUIRE(secondWrapper.restore(state));
    CHECK_FALSE(fixture.owner.canCommit(*first));
    first = fixture.owner.prepareRestore(state);
    REQUIRE(first);
    fixture.chests.clear();
    CHECK_FALSE(fixture.owner.commitRestore(std::move(*first)));
}

TEST_CASE("container malformed state is rejected atomically without binding new art",
          "[chests-checkpoint]") {
    Fixture fixture("chests-checkpoint-invalid");
    const auto before = fixture.state();
    auto invalid = before;
    const auto variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
    switch (variant) {
    case 0: invalid.infos[3].choices = {5, 4}; break;
    case 1: invalid.chests[0].instance = 900; break;
    case 2: invalid.chests[0].contents = 999; break;
    case 3: invalid.chests[0].refusalLeft = std::numeric_limits<f32>::quiet_NaN(); break;
    case 4: invalid.chests[0].opener = 4; break;
    case 5: invalid.chests[0].figure.state.pose.tree = "OTHER"; break;
    case 6: invalid.chests[0].preview = invalid.chests[0].figure; break;
    case 7: invalid.chests[0].box.halfAcross = -1; break;
    case 8: invalid.chests[0].floor = ChestsCheckpoint::Floor{1, Mat4{1}}; break;
    case 9: invalid.chests[0].collectedSeconds = -1.0f; break;
    case 10: invalid.infos.resize(ChestsCheckpoint::State::kMaxInfos + 1); break;
    case 11: invalid.placements[0].position.x += 1; break;
    }
    const auto uploads = fixture.device.texturesCreated;
    CHECK_FALSE(fixture.owner.prepareRestore(invalid));
    CHECK_FALSE(fixture.owner.restore(invalid));
    CHECK(fixture.state() == before);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("native campaign containers retain full authored roster and preloaded figures",
          "[chests-checkpoint][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto name = GENERATE("G1", "G3", "G4");
    CAPTURE(name);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName(name)));
    Chests chests;
    REQUIRE(chests.bind(device, world.layout(), world.itemScope(), &world.collision()));
    ChestsCheckpoint owner(chests);
    const auto before = owner.checkpoint();
    REQUIRE(before);
    REQUIRE(before->chests.size() == chests.size());
    const auto uploads = device.texturesCreated;
    auto prepared = owner.prepareRestore(*before);
    REQUIRE(prepared);
    chests.setPlayerCount(4);
    chests.capturePresentation();
    chests.update(1.0f / 30, {});
    REQUIRE(owner.commitRestore(std::move(*prepared)));
    CHECK(owner.checkpoint() == before);
    CHECK(device.texturesCreated == uploads);
}
} // namespace
