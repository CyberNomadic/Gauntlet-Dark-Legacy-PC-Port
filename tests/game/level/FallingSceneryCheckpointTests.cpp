#include <array>
#include <limits>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/level/FallingSceneryCheckpoint.h"
#include "game/rollback/LocalStateHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void checkCues(std::span<const FallingCue> a, std::span<const FallingCue> b) {
    REQUIRE(a.size() == b.size());
    for (usize index = 0; index < a.size(); ++index) {
        CHECK((std::tie(a[index].position, a[index].sound) ==
               std::tie(b[index].position, b[index].sound)));
    }
}
struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    FallingScenery scenery;
    FallingSceneryCheckpoint owner{scenery};
    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0]}],
          "bounds":{"min":[-20,-10,-20],"max":[400,20,20]},
          "itemInfos":[
            {"type":10,"subtype":40,"radius":2,"height":3},
            {"type":10,"subtype":49,"radius":2,"height":3},
            {"type":10,"subtype":52,"radius":2,"height":3},
            {"type":10,"subtype":53,"radius":2,"height":3}],
          "itemInstances":[
            {"info":0,"name":"WALL","position":[0,0,0]},
            {"info":1,"name":"WALL","position":[20,0,0]},
            {"info":2,"name":"WALL","position":[40,0,0]},
            {"info":3,"name":"WALL","position":[60,0,0],"minPlayers":2}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(models.load(directory));
        REQUIRE(textures.load(directory));
        scenery.bind(device, layout, models, textures, "D2");
        REQUIRE(scenery.size() == 4);
    }
    FallingSceneryCheckpoint::State state() const {
        const auto value = owner.checkpoint();
        REQUIRE(value);
        return *value;
    }
    std::vector<FallingCue> step(s32 tick, f32 seconds) {
        std::vector<FallingCue> result;
        if (tick == 0) {
            REQUIRE(scenery.touch({40, 0, 0}, 1).empty());
        }
        if (tick <= 3) {
            scenery.setPlayerCount(2);
            result = tick == 2 ? scenery.shoot({40, 0, 0}, 1)
                               : scenery.touch({20.0f * static_cast<f32>(tick), 0, 0}, 1);
            REQUIRE(result.size() == 1);
        }
        scenery.update(seconds);
        return result;
    }
};

TEST_CASE("falling checkpoint resumes native profiles cues half frames and retirement",
          "[falling-checkpoint]") {
    Fixture host("falling-checkpoint-host");
    Fixture guest("falling-checkpoint-guest");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto uploads = guest.device.texturesCreated;
    const auto* first = &host.scenery.piece(0);
    usize cues = 0;
    bool distinctPrevious = false;
    for (s32 tick = 0; tick < 240; ++tick) {
        CAPTURE(tick, seconds);
        const auto before = host.state();
        REQUIRE(guest.owner.restore(before));
        auto undo = host.owner.prepareRestore(before);
        REQUIRE(undo);
        const auto output = host.step(tick, seconds);
        cues += output.size();
        const auto expected = host.state();
        distinctPrevious |= expected.pieces[0].previousPosition != expected.pieces[0].position;
        checkCues(guest.step(tick, seconds), output);
        CHECK(guest.state() == expected);
        for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
            host.device.draws.clear();
            guest.device.draws.clear();
            host.scenery.draw(host.device, Mat4{1}, {}, alpha);
            guest.scenery.draw(guest.device, Mat4{1}, {}, alpha);
            REQUIRE(host.device.draws.size() == guest.device.draws.size());
            for (usize draw = 0; draw < host.device.draws.size(); ++draw) {
                CHECK(host.device.draws[draw].vertices == guest.device.draws[draw].vertices);
                CHECK(host.device.draws[draw].transform == guest.device.draws[draw].transform);
            }
        }
        REQUIRE(host.owner.commitRestore(std::move(*undo)));
        CHECK(&host.scenery.piece(0) == first);
        CHECK(host.state() == before);
        checkCues(host.step(tick, seconds), output);
        CHECK(host.state() == expected);
    }
    CHECK(cues == 4);
    CHECK(distinctPrevious);
    for (usize index = 0; index < host.scenery.size(); ++index) {
        CHECK_FALSE(host.scenery.piece(index).motion.visible);
    }
    CHECK(guest.device.texturesCreated == uploads);
}

TEST_CASE("falling checkpoint rejects malformed motion and stale resource plans",
          "[falling-checkpoint]") {
    Fixture fixture("falling-checkpoint-fences");
    const auto before = fixture.state();
    auto invalid = before;
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5);
    switch (variant) {
    case 0: invalid.pieces[0].source.instance.position.x += 1; break;
    case 1: invalid.pieces[0].previousPosition.x = std::numeric_limits<f32>::quiet_NaN(); break;
    case 2: invalid.pieces[0].gravityStep += 1; break;
    case 3: invalid.remainder = FallingPiece::kStep; break;
    case 4: invalid.breakSound = "FOREIGN"; break;
    case 5: invalid.bottom += 1; break;
    default: break;
    }
    CHECK_FALSE(fixture.owner.restore(invalid));
    CHECK(fixture.state() == before);
    auto first = fixture.owner.prepareRestore(before);
    auto sibling = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    REQUIRE(sibling);
    FallingSceneryCheckpoint other(fixture.scenery);
    CHECK_FALSE(other.commitRestore(std::move(*first)));
    REQUIRE(fixture.owner.commitRestore(std::move(*first)));
    CHECK_FALSE(fixture.owner.canCommit(*sibling));
    first = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    fixture.scenery.clear();
    CHECK_FALSE(fixture.owner.canCommit(*first));
}

TEST_CASE("native forest falling checkpoint retains the full original model roster",
          "[falling-checkpoint][assets]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELF1/WORLDS.PS2").parent_path();
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(directory));
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    FallingScenery scenery;
    scenery.bind(device, layout, models, textures, "F1");
    REQUIRE(scenery.size() == 92);
    FallingSceneryCheckpoint owner(scenery);
    REQUIRE_FALSE(scenery.touch({-22.1875f, 17.5f, 108.953125f}, 1).empty());
    scenery.update(0.051f);
    const auto state = owner.checkpoint();
    REQUIRE(state);
    const auto uploads = device.texturesCreated;
    auto undo = owner.prepareRestore(*state);
    REQUIRE(undo);
    scenery.update(0.137f);
    scenery.setPlayerCount(0);
    REQUIRE(owner.commitRestore(std::move(*undo)));
    CHECK(owner.checkpoint() == state);
    CHECK(device.texturesCreated == uploads);
}
TEST_CASE("native falling scenery rewinds start cues half-frame motion and retirement",
          "[local-rewind][falling-local-rewind]") {
    Fixture reference("falling-native-reference");
    Fixture corrected("falling-native-corrected");
    LocalStateHistory<FallingScenery::LocalSnapshot> history;
    std::array<FallingScenery::LocalSnapshot, 8> expected;
    FallingScenery::LocalSnapshot observed;
    const auto* piece = &corrected.scenery.piece(0);
    const auto* model = &piece->model;
    const auto uploads = corrected.device.texturesCreated;
    s32 tick = 0;
    for (s32 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        REQUIRE(history.capture(static_cast<u64>(tick), [&](auto& saved) {
            corrected.scenery.saveLocal(saved);
            return true;
        }));
        const auto before = corrected.state();
        auto stale = corrected.owner.prepareRestore(before);
        REQUIRE(stale);
        const s32 distance = 1 + window % 8;
        const auto* saved = history.find(static_cast<u64>(tick));
        REQUIRE(saved);
        for (s32 offset = 0; offset < distance; ++offset) {
            corrected.step(tick + offset, 1.0f / 60);
            corrected.scenery.saveLocal(expected[static_cast<usize>(offset)]);
        }
        REQUIRE(corrected.scenery.restoreLocal(*saved));
        std::vector<std::vector<FallingCue>> cues;
        for (s32 offset = 0; offset < distance; ++offset) {
            cues.push_back(reference.step(tick + offset, 1.0f / 60));
            corrected.scenery.setPlayerCount(4);
            corrected.scenery.touch({0, 0, 0}, 100);
            corrected.scenery.shoot({40, 0, 0}, 100);
            corrected.scenery.update(0.13f);
        }
        REQUIRE(corrected.scenery.restoreLocal(*saved));
        CHECK_FALSE(corrected.owner.canCommit(*stale));
        CHECK(corrected.state() == before);
        for (s32 offset = 0; offset < distance; ++offset) {
            checkCues(corrected.step(tick + offset, 1.0f / 60), cues[static_cast<usize>(offset)]);
            corrected.scenery.saveLocal(observed);
            CHECK(corrected.scenery.equalLocal(observed, expected[static_cast<usize>(offset)]));
        }
        CHECK(corrected.state() == reference.state());
        CHECK(&corrected.scenery.piece(0) == piece);
        CHECK(&corrected.scenery.piece(0).model == model);
        for (const f32 alpha : {-1.0f, 0.0f, 0.5f, 1.0f}) {
            reference.device.draws.clear();
            corrected.device.draws.clear();
            reference.scenery.draw(reference.device, Mat4{1}, {}, alpha);
            corrected.scenery.draw(corrected.device, Mat4{1}, {}, alpha);
            REQUIRE(corrected.device.draws.size() == reference.device.draws.size());
            for (usize draw = 0; draw < reference.device.draws.size(); ++draw) {
                CHECK(corrected.device.draws[draw].vertices ==
                      reference.device.draws[draw].vertices);
            }
        }
        tick += distance;
    }
    CHECK(corrected.device.texturesCreated == uploads);
    FallingScenery::LocalSnapshot saved;
    corrected.scenery.saveLocal(saved);
    CHECK_FALSE(reference.scenery.restoreLocal(saved));
    CHECK_FALSE(reference.scenery.equalLocal(saved, saved));
    corrected.scenery.clear();
    CHECK_FALSE(corrected.scenery.restoreLocal(saved));
    CHECK_FALSE(corrected.scenery.equalLocal(saved, saved));
    corrected.scenery.bind(corrected.device, corrected.layout, corrected.models, corrected.textures,
                           "D2");
    CHECK_FALSE(corrected.scenery.restoreLocal(saved));
}
TEST_CASE("native falling equality retains subframe time without a changed piece position",
          "[local-rewind][falling-local-rewind]") {
    Fixture fixture("falling-native-hidden-clock");
    FallingScenery::LocalSnapshot before;
    FallingScenery::LocalSnapshot after;
    FallingScenery::LocalSnapshot restored;
    fixture.scenery.saveLocal(before);
    const auto position = fixture.scenery.piece(0).motion.position;
    fixture.scenery.update(1.0f / 240);
    fixture.scenery.saveLocal(after);
    CHECK(fixture.scenery.piece(0).motion.position == position);
    CHECK_FALSE(fixture.scenery.equalLocal(before, after));
    REQUIRE(fixture.scenery.restoreLocal(before));
    fixture.scenery.saveLocal(restored);
    CHECK(fixture.scenery.equalLocal(before, restored));
}
} // namespace
