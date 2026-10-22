#include <array>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/level/Rotators.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void checkCues(std::span<const RotatorCue> actual, std::span<const RotatorCue> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize index = 0; index < actual.size(); ++index) {
        CHECK(actual[index] == expected[index]);
    }
}

// Compare the actual public native fields across independently loaded resources.
// A figure's resource pointer is local to its owner; its output is compared below,
// while complete native figure values are checked by same-owner equalLocal.
auto valuesOf(const Rotators::Rotator& value) {
    return std::tuple{value.instance,       value.object, value.subtype, value.speed,
                      value.limit,          value.turned, value.origin,  value.spot,
                      value.radius,         value.height, value.started, value.done,
                      value.pad.has_value()};
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    ItemArchive items;
    WorldScene scene;
    Rotators rotators;

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
    std::vector<RotatorCue> step(s32 tick, f32 seconds) {
        std::vector<TriggerVisitor> visitors;
        if (tick == 2 || tick == 4 || tick == 6) {
            visitors.push_back(TriggerVisitor{.position = Vec3{static_cast<f32>(tick * 5), 0, 0}});
        }
        return rotators.update(seconds, visitors, scene);
    }
};

TEST_CASE("native rotator rewind restores spinning and first pad contacts after wrong predictions",
          "[rotators-local-rewind]") {
    Fixture reference("rotators-local-reference");
    Fixture predicted("rotators-local-predicted");
    Rotators::LocalSnapshot saved;
    std::array<Rotators::LocalSnapshot, 8> expected;
    Rotators::LocalSnapshot observed;
    WorldScene::LocalSnapshot savedScene;
    const auto uploads = predicted.device.texturesCreated;
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    s32 tick = 0;
    usize stopped = 0;
    for (s32 window = 0; window < 80; ++window) {
        CAPTURE(window, tick, seconds);
        const s32 distance = 1 + window % 8;
        predicted.rotators.saveLocal(saved);
        predicted.scene.saveLocal(savedScene);
        for (s32 step = 0; step < distance; ++step) {
            predicted.step(tick + step, seconds);
            predicted.rotators.saveLocal(expected[static_cast<usize>(step)]);
        }
        REQUIRE(predicted.rotators.restoreLocal(saved));
        REQUIRE(predicted.scene.restoreLocal(savedScene));
        // The incorrect branch steps on every pad and advances farther than the
        // retained window. Restoring the owner must not place the world itself.
        std::array<TriggerVisitor, 3> wrongVisitors{};
        for (usize pad = 0; pad < wrongVisitors.size(); ++pad) {
            wrongVisitors[pad].position = predicted.rotators.rotator(pad + 1).spot;
        }
        predicted.rotators.update(0.25f, wrongVisitors, predicted.scene);
        predicted.rotators.update(0.25f, {}, predicted.scene);
        const auto wrongTransform = predicted.scene.worldTransform(0);
        REQUIRE(predicted.rotators.restoreLocal(saved));
        predicted.rotators.saveLocal(observed);
        CHECK(predicted.rotators.equalLocal(observed, saved));
        CHECK(predicted.scene.worldTransform(0) == wrongTransform);
        REQUIRE(predicted.scene.restoreLocal(savedScene));
        for (s32 step = 0; step < distance; ++step, ++tick) {
            const auto cues = reference.step(tick, seconds);
            checkCues(predicted.step(tick, seconds), cues);
            predicted.rotators.saveLocal(observed);
            CHECK(predicted.rotators.equalLocal(observed, expected[static_cast<usize>(step)]));
            REQUIRE(predicted.rotators.size() == reference.rotators.size());
            for (usize index = 0; index < reference.rotators.size(); ++index) {
                CHECK((valuesOf(predicted.rotators.rotator(index)) ==
                       valuesOf(reference.rotators.rotator(index))));
            }
            for (const auto& cue : cues) {
                stopped += cue.kind == RotatorCue::Kind::Stopped ? 1U : 0U;
            }
            for (usize object = 0; object < reference.layout.objects().size(); ++object) {
                CHECK(predicted.scene.worldTransform(object) ==
                      reference.scene.worldTransform(object));
            }
        }
        reference.device.draws.clear();
        predicted.device.draws.clear();
        reference.rotators.draw(reference.device, Mat4{1}, {});
        predicted.rotators.draw(predicted.device, Mat4{1}, {});
        REQUIRE(predicted.device.draws.size() == reference.device.draws.size());
        REQUIRE_FALSE(predicted.device.draws.empty());
        for (usize draw = 0; draw < reference.device.draws.size(); ++draw) {
            CHECK(predicted.device.draws[draw].transform == reference.device.draws[draw].transform);
            CHECK(predicted.device.draws[draw].vertices == reference.device.draws[draw].vertices);
        }
        REQUIRE(predicted.rotators.canRestoreLocal(saved));
    }
    CHECK(stopped == 3);
    CHECK(predicted.rotators.rotator(1).turned >= 1);
    CHECK(predicted.rotators.rotator(2).turned <= -1);
    CHECK(predicted.rotators.rotator(3).done);
    CHECK_FALSE(predicted.rotators.rotator(3).pad);
    CHECK(predicted.device.texturesCreated == uploads);
}

TEST_CASE("native invisible turntable rewind preserves contact and delayed corrections",
          "[rotators-local-rewind][assets]") {
    const std::string name = GENERATE("I2", "J3");
    const auto root =
        test::assetOrSkip("LEVELS/LEVELI2/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    REQUIRE(layout.load(root / (std::string{"LEVELS/LEVEL"} + name)));
    REQUIRE(items.load(root / (std::string{"ITEMS/LEVEL"} + name[0])));
    Rotators reference;
    Rotators predicted;
    reference.bind(layout);
    predicted.bind(layout);
    reference.bindFigures(device, layout, AssetScope(items));
    predicted.bindFigures(device, layout, AssetScope(items));
    WorldScene referenceScene;
    WorldScene predictedScene;
    const auto uploads = device.texturesCreated;
    std::vector<TriggerVisitor> pads;
    for (usize index = 0; index < reference.size(); ++index) {
        const auto& rotator = reference.rotator(index);
        if (rotator.subtype == Rotators::kTurnedByPad) {
            REQUIRE_FALSE(rotator.pad);
            pads.push_back(TriggerVisitor{.position = rotator.spot});
        }
    }
    REQUIRE(pads.size() == (name == "I2" ? 1 : 2));
    const auto step = [&](Rotators& owner, WorldScene& scene, s32 tick) {
        std::vector<TriggerVisitor> visitors;
        for (usize pad = 0; pad < pads.size(); ++pad) {
            if (tick == 2 + static_cast<s32>(pad) * 2) {
                visitors.push_back(pads[pad]);
            }
        }
        return owner.update(1.0f / 60, visitors, scene);
    };
    Rotators::LocalSnapshot saved;
    Rotators::LocalSnapshot observed;
    std::array<Rotators::LocalSnapshot, 8> expected;
    s32 tick = 0;
    for (s32 window = 0; window < 136; ++window) {
        CAPTURE(name, window, tick);
        const s32 distance = 1 + window % 8;
        predicted.saveLocal(saved);
        for (s32 offset = 0; offset < distance; ++offset) {
            step(predicted, predictedScene, tick + offset);
            predicted.saveLocal(expected[static_cast<usize>(offset)]);
        }
        REQUIRE(predicted.restoreLocal(saved));
        predicted.update(0.5f, pads, predictedScene);
        REQUIRE(predicted.restoreLocal(saved));
        predicted.saveLocal(observed);
        CHECK(predicted.equalLocal(observed, saved));
        for (s32 offset = 0; offset < distance; ++offset, ++tick) {
            checkCues(step(predicted, predictedScene, tick), step(reference, referenceScene, tick));
            predicted.saveLocal(observed);
            CHECK(predicted.equalLocal(observed, expected[static_cast<usize>(offset)]));
            REQUIRE(predicted.size() == reference.size());
            for (usize index = 0; index < reference.size(); ++index) {
                CHECK((valuesOf(predicted.rotator(index)) == valuesOf(reference.rotator(index))));
            }
        }
    }
    CHECK(tick >= 600);
    for (usize index = 0; index < reference.size(); ++index) {
        if (reference.rotator(index).subtype == Rotators::kTurnedByPad) {
            CHECK(reference.rotator(index).started);
            CHECK_FALSE(reference.rotator(index).pad);
        }
    }
    device.draws.clear();
    reference.draw(device, Mat4{1}, {});
    predicted.draw(device, Mat4{1}, {});
    CHECK(device.draws.empty());
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("native rotator equality retains pad activation before its first angular step",
          "[rotators-local-rewind]") {
    Fixture fixture("rotators-local-hidden-values");
    Rotators::LocalSnapshot before;
    Rotators::LocalSnapshot after;
    Rotators::LocalSnapshot restored;
    fixture.rotators.saveLocal(before);
    const auto angle = fixture.rotators.rotator(1).turned;
    const auto position = fixture.rotators.rotator(1).spot;
    const auto cues =
        fixture.rotators.update(0, std::array{TriggerVisitor{.position = position}}, fixture.scene);
    REQUIRE_FALSE(cues.empty());
    REQUIRE(fixture.rotators.rotator(1).started);
    CHECK(fixture.rotators.rotator(1).turned == angle);
    fixture.rotators.saveLocal(after);
    CHECK_FALSE(fixture.rotators.equalLocal(before, after));
    REQUIRE(fixture.rotators.restoreLocal(before));
    fixture.rotators.saveLocal(restored);
    CHECK(fixture.rotators.equalLocal(before, restored));
}

TEST_CASE("native rotator histories are scoped to their owner and bound stage",
          "[rotators-local-rewind]") {
    Fixture first("rotators-local-guards");
    Fixture other("rotators-local-foreign");
    Rotators::LocalSnapshot saved;
    CHECK_FALSE(first.rotators.restoreLocal(saved));
    CHECK_FALSE(first.rotators.equalLocal(saved, saved));
    first.rotators.saveLocal(saved);
    CHECK(first.rotators.equalLocal(saved, saved));
    Rotators::LocalSnapshot otherBefore;
    Rotators::LocalSnapshot otherAfter;
    other.rotators.saveLocal(otherBefore);
    CHECK_FALSE(other.rotators.restoreLocal(saved));
    CHECK_FALSE(other.rotators.equalLocal(saved, saved));
    other.rotators.saveLocal(otherAfter);
    CHECK(other.rotators.equalLocal(otherBefore, otherAfter));
    REQUIRE(first.rotators.restoreLocal(saved));
    REQUIRE(first.rotators.restoreLocal(saved));
    first.rotators.bindFigures(first.device, first.layout, AssetScope(first.items));
    CHECK_FALSE(first.rotators.restoreLocal(saved));
    first.rotators.saveLocal(saved);
    first.rotators.bind(first.layout);
    CHECK_FALSE(first.rotators.restoreLocal(saved));
    first.rotators.saveLocal(saved);
    first.rotators.clear();
    CHECK_FALSE(first.rotators.restoreLocal(saved));
    Rotators empty;
    empty.saveLocal(saved);
    REQUIRE(empty.restoreLocal(saved));
}
} // namespace
