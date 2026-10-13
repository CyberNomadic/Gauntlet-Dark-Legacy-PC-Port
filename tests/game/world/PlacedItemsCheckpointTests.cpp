#include <algorithm>
#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/world/PlacedItemsCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

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
    void takeGem(usize index) {
        const auto result =
            items.collect(device, std::array{Collector{items.item(index).position, 1, 1}});
        REQUIRE(result.size() == 1);
        REQUIRE(result.front().item == index);
    }
};

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
