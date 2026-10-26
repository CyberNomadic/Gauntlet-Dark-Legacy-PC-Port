#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/level/LevelWorld.h"
#include "game/level/PlacedItemsCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
static_assert(std::is_aggregate_v<PlacedItemsCheckpoint::State>);

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive archive;
    WorldCollision collision;
    PlacedItems items;
    PlacedItemsCheckpoint owner{items};

    explicit Fixture(std::string_view tag, bool moving = false) {
        const auto root = test::scratchDirectory(tag);
        writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "textures.json", R"({"bitmaps":[
          {"name":"WHITE","file":"white.png","width":2,"height":2},
          {"name":"FLAME0","file":"white.png","width":2,"height":2},
          {"name":"FLAME1","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({
          "particles":[{"preset":2,"enables":16929,"texture":"FLAME0",
            "particleLife":[0.1,0],"rate":[30,30,30,30]}],
          "textureAnimations":[{"name":"FLAME","texture":1,"source":1,"frames":2,"rate":1}],
          "trees":[
           {"name":"COIN","nodes":[
             {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
             {"name":"FIRE","particle":0,"parent":0,"position":[0,1,0]}],
            "sequences":[{"name":"IDLE","frames":4,"frameRate":30,
              "tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[0,3]}]}]},
           {"name":"GEMORANGE","nodes":[
             {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
           {"name":"TREAS_JUNK","nodes":[
             {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]},
           {"name":"GETGEMORANGE","nodes":[
             {"name":"SPARK","particle":0,"parent":-1,"position":[0,1,0]}],
            "sequences":[{"name":"BURST","frames":6,"frameRate":30}]}]})");
        writeTextFile(root / "world.json", R"({"objects":[{"name":"ROOT","position":[0,0,0]}],
          "itemInfos":[
            {"type":1,"subtype":1,"name":"COIN","value":500,"radius":1,"height":2,"armor":0,"collisionType":1},
            {"type":1,"subtype":15,"name":"GEMORANGE","value":4,"radius":1,"height":2}],
          "itemInstances":[{"info":0,"position":[0,0,0]},
            {"info":1,"position":[20,0,0]}, {"info":1,"position":[40,0,0],"minPlayers":2}]})");
        test::convertModelFixture(root);
        REQUIRE(layout.load(root));
        REQUIRE(archive.load(root));
        collision.build(
            {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}}, 0, 4}});
        if (moving) {
            collision.setMovingObjects(std::array<s32, 1>{0});
            collision.setObjectTransform(0, Mat4{1});
        }
        REQUIRE(items.bind(device, layout, &collision, std::array{&archive}));
        items.setPlayerCount(2);
        REQUIRE_FALSE(owner.checkpoint());
        REQUIRE(owner.bindResources(device));
    }
    PlacedItemsCheckpoint::State state() const {
        const auto saved = owner.checkpoint();
        REQUIRE(saved);
        REQUIRE(saved->valid());
        return *saved;
    }
    PlacedItemsCheckpoint::State nativeState() const {
        auto saved = state();
        // The independent diagnostic also preserves the last draw's caches.
        // Native history intentionally derives these again on the next draw.
        for (auto& item : saved.items) {
            item.presentationPose.reset();
        }
        return saved;
    }
    void takeGem(usize index) {
        const auto result =
            items.collect(device, std::array{Collector{items.item(index).position, 1, 1}});
        REQUIRE(result.size() == 1);
        REQUIRE(result.front().item == index);
    }
};

TEST_CASE("native pickup history restores corrected births flight rewards and moving floors",
          "[local-rewind][placed-items-local-rewind]") {
    Fixture native("pickup-local-native", true);
    Fixture replay("pickup-local-replay", true);
    std::array<PlacedItems::LocalSnapshot, 16> history;
    std::array<PlacedItems::LocalSnapshot, 8> expected;
    PlacedItems::LocalSnapshot observed;
    WorldCollision::LocalSnapshot floor;
    const auto step = [](Fixture& fixture, u32 tick, bool wrong) {
        fixture.items.capturePresentation();
        fixture.collision.setObjectTransform(
            0, glm::translate(Mat4{1},
                              Vec3{0, wrong ? -20.0f : static_cast<f32>(tick % 20) * 0.2f, 0}));
        fixture.items.setPlayerCount(wrong ? 1 : 2);
        if (tick % 17 == 0 || wrong) {
            REQUIRE(fixture.items.throwItem(fixture.device, "COIN", {5, 8, 0},
                                            wrong ? Vec3{-20, 5, 0} : Vec3{2, 8, 0},
                                            &fixture.collision, 0.3f));
        }
        if (tick % 29 == 0) {
            fixture.items.hideCrystals();
        }
        fixture.items.reveal(1.0f / 60);
        fixture.items.attach(0, glm::translate(Mat4{1}, Vec3{0, 2, 0}), tick % 7 < 3);
        fixture.items.setOpener(0, static_cast<s32>(tick % 4));
        if (tick % 23 == 0 || wrong) {
            fixture.items.blast(fixture.device, Vec3{5, 8, 0}, 30, 100);
        }
        if (tick % 31 == 0) {
            if (const auto item = fixture.items.claim({5, 8, 0}, 12, 16)) {
                REQUIRE(
                    fixture.items.release(*item, {3, 7, 0}, {2, 3, 0}, &fixture.collision, 0.1f));
            }
        }
        if (wrong) {
            fixture.items.discard(0);
        }
        const auto pickups = fixture.items.collect(
            fixture.device, std::array{Collector{Vec3{tick < 80 ? 20.0f : 40.0f, 0, 0}, 2, 3}});
        fixture.items.update(1.0f / 60);
        // Drawing is interleaved with simulation to expose stale shared caches.
        fixture.device.draws.clear();
        fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.37f);
        return pickups;
    };
    u32 tick = 0;
    const auto uploads = replay.device.texturesCreated;
    for (u32 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        auto& saved = history[window % history.size()];
        replay.items.saveLocal(saved);
        replay.collision.saveLocal(floor);
        const u32 distance = window % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, false);
            replay.items.saveLocal(expected[i]);
        }
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.items.restoreLocal(saved));
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, true);
        }
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.items.restoreLocal(saved));
        CHECK(replay.nativeState() == native.nativeState());
        for (u32 i = 0; i < distance; ++i) {
            const auto a = step(native, tick + i, false);
            const auto b = step(replay, tick + i, false);
            replay.items.saveLocal(observed);
            CHECK(replay.items.equalLocal(observed, expected[i]));
            REQUIRE(a.size() == b.size());
            for (usize j = 0; j < a.size(); ++j) {
                CHECK(a[j].item == b[j].item);
                CHECK(a[j].collector == b[j].collector);
                CHECK(a[j].amount == b[j].amount);
                CHECK(a[j].position == b[j].position);
            }
            // Hidden/retired items do not redraw their derived pose cache. A
            // discarded speculative draw may therefore leave different cache
            // contents, but not different continuation values or visible draws.
            // Complete same-owner native equality is checked above; compare
            // the independent diagnostic without its stale draw-only caches.
            CHECK(replay.nativeState() == native.nativeState());
            REQUIRE(replay.device.draws.size() == native.device.draws.size());
            for (usize j = 0; j < native.device.draws.size(); ++j) {
                test::checkRecordedDraw(replay.device.draws[j], native.device.draws[j]);
            }
        }
        tick += distance;
    }
    CHECK(replay.device.texturesCreated == uploads);
    CHECK(replay.device.textureUpdates == 0);
    CHECK_FALSE(native.items.restoreLocal(history[0]));
    CHECK_FALSE(native.items.equalLocal(history[0], history[0]));
    replay.items.clear();
    CHECK_FALSE(replay.items.restoreLocal(history[0]));
    CHECK_FALSE(replay.items.equalLocal(history[0], history[0]));
}

TEST_CASE(
    "pickup rewind derives interpolated poses after corrected births without saving draw caches",
    "[local-rewind][placed-items-local-rewind]") {
    Fixture fixture("pickup-local-draw-cache");
    fixture.items.capturePresentation();
    fixture.items.update(1.0f / 60);
    PlacedItems::LocalSnapshot beforeDraw;
    fixture.items.saveLocal(beforeDraw);
    const auto native = fixture.nativeState();
    fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.37f);
    const auto expected = fixture.device.draws;
    REQUIRE_FALSE(expected.empty());
    PlacedItems::LocalSnapshot afterDraw;
    fixture.items.saveLocal(afterDraw);
    CHECK(fixture.items.equalLocal(beforeDraw, afterDraw));
    CHECK(fixture.nativeState() == native);

    for (const auto* saved : {&beforeDraw, &afterDraw}) {
        REQUIRE(fixture.items.throwItem(fixture.device, "COIN", {8, 10, 0}, {2, 4, 0},
                                        &fixture.collision, 0.5f));
        fixture.items.capturePresentation();
        fixture.items.update(7.0f / 60);
        fixture.device.draws.clear();
        fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.91f);
        const auto uploads = fixture.device.texturesCreated;
        REQUIRE(fixture.items.restoreLocal(*saved));
        CHECK(fixture.nativeState() == native);
        fixture.device.draws.clear();
        fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.37f);
        REQUIRE(fixture.device.draws.size() == expected.size());
        for (usize i = 0; i < expected.size(); ++i) {
            test::checkRecordedDraw(fixture.device.draws[i], expected[i]);
        }
        CHECK(fixture.device.texturesCreated == uploads);
    }
}

TEST_CASE("native pickup history refuses a changed collision owner before mutating values",
          "[local-rewind][placed-items-local-rewind]") {
    Fixture fixture("pickup-local-collision-guard");
    PlacedItems::LocalSnapshot saved;
    fixture.items.saveLocal(saved);
    WorldCollision other;
    other.build({{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}}, 0, 4}});
    REQUIRE(fixture.items.throwItem(fixture.device, "COIN", {5, 8, 0}, {2, 4, 0}, &other, 0.5f));
    const auto before = fixture.items.size();
    CHECK_FALSE(fixture.items.canRestoreLocal(saved));
    CHECK_FALSE(fixture.items.restoreLocal(saved));
    CHECK(fixture.items.size() == before);
}

TEST_CASE("native pickup equality distinguishes loot ownership without visible movement",
          "[local-rewind][placed-items-local-rewind]") {
    Fixture fixture("pickup-local-owner-values");
    PlacedItems::LocalSnapshot before;
    PlacedItems::LocalSnapshot after;
    PlacedItems::LocalSnapshot restored;
    fixture.items.saveLocal(before);
    const auto position = fixture.items.item(0).position;
    const auto value = fixture.items.item(0).value;
    fixture.items.setOpener(0, 3);
    fixture.items.saveLocal(after);
    CHECK(fixture.items.item(0).position == position);
    CHECK(fixture.items.item(0).value == value);
    CHECK_FALSE(fixture.items.equalLocal(before, after));
    REQUIRE(fixture.items.restoreLocal(before));
    fixture.items.saveLocal(restored);
    CHECK(fixture.items.equalLocal(before, restored));
}

TEST_CASE("pickup checkpoints continue native flight reveal animation particles and collection at "
          "both rates",
          "[placed-items-checkpoint]") {
    Fixture host("pickup-checkpoint-native-host");
    Fixture guest("pickup-checkpoint-native-guest");
    const s32 rate = GENERATE(30, 60);
    const f32 step = 1.0f / static_cast<f32>(rate);
    host.items.hideCrystals();
    REQUIRE(host.items.throwItem(host.device, "COIN", {5, 8, 0}, {7, 3, 0}, &host.collision, 0.75f,
                                 42));
    host.items.attach(0, glm::translate(Mat4{1}, Vec3{0, 2, 0}), true);
    host.items.setOpener(0, 3);
    auto seed = host.state();
    REQUIRE(seed.items.size() == 4);
    REQUIRE(seed.items.back().thrown);
    REQUIRE(guest.owner.restore(seed));
    const auto uploads = guest.device.texturesCreated;
    bool flew = false;
    bool landed = false;
    bool burst = false;
    for (s32 tick = 0; tick < rate * 3; ++tick) {
        if (tick % 11 == 0) {
            REQUIRE(guest.owner.restore(host.state()));
        }
        for (auto* fixture : {&host, &guest}) {
            fixture->items.capturePresentation();
            fixture->items.reveal(step);
            if (tick == rate / 2) {
                fixture->items.attach(0, glm::translate(Mat4{1}, Vec3{0, 1, 0}), false);
            }
            if (tick == rate) {
                fixture->takeGem(1);
            }
            fixture->items.update(step);
        }
        const auto expected = host.state();
        CHECK(guest.state() == expected);
        flew |= expected.items.back().thrown;
        landed |= !expected.items.back().thrown;
        burst |= !expected.effects.empty();
        if (tick % 13 == 0) {
            host.items.draw(host.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.5f);
            guest.items.draw(guest.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.5f);
            CHECK(guest.state() == host.state());
        }
    }
    CHECK(flew);
    CHECK(landed);
    CHECK(burst);
    CHECK(guest.device.texturesCreated == uploads);
    CHECK(guest.items.item(0).archive == &guest.archive);
    CHECK(guest.items.item(0).archive != host.items.item(0).archive);
}

TEST_CASE(
    "prepared pickup undo restores dynamic append replacement and bursts without resource creation",
    "[placed-items-checkpoint]") {
    Fixture fixture("pickup-checkpoint-undo");
    fixture.items.update(1.0f / 30);
    fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.5f);
    const auto baseline = fixture.state();
    auto undo = fixture.owner.prepareRestore(baseline);
    REQUIRE(undo);
    const auto uploads = fixture.device.texturesCreated;
    REQUIRE(fixture.items.throwItem(fixture.device, "COIN", {4, 5, 0}, {1, 2, 3},
                                    &fixture.collision, 0.4f));
    REQUIRE(fixture.items.blast(fixture.device, fixture.items.item(0).position, 2, 100).size() ==
            1);
    REQUIRE(fixture.items.item(0).name == "TREAS_JUNK");
    fixture.takeGem(1);
    fixture.items.update(1.0f / 30);
    const auto changed = fixture.state();
    REQUIRE(changed != baseline);
    REQUIRE(changed.items[0].presentationPose); // cached old COIN pose survives native replacement
    REQUIRE(changed.items[0].presentationPose->resource != changed.items[0].resource);
    CHECK(changed.figures[changed.items[0].presentationPose->resource].name == "COIN");
    CHECK(changed.figures[changed.items[0].resource].name == "TREAS_JUNK");
    CHECK(changed.items[0].presentationPose == baseline.items[0].presentationPose);
    REQUIRE(fixture.owner.canCommit(*undo));
    REQUIRE(fixture.owner.commitRestore(std::move(*undo)));
    CHECK(fixture.state() == baseline);
    REQUIRE(fixture.owner.restore(changed));
    CHECK(fixture.state() == changed);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("pickup catalog captures dormant replacements and all retained burst field entries",
          "[placed-items-checkpoint]") {
    Fixture host("pickup-checkpoint-burst-host");
    Fixture guest("pickup-checkpoint-burst-guest");
    const auto initial = host.state();
    REQUIRE(std::ranges::any_of(
        initial.figures, [](const auto& resource) { return resource.name == "TREAS_JUNK"; }));
    REQUIRE(std::ranges::none_of(initial.items, [&](const auto& item) {
        return initial.figures[item.resource].name == "TREAS_JUNK";
    }));
    host.takeGem(1);
    for (s32 tick = 0; tick < 5; ++tick) {
        host.items.update(1.0f / 30);
    }
    host.takeGem(2);
    bool retained = false;
    for (s32 tick = 0; tick < 30; ++tick) {
        const auto saved = host.state();
        usize linked = 0;
        for (const auto& effect : saved.effects) {
            linked += effect.emitters.size();
        }
        retained |= saved.particles.entries.size() > linked;
        REQUIRE(guest.owner.restore(saved));
        CHECK(guest.state() == saved);
        host.items.update(1.0f / 30);
        guest.items.update(1.0f / 30);
        CHECK(guest.state() == host.state());
    }
    CHECK(retained);
    CHECK(host.items.effectCount() == 0);
    CHECK(host.items.bursts().size() == 0);
}

TEST_CASE(
    "pickup restoration preserves carried state floor-local attachment and pending presentation",
    "[placed-items-checkpoint]") {
    Fixture fixture("pickup-checkpoint-support", true);
    REQUIRE(fixture.items.item(0).floor);
    fixture.items.capturePresentation();
    const auto baseline = fixture.state();
    REQUIRE(baseline.capturePending);
    fixture.collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, 7, 0}));
    fixture.items.syncFloors();
    CHECK(fixture.state() != baseline);
    REQUIRE(fixture.owner.restore(baseline));
    CHECK(fixture.state() == baseline); // no implicit floor resnap during restore
    fixture.items.syncFloors();
    REQUIRE(fixture.items.claim(fixture.items.item(0).position, 1, 1) == 0);
    const auto carried = fixture.state();
    REQUIRE(carried.items[0].carried);
    REQUIRE_FALSE(carried.items[0].floor);
    REQUIRE(fixture.items.release(0, {0, 10, 0}, {2, 3, 4}, &fixture.collision, 0.5f));
    REQUIRE(fixture.owner.restore(carried));
    CHECK(fixture.state() == carried);
}

TEST_CASE("pickup preparations are atomic owner-bound single-use and invalidated by rebind",
          "[placed-items-checkpoint]") {
    Fixture first("pickup-checkpoint-owner-a");
    Fixture second("pickup-checkpoint-owner-b");
    const auto baseline = first.state();
    auto prepared = first.owner.prepareRestore(baseline);
    auto sibling = first.owner.prepareRestore(baseline);
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK_FALSE(second.owner.canCommit(*prepared));
    CHECK_FALSE(second.owner.commitRestore(std::move(*prepared)));
    CHECK(first.owner.canCommit(*prepared));
    REQUIRE(first.owner.commitRestore(std::move(*prepared)));
    CHECK_FALSE(first.owner.canCommit(*prepared));
    CHECK_FALSE(first.owner.canCommit(*sibling));
    prepared = first.owner.prepareRestore(baseline);
    REQUIRE(prepared);
    first.owner.clearResources();
    CHECK_FALSE(first.owner.canCommit(*prepared));
    REQUIRE(first.owner.bindResources(first.device));
    CHECK_FALSE(first.owner.canCommit(*prepared));
    prepared = first.owner.prepareRestore(baseline);
    REQUIRE(prepared);
    first.items.clear();
    CHECK_FALSE(first.owner.canCommit(*prepared));
    REQUIRE_FALSE(first.owner.checkpoint());
    CHECK(second.state() == baseline);
}

TEST_CASE("malformed complete pickup states never partially mutate the owner",
          "[placed-items-checkpoint]") {
    Fixture fixture("pickup-checkpoint-malformed");
    fixture.takeGem(1);
    fixture.items.update(1.0f / 30);
    const auto baseline = fixture.state();
    auto malformed = baseline;
    const s32 mutation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13);
    switch (mutation) {
    case 0: malformed.items[0].velocity.x = std::numeric_limits<f32>::infinity(); break;
    case 1: malformed.items[0].resource = std::numeric_limits<u32>::max(); break;
    case 2: malformed.items[0].animation.sequence = 999; break;
    case 3: malformed.items[0].pose.nodes[0].name = "FOREIGN"; break;
    case 4: malformed.items[0].particles.field.entries[0].emitter.descriptor.rate[0] += 1; break;
    case 5: malformed.definitions[0].value += 1; break;
    case 6: malformed.placements[0].position.x += 1; break;
    case 7: malformed.items.erase(malformed.items.begin()); break;
    case 8: malformed.effects[0].emitters[0].second = 999; break;
    case 9: malformed.particleResources[0] = std::numeric_limits<u32>::max(); break;
    case 10: malformed.particles.entries[0].emitter.descriptor.texture = "FOREIGN"; break;
    case 11: malformed.effects.clear(); break; // active orphan is not a native retired entry
    case 12: malformed.textures[0].remainder = 1; break;
    case 13: malformed.figures[0].archive = 1; break;
    default: FAIL("missing mutation");
    }
    CAPTURE(mutation);
    CHECK_FALSE(fixture.owner.prepareRestore(malformed));
    CHECK_FALSE(fixture.owner.restore(malformed));
    CHECK(fixture.state() == baseline);
}

TEST_CASE("pickup checkpoint limits refuse the complete seed without truncation",
          "[placed-items-checkpoint]") {
    Fixture fixture("pickup-checkpoint-bounds");
    auto state = fixture.state();
    state.items.resize(PlacedItemsCheckpoint::State::kMaxItems + 1, state.items.back());
    CHECK_FALSE(state.valid());
    state = fixture.state();
    state.figures.resize(PlacedItemsCheckpoint::State::kMaxResources + 1);
    CHECK_FALSE(state.valid());
    state = fixture.state();
    state.definitions.resize(PlacedItemsCheckpoint::State::kMaxItems + 1);
    CHECK_FALSE(state.valid());
    CHECK_FALSE(fixture.owner.restore(state));
}

TEST_CASE("pickup capture retains full public identity and live mutable validation",
          "[placed-items-checkpoint][pickup-capture-validation]") {
    Fixture fixture("pickup-capture-validation");
    fixture.items.update(1.0f / 30);
    fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.5f);
    const auto baseline = fixture.state();
    const auto nan = std::numeric_limits<f32>::quiet_NaN();
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    for (s32 mutation = 0; mutation < 9; ++mutation) {
        CAPTURE(mutation);
        auto malformed = baseline;
        switch (mutation) {
        case 0: malformed.definitions[0].radius = nan; break;
        case 1: malformed.placements[0].position.x = nan; break;
        case 2:
            malformed.placements[0].collision.emplace_back();
            malformed.placements[0].collision.back().normal.x = nan;
            break;
        case 3: malformed.figures[0].name.clear(); break;
        case 4:
            REQUIRE_FALSE(malformed.bursts.empty());
            malformed.bursts[0].archive = static_cast<u32>(malformed.archives.size());
            break;
        case 5: malformed.archives.clear(); break;
        case 6: malformed.layout = false; break;
        case 7: malformed.figures.push_back(malformed.figures.front()); break;
        case 8: malformed.items[0].velocity.x = nan; break;
        default: break;
        }
        CHECK_FALSE(malformed.valid());
        CHECK_FALSE(fixture.owner.prepareRestore(malformed));
        CHECK_FALSE(fixture.owner.restore(malformed));
        CHECK(fixture.state() == baseline);
    }

    // Capture may trust only its immutable catalog, never mutable native rows.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& item = const_cast<PlacedItems::Item&>(fixture.items.item(0));
    const auto velocity = std::exchange(item.velocity.x, nan);
    CHECK_FALSE(fixture.owner.checkpoint());
    item.velocity.x = velocity;
    const auto subtype = std::exchange(item.subtype, item.subtype + 1);
    CHECK_FALSE(fixture.owner.checkpoint());
    item.subtype = subtype;
    CHECK(fixture.state() == baseline);
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
}

TEST_CASE("pickup presentation candidate filtering retains topology and composed matrix checks",
          "[placed-items-checkpoint][pickup-capture-validation]") {
    Fixture fixture("pickup-presentation-candidate-validation");
    fixture.items.update(1.0f / 30);
    fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.5f);
    const auto baseline = fixture.state();
    const auto& item = fixture.items.item(0);
    REQUIRE(item.figure);
    REQUIRE(baseline.items[0].presentationPose);
    REQUIRE(fixture.items.presentationPoseOf(0));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& presentation = *const_cast<TreePose*>(fixture.items.presentationPoseOf(0));
    REQUIRE(presentation.size() == 2);
    auto changedTree = *item.figure;
    const s32 mutation = GENERATE(0, 1, 2, 3);
    switch (mutation) {
    case 0:
        changedTree.name = "FOREIGN";
        presentation.rest(changedTree);
        break;
    case 1:
        changedTree.nodes.pop_back();
        presentation.rest(changedTree);
        break;
    case 2:
        changedTree.nodes[1].position.x += 1;
        presentation.rest(changedTree);
        break;
    case 3:
        // Each local scalar remains valid, but the child inherits a 4,000,000
        // scale. A name/count-only acceptance would silently lose this guard.
        for (usize node = 0; node < 2; ++node) {
            auto pose = presentation.poses()[node];
            pose.scale = Vec3{2000};
            presentation.setNodePose(node, pose);
        }
        CHECK(presentation.matrices()[1][0][0] > TreePose::Checkpoint::kMaxMagnitude);
        break;
    default: break;
    }
    CAPTURE(mutation);
    const auto captured = presentation.checkpoint();
    REQUIRE(captured);
    REQUIRE(captured->valid());
    TreePose independent;
    CHECK_FALSE(independent.restore(*item.figure, *captured));
    CHECK_FALSE(fixture.owner.checkpoint());
    auto scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(fixture.owner.checkpoint(std::move(scratch)));

    auto malformed = baseline;
    malformed.items[0].presentationPose->pose = *captured;
    REQUIRE(malformed.valid()); // Resource-bound restore supplies the stronger checks.
    CHECK_FALSE(fixture.owner.prepareRestore(malformed));
    REQUIRE(fixture.owner.restore(baseline));
    CHECK(fixture.state() == baseline);
}

TEST_CASE("pickup capture consumes catalog storage while recapturing all native changes",
          "[placed-items-checkpoint][pickup-capture-reuse]") {
    Fixture fixture("pickup-capture-reuse");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    fixture.items.update(seconds);
    fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.5f);
    const auto retained = fixture.state();
    auto scratch = retained;
    const auto* definitions = scratch.definitions.data();
    const auto* placements = scratch.placements.data();
    const auto* figures = scratch.figures.data();
    const auto* bursts = scratch.bursts.data();
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    bool activeBurst = false;
    bool retiredBurst = false;
    for (s32 tick = 0; tick < 90; ++tick) {
        CAPTURE(tick, seconds);
        fixture.items.capturePresentation();
        if (tick == 1) {
            REQUIRE(fixture.items.throwItem(fixture.device, "COIN", {4, 5, 0}, {1, 2, 3},
                                            &fixture.collision, 0.4f));
        }
        if (tick == 3) {
            REQUIRE(fixture.items.blast(fixture.device, fixture.items.item(0).position, 2, 100)
                        .size() == 1);
        }
        if (tick == 5) {
            fixture.takeGem(1);
        }
        fixture.items.update(seconds);
        const auto expected = fixture.state();
        auto next = fixture.owner.checkpoint(std::move(scratch));
        REQUIRE(next);
        CHECK(next->valid());
        CHECK(*next == expected);
        CHECK(next->definitions.data() == definitions);
        CHECK(next->placements.data() == placements);
        CHECK(next->figures.data() == figures);
        CHECK(next->bursts.data() == bursts);
        if (tick >= 3) {
            REQUIRE(next->items[0].presentationPose);
            CHECK(next->items[0].presentationPose == retained.items[0].presentationPose);
            CHECK(next->items[0].presentationPose->resource != next->items[0].resource);
        }
        activeBurst |= !next->effects.empty();
        retiredBurst |= activeBurst && next->effects.empty();
        scratch = std::move(*next);
    }
    CHECK(activeBurst);
    CHECK(retiredBurst);
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
    // Separate published value images and preallocated undo remain independent.
    REQUIRE(fixture.owner.restore(retained));
    CHECK(fixture.state() == retained);
}

TEST_CASE("pickup scratch capture authenticates exact identity but never trusts old live values",
          "[placed-items-checkpoint][pickup-capture-reuse]") {
    Fixture fixture("pickup-capture-reuse-validation");
    const auto baseline = fixture.state();
    const auto nan = std::numeric_limits<f32>::quiet_NaN();
    for (s32 mutation = 0; mutation < 9; ++mutation) {
        CAPTURE(mutation);
        auto scratch = baseline;
        switch (mutation) {
        case 0: scratch.definitions[0].value += 1; break;
        case 1: scratch.definitions[0].collisionOffset.x = -0.0f; break;
        case 2: scratch.definitions[0].choices.push_back(1); break;
        case 3: scratch.placements[0].position.x = -0.0f; break;
        case 4: scratch.placements[0].collision.emplace_back(); break;
        case 5: scratch.figures[0].name += "_FOREIGN"; break;
        case 6: scratch.bursts.clear(); break;
        case 7: scratch.archives[0] = false; break;
        case 8: scratch.layout = false; break;
        default: break;
        }
        // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
        CHECK_FALSE(fixture.owner.checkpoint(std::move(scratch)));
        CHECK(fixture.state() == baseline);
    }

    auto scratch = baseline;
    scratch.items[0].velocity.x = nan;
    scratch.items[0].resource = std::numeric_limits<u32>::max();
    scratch.effects.emplace_back();
    scratch.particles.entries.emplace_back();
    scratch.particleResources.push_back(std::numeric_limits<u32>::max());
    scratch.textures.clear();
    scratch.players = -1;
    scratch.revealTime = nan;
    scratch.revealing = !baseline.revealing;
    scratch.capturePending = !baseline.capturePending;
    scratch.burstsAdvanced = !baseline.burstsAdvanced;
    scratch.collision = !baseline.collision;
    CHECK_FALSE(scratch.valid());
    CHECK_FALSE(fixture.owner.prepareRestore(scratch));
    auto fresh = fixture.owner.checkpoint(std::move(scratch));
    REQUIRE(fresh);
    CHECK(*fresh == baseline);
    CHECK(fixture.state() == baseline);

    // Invalid current state cannot be repaired by valid stale scratch.
    auto undo = fixture.owner.prepareRestore(baseline);
    REQUIRE(undo);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& item = const_cast<PlacedItems::Item&>(fixture.items.item(0));
    item.velocity.x = nan;
    scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(fixture.owner.checkpoint(std::move(scratch)));
    CHECK_FALSE(fixture.owner.checkpoint());
    REQUIRE(fixture.owner.canCommit(*undo));
    REQUIRE(fixture.owner.commitRestore(std::move(*undo)));
    CHECK(fixture.state() == baseline);
}

TEST_CASE("pickup scratch requires current bindings and permits independently equal catalogs",
          "[placed-items-checkpoint][pickup-capture-reuse]") {
    Fixture first("pickup-capture-reuse-bind-a");
    const Fixture second("pickup-capture-reuse-bind-b");
    const auto baseline = first.state();
    auto scratch = baseline;
    auto independent = second.owner.checkpoint(std::move(scratch));
    REQUIRE(independent);
    CHECK(*independent == second.state());
    first.owner.clearResources();
    scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(first.owner.checkpoint(std::move(scratch)));
    REQUIRE(first.owner.bindResources(first.device));
    scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK(first.owner.checkpoint(std::move(scratch)) == std::optional{baseline});

    // A native rebind invalidates the old wrapper even when value rows agree.
    REQUIRE(
        first.items.bind(first.device, first.layout, &first.collision, std::array{&first.archive}));
    first.items.setPlayerCount(2);
    scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(first.owner.checkpoint(std::move(scratch)));
    REQUIRE(first.owner.bindResources(first.device));
    scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    REQUIRE(first.owner.checkpoint(std::move(scratch)));

    // Explicitly rebinding a changed authored catalog must not recycle old data.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& infos = const_cast<std::vector<ItemInfo>&>(first.layout.itemInfos());
    ++infos[0].value;
    REQUIRE(
        first.items.bind(first.device, first.layout, &first.collision, std::array{&first.archive}));
    first.items.setPlayerCount(2);
    REQUIRE(first.owner.bindResources(first.device));
    const auto changed = first.state();
    scratch = baseline;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(first.owner.checkpoint(std::move(scratch)));
    CHECK(first.state() == changed);
    scratch = changed;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK(first.owner.checkpoint(std::move(scratch)) == std::optional{changed});
}

TEST_CASE("pickup observation accepts only the authored multiple key record substitution",
          "[placed-items-checkpoint][pickup-keyring-observation][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG2/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G2");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    auto& items = world.placedItems();
    PlacedItemsCheckpoint owner{items};
    REQUIRE(owner.bindResources(device));
    const auto baseline = owner.checkpoint();
    REQUIRE(baseline);
    REQUIRE(baseline->valid());
    const auto converted = std::ranges::find_if(baseline->items, [&](const auto& item) {
        return item.instance >= 0 &&
               baseline->placements[static_cast<usize>(item.instance)].info != item.info;
    });
    REQUIRE(converted != baseline->items.end());
    const auto itemIndex = static_cast<usize>(converted - baseline->items.begin());
    const auto placementIndex = static_cast<usize>(converted->instance);
    const auto sourceRecord = baseline->placements[placementIndex].info;
    REQUIRE(baseline->definitions[static_cast<usize>(sourceRecord)].name == "KEY");
    REQUIRE(baseline->definitions[static_cast<usize>(converted->info)].name == "KEYRING");
    CHECK(converted->value == 2);

    auto changed = *baseline;
    changed.items[itemIndex].info = sourceRecord;
    CHECK_FALSE(changed.valid());
    CHECK_FALSE(owner.prepareRestore(changed));
    changed = *baseline;
    changed.placements[placementIndex].params[0] = 1;
    changed.placements[placementIndex].params[1] = 0;
    CHECK_FALSE(changed.valid());
    changed = *baseline;
    changed.definitions[static_cast<usize>(sourceRecord)].subtype = 1;
    CHECK_FALSE(changed.valid());
    changed = *baseline;
    changed.items[itemIndex].radius += 1;
    CHECK_FALSE(changed.valid());

    REQUIRE(owner.restore(*baseline));
    CHECK(owner.checkpoint() == baseline);
}

TEST_CASE("native pickup catalog includes inactive level resources and resumes collection bursts",
          "[placed-items-checkpoint][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2");
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive powerups;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    REQUIRE(powerups.load(root / "POWERUPS"));
    PlacedItems items;
    REQUIRE(items.bind(device, layout, nullptr, std::array{&powerups}));
    items.setPlayerCount(1);
    PlacedItemsCheckpoint owner{items};
    REQUIRE(owner.bindResources(device));
    const auto baseline = owner.checkpoint();
    REQUIRE(baseline);
    usize gem = items.size();
    for (usize i = 0; i < items.size(); ++i) {
        if (items.item(i).name == "GEMORANGE") {
            gem = i;
            break;
        }
    }
    REQUIRE(gem < items.size());
    auto undo = owner.prepareRestore(*baseline);
    REQUIRE(undo);
    REQUIRE(items.collect(device, std::array{Collector{items.item(gem).position, 1, 1}}).size() ==
            1);
    items.update(1.0f / 60);
    const auto active = owner.checkpoint();
    REQUIRE(active);
    REQUIRE_FALSE(active->effects.empty());
    REQUIRE_FALSE(active->particles.entries.empty());
    const auto uploads = device.texturesCreated;
    REQUIRE(owner.commitRestore(std::move(*undo)));
    CHECK(owner.checkpoint() == baseline);
    REQUIRE(owner.restore(*active));
    CHECK(owner.checkpoint() == active);
    items.update(1.0f / 60);
    const auto next = owner.checkpoint();
    REQUIRE(next);
    REQUIRE(owner.restore(*active));
    items.update(1.0f / 60);
    CHECK(owner.checkpoint() == next);
    CHECK(device.texturesCreated == uploads);
}
} // namespace
