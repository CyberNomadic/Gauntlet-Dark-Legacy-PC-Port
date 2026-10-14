#include <array>
#include <limits>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/world/RotatorsCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void checkCues(std::span<const RotatorCue> actual, std::span<const RotatorCue> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize index = 0; index < actual.size(); ++index) {
        CHECK(std::tie(actual[index].kind, actual[index].rotator, actual[index].position) ==
              std::tie(expected[index].kind, expected[index].rotator, expected[index].position));
    }
}
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
    std::vector<RotatorCue> step(s32 tick, f32 seconds) {
        std::vector<TriggerVisitor> visitors;
        if (tick == 2 || tick == 4 || tick == 6) {
            visitors.push_back(TriggerVisitor{.position = Vec3{static_cast<f32>(tick * 5), 0, 0}});
        }
        return rotators.update(seconds, visitors, scene);
    }
};

TEST_CASE("rotator checkpoint resumes native spinning and both pad directions with exact cues",
          "[rotators-checkpoint]") {
    Fixture host("rotators-checkpoint-host");
    Fixture guest("rotators-checkpoint-guest");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto uploads = guest.device.texturesCreated;
    const auto* first = &host.rotators.rotator(0);
    usize stopped = 0;
    for (s32 tick = 0; tick < 240; ++tick) {
        CAPTURE(tick, seconds);
        const auto before = host.state();
        const auto geometry = host.scene.geometry();
        REQUIRE(guest.owner.restore(before));
        REQUIRE(guest.scene.applyGeometry(geometry));
        auto undo = host.owner.prepareRestore(before);
        REQUIRE(undo);
        const auto output = host.step(tick, seconds);
        const auto expected = host.state();
        for (const auto& cue : output) {
            stopped += cue.kind == RotatorCue::Kind::Stopped ? 1U : 0U;
        }
        checkCues(guest.step(tick, seconds), output);
        CHECK(guest.state() == expected);
        for (usize object = 0; object < 4; ++object) {
            CHECK(guest.scene.worldTransform(object) == host.scene.worldTransform(object));
        }
        host.device.draws.clear();
        guest.device.draws.clear();
        host.rotators.draw(host.device, Mat4{1}, {});
        guest.rotators.draw(guest.device, Mat4{1}, {});
        REQUIRE(host.device.draws.size() == guest.device.draws.size());
        REQUIRE_FALSE(host.device.draws.empty());
        for (usize draw = 0; draw < host.device.draws.size(); ++draw) {
            CHECK(host.device.draws[draw].transform == guest.device.draws[draw].transform);
            CHECK(host.device.draws[draw].vertices == guest.device.draws[draw].vertices);
        }
        const auto advanced = host.scene.worldTransform(0);
        REQUIRE(host.owner.commitRestore(std::move(*undo)));
        CHECK(&host.rotators.rotator(0) == first);
        CHECK(host.state() == before);
        CHECK(host.scene.worldTransform(0) == advanced); // geometry belongs to the aggregate
        REQUIRE(host.scene.applyGeometry(geometry));
        checkCues(host.step(tick, seconds), output);
        CHECK(host.state() == expected);
    }
    CHECK(stopped == 3);
    CHECK(host.rotators.rotator(1).turned >= 1);
    CHECK(host.rotators.rotator(2).turned <= -1);
    CHECK(host.rotators.rotator(3).done);
    CHECK(guest.device.texturesCreated == uploads);
}

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
    auto& figure = *fixture.rotators.rotator(1).pad;
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

TEST_CASE("native invisible turntable checkpoints preserve contact and delayed corrections",
          "[rotators-checkpoint][assets]") {
    const std::string name = GENERATE("I2", "J3");
    const auto root =
        test::assetOrSkip("LEVELS/LEVELI2/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    REQUIRE(layout.load(root / (std::string{"LEVELS/LEVEL"} + name)));
    REQUIRE(items.load(root / (std::string{"ITEMS/LEVEL"} + name[0])));
    Rotators live, restored;
    live.bind(layout);
    restored.bind(layout);
    live.bindFigures(device, layout, AssetScope(items));
    restored.bindFigures(device, layout, AssetScope(items));
    RotatorsCheckpoint a{live}, b{restored};
    WorldScene first, second;
    const auto uploads = device.texturesCreated;
    usize pads = 0;
    for (usize index = 0; index < live.size(); ++index) {
        const auto& rotator = live.rotator(index);
        if (rotator.subtype == Rotators::kTurnedByPad) {
            ++pads;
            REQUIRE_FALSE(rotator.pad);
            const std::array visitors{TriggerVisitor{.position = rotator.spot}};
            live.update(1.0f / 60, visitors, first);
        }
    }
    REQUIRE(pads == (name == "I2" ? 1 : 2));
    for (s32 tick = 0; tick < 600; ++tick) {
        CAPTURE(name, tick);
        const auto state = a.checkpoint();
        REQUIRE(state);
        if (tick % 5 == 0) {
            REQUIRE(b.restore(*state));
        }
        checkCues(restored.update(1.0f / 60, {}, second), live.update(1.0f / 60, {}, first));
        CHECK(b.checkpoint() == a.checkpoint());
    }
    CHECK(device.texturesCreated == uploads);
}
} // namespace
