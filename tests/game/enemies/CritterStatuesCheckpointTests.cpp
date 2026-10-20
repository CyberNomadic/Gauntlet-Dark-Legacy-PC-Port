#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/CritterStatues.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path checkpointArchive() {
    const auto path = test::scratchDirectory("statue-checkpoint");
    std::filesystem::create_directories(path / "models");
    writeTextFile(path / "models/body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(path / "objects.json", R"({"objects":[
      {"index":0,"name":"STONE","file":"models/body.obj","meshTriangles":1}]})");
    writeFile(path / "skin.png", test::kTinyPng);
    writeTextFile(path / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(path);
    writeTextFile(path / "animations.json", R"({
      "particles":[{"enables":16928,"texture":"SKIN","particleLife":[1,0],
                    "rate":[30,30,30,30],"flags":128,"flagMask":128}],
      "trees":[{"name":"GOL_STATUE","nodes":[
        {"name":"STONE","object":"STONE","parent":-1,"position":[0,0,0]},
        {"name":"SMOKE","particle":0,"parent":0,"position":[0,2,0]}],
        "sequences":[{"name":"IDLE","frames":0,"frameRate":30},
                     {"name":"ACTIVE","frames":10,"frameRate":30},
                     {"name":"EMPTY","frames":0,"frameRate":30}]}]})");
    return path;
}

CritterStatues::Placement statueAt(f32 x, s32 activeOn = 0) {
    CritterStatues::Placement result;
    result.kind = CombatantKind::Golem;
    result.form = "STONE";
    result.instance.info = 3;
    result.instance.minPlayers = 2;
    result.instance.flags = 7;
    result.instance.name = "PLACED";
    result.instance.position = {x, 2, 4};
    result.instance.rotation = {0.1f, 0.2f, 0.3f};
    result.instance.params = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    result.instance.collision.push_back({{0, 1, 0}, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}}});
    result.radius = 3;
    result.height = 5;
    result.viewRadius = 10;
    result.sight = 12;
    result.activeOn = activeOn;
    result.carried = 8;
    return result;
}

struct StatueFixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    CritterStatues native;
    CritterStatues replay;
    StatueFixture() {
        REQUIRE(archive.load(checkpointArchive()));
        for (auto* pool : {&native, &replay}) {
            pool->setContactGridBounds({-320, 0, -320}, {320, 0, 320});
            REQUIRE(pool->add(device, archive, statueAt(0, 3), nullptr));
            REQUIRE(pool->add(device, archive, statueAt(30), nullptr));
            REQUIRE(pool->add(device, archive, statueAt(60), nullptr));
        }
    }
};

TEST_CASE("statue checkpoints restore compacted identities queued births and exact continuation",
          "[critter-statues-checkpoint][rollback]") {
    const s32 ticks = GENERATE(1, 2);
    const f32 seconds = static_cast<f32>(ticks) / 60;
    StatueFixture fixture;
    const auto initial = fixture.native.checkpoint();
    REQUIRE(initial);
    fixture.native.wake(0);
    fixture.native.wake(1);
    fixture.native.update(ticks, seconds, {}); // starts ACTIVE without consuming its clock
    for (s32 tick = 0; tick < 6; tick += ticks) {
        fixture.native.update(ticks, seconds, {});
    }
    REQUIRE(fixture.native.count() == 2);
    REQUIRE(fixture.native.instanceOf(0) == initial->statues[1].instance);
    const auto state = fixture.native.checkpoint();
    REQUIRE(state);
    REQUIRE(state->risen.size() == 1);
    REQUIRE(state->risen.front() == statueAt(0, 3));
    REQUIRE(state->statues[0].figure.particles.field.entries.size() == 1);
    REQUIRE_FALSE(state->statues[0].figure.particles.field.entries[0].emitter.particles.empty());
    const auto uploads = fixture.device.texturesCreated;
    const auto updates = fixture.device.textureUpdates;
    REQUIRE(fixture.replay.restore(*state));
    CHECK(fixture.replay.checkpoint() == state);
    CHECK(fixture.replay.takeRisen() == fixture.native.takeRisen());
    for (s32 tick = 0; tick < 30; tick += ticks) {
        const bool seen = tick % 6 != 0;
        const auto view = [seen](const Vec3&, f32) { return seen; };
        fixture.native.update(ticks, seconds, view);
        fixture.replay.update(ticks, seconds, view);
        CHECK(fixture.replay.checkpoint() == fixture.native.checkpoint());
    }
    REQUIRE(fixture.replay.restore(*initial));
    REQUIRE(fixture.replay.count() == 3);
    CHECK(fixture.replay.instanceOf(0) == initial->statues[0].instance);
    CHECK(fixture.replay.checkpoint() == initial);
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.device.textureUpdates == updates);
}

TEST_CASE("statue checkpoint preparation is inert atomic single use and owner bound",
          "[critter-statues-checkpoint][rollback]") {
    StatueFixture fixture;
    const auto state = fixture.native.checkpoint();
    REQUIRE(state);
    fixture.native.wake(1);
    const auto before = fixture.native.checkpoint();
    REQUIRE(before);
    auto prepared = fixture.native.prepareRestore(*state);
    REQUIRE(prepared);
    CHECK(fixture.native.checkpoint() == before);
    CHECK_FALSE(fixture.replay.canCommit(*prepared));
    CHECK_FALSE(fixture.replay.commitRestore(std::move(*prepared)));
    REQUIRE(fixture.native.canCommit(*prepared));
    auto sibling = fixture.native.prepareRestore(*state);
    REQUIRE(sibling);
    REQUIRE(fixture.native.commitRestore(std::move(*prepared)));
    CHECK(fixture.native.checkpoint() == state);
    CHECK_FALSE(fixture.native.canCommit(*prepared));
    CHECK_FALSE(fixture.native.canCommit(*sibling));
    auto stale = fixture.native.prepareRestore(*state);
    REQUIRE(stale);
    const auto previousId = fixture.native.instanceOf(2);
    fixture.native.clear();
    CHECK_FALSE(fixture.native.canCommit(*stale));
    REQUIRE(fixture.native.add(fixture.device, fixture.archive, statueAt(90), nullptr));
    CHECK(fixture.native.instanceOf(0) > previousId);
    CHECK_FALSE(fixture.native.restore(*state));
    auto appended = fixture.replay.prepareRestore(*state);
    REQUIRE(appended);
    REQUIRE(fixture.replay.add(fixture.device, fixture.archive, statueAt(90), nullptr));
    CHECK_FALSE(fixture.replay.canCommit(*appended));
}

TEST_CASE("malformed statue values and foreign prototypes never partially restore",
          "[critter-statues-checkpoint][rollback]") {
    StatueFixture fixture;
    const auto initial = fixture.native.checkpoint();
    REQUIRE(initial);
    auto bad = *initial;
    SECTION("duplicate identity") {
        bad.statues[1].instance = bad.statues[0].instance;
    }
    SECTION("unknown identity") {
        bad.statues[0].instance = bad.nextInstance + 1;
    }
    SECTION("counter may not reuse identity") {
        bad.nextInstance = bad.statues.back().instance;
    }
    SECTION("zero counter") {
        bad.nextInstance = 0;
    }
    SECTION("rising needs wake") {
        bad.statues[0].rising = true;
    }
    SECTION("negative countdown") {
        bad.statues[0].ticksLeft = -1;
    }
    SECTION("unknown tree") {
        bad.statues[1].figure.pose.tree = "OTHER";
    }
    SECTION("invalid node") {
        bad.statues[1].figure.pose.nodes[0].parent = 0;
    }
    SECTION("unknown particle topology") {
        bad.statues[1].figure.particles.nodes[0].node = 19;
    }
    SECTION("NaN clock") {
        bad.statues[1].figure.animation.time = std::numeric_limits<f64>::quiet_NaN();
    }
    SECTION("invalid sequence") {
        bad.statues[1].figure.animation.sequence = 999;
    }
    SECTION("idle animation must keep looping") {
        bad.statues[0].figure.loop = false;
    }
    SECTION("native active duration is bounded by trusted assets") {
        auto& body = bad.statues[1];
        body.woken = body.rising = true;
        body.figure.index = CritterStatues::kActiveSequence;
        body.figure.loop = false;
        body.ticksLeft = 100;
    }
    SECTION("foreign full placement") {
        ++bad.roster[1].placement.instance.params[11];
    }
    SECTION("foreign native floor-binding policy") {
        bad.roster[1].placement.onFloor = !bad.roster[1].placement.onFloor;
        REQUIRE(bad.valid());
    }
    SECTION("invalid grid") {
        bad.contactGridInverseWidth = -1;
    }
    SECTION("active body cannot also be queued") {
        bad.risen.push_back(bad.roster[0].placement);
    }
    SECTION("unknown queued placement") {
        bad.statues.erase(bad.statues.begin());
        bad.risen.push_back(statueAt(999));
    }
    CHECK_FALSE(fixture.native.prepareRestore(bad));
    CHECK_FALSE(fixture.native.restore(bad));
    CHECK(fixture.native.checkpoint() == initial);
}

TEST_CASE("item figure checkpoint retains held pose presentation particles and fractional clocks",
          "[item-figure-checkpoint][rollback]") {
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(checkpointArchive()));
    ItemFigure figure;
    REQUIRE(figure.place(device, archive, "GOL_STATUE", statueAt(0).instance, nullptr));
    figure.play(1, false);
    figure.gateParticlesOnSequence(true);
    figure.update(0.073f);
    figure.capturePresentation();
    figure.tilt(0.4f, -0.2f);
    figure.update(0.017f);
    figure.draw(device, Mat4{1}, {}, 1, 1, nullptr, TreeModel::Pass::All, 0.4f);
    const auto state = figure.checkpoint();
    REQUIRE(state);
    REQUIRE(state->presentationPose);
    REQUIRE(state->textures.remainder > 0);
    REQUIRE_FALSE(state->particles.field.entries[0].emitter.particles.empty());
    const auto uploads = device.texturesCreated;
    auto clone = figure.restored(*state);
    REQUIRE(clone);
    CHECK(clone->checkpoint() == state);
    for (s32 tick = 0; tick < 8; ++tick) {
        figure.capturePresentation();
        clone->capturePresentation();
        figure.update(0.019f);
        clone->update(0.019f);
        CHECK(clone->checkpoint() == figure.checkpoint());
    }
    figure.play(2, false);
    const auto held = figure.checkpoint();
    REQUIRE(held);
    REQUIRE(held->holdPose);
    REQUIRE(held->poseSequence != held->animation.sequence);
    REQUIRE(clone->restore(*held));
    CHECK(clone->checkpoint() == held);
    figure.update(0.02f);
    clone->update(0.02f);
    CHECK(clone->checkpoint() == figure.checkpoint());
    auto bad = *held;
    bad.textureSequence = 99;
    const auto before = clone->checkpoint();
    CHECK_FALSE(clone->restore(bad));
    CHECK(clone->checkpoint() == before);
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("native gargoyle statue wakes and retires identically after fractional restore",
          "[critter-statues-checkpoint][rollback][assets]") {
    const auto root = test::assetOrSkip("MONSTERS/GAR_EAGL/ANIM.PS2").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(root));
    auto placement = statueAt(0);
    placement.kind = CombatantKind::Gargoyle;
    placement.form = "GAR_EAGL";
    CritterStatues native;
    CritterStatues replay;
    REQUIRE(native.add(device, archive, placement, nullptr));
    REQUIRE(replay.add(device, archive, placement, nullptr));
    native.wake(0);
    native.update(1, 1.0f / 60, {});
    for (s32 tick = 0; tick < 17; ++tick) {
        native.update(1, 1.0f / 60, {});
    }
    const auto state = native.checkpoint();
    REQUIRE(state);
    REQUIRE(replay.restore(*state));
    for (s32 tick = 17; tick < 171; ++tick) {
        native.update(1, 1.0f / 60, {});
        replay.update(1, 1.0f / 60, {});
        CHECK(replay.checkpoint() == native.checkpoint());
    }
    REQUIRE(native.count() == 0);
    CHECK(replay.takeRisen() == native.takeRisen());
}

TEST_CASE("native statue rewind restores wrong wake branches across compaction and pending births",
          "[critter-statues-local-rewind][native-equality]") {
    StatueFixture fixture;
    CritterStatues::LocalSnapshot saved;
    std::array<CritterStatues::LocalSnapshot, 8> uninterrupted;
    CritterStatues::LocalSnapshot actual;
    const auto uploads = fixture.device.texturesCreated;
    const auto updates = fixture.device.textureUpdates;
    const auto advance = [](CritterStatues& statues, s32 tick) {
        if (tick == 0 || tick == 10 || tick == 24) {
            statues.wake(statues.count() > 1 ? 1 : 0);
        }
        statues.update(1, 1.0f / 60, [tick](const Vec3&, f32) { return tick % 4 != 0; });
        if (tick % 13 == 12) {
            statues.takeRisen();
        }
    };
    bool sawRisen = false;
    bool sawCompaction = false;
    s32 tick = 0;
    for (s32 window = 0; window < 24; ++window) {
        CAPTURE(window, tick);
        const auto before = fixture.replay.checkpoint();
        REQUIRE(before);
        fixture.replay.saveLocal(saved);
        const s32 distance = 1 + window % 8;
        for (s32 step = 0; step < distance; ++step) {
            advance(fixture.replay, tick + step);
            fixture.replay.saveLocal(uninterrupted[static_cast<usize>(step)]);
        }
        REQUIRE(fixture.replay.restoreLocal(saved));
        auto diagnostic = fixture.replay.prepareRestore(*before);
        REQUIRE(diagnostic);
        for (usize i = 0; i < fixture.replay.count(); ++i) {
            fixture.replay.wake(i);
        }
        fixture.replay.setContactGridBounds({-900, 0, -800}, {700, 0, 600});
        for (s32 step = 0; step < distance; ++step) {
            fixture.replay.update(2, 1.0f / 30, {});
        }
        fixture.replay.takeRisen();
        REQUIRE(fixture.replay.restoreLocal(saved));
        CHECK_FALSE(fixture.replay.canCommit(*diagnostic));
        CHECK(fixture.replay.checkpoint() == before);
        for (s32 step = 0; step < distance; ++step, ++tick) {
            advance(fixture.native, tick);
            advance(fixture.replay, tick);
            fixture.replay.saveLocal(actual);
            CHECK(fixture.replay.equalLocal(uninterrupted[static_cast<usize>(step)], actual));
            const auto expected = fixture.native.checkpoint();
            REQUIRE(expected);
            CHECK(fixture.replay.checkpoint() == expected);
            sawRisen |= !expected->risen.empty();
            sawCompaction |= expected->statues.size() < 3;
        }
        REQUIRE(fixture.replay.canRestoreLocal(saved));
    }
    CHECK(sawRisen);
    CHECK(sawCompaction);
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.device.textureUpdates == updates);
}

TEST_CASE("native statue history refuses different pools loads and diagnostic replacement",
          "[critter-statues-local-rewind][native-equality]") {
    StatueFixture fixture;
    CritterStatues::LocalSnapshot saved;
    CHECK_FALSE(fixture.native.equalLocal(saved, saved));
    CHECK_FALSE(fixture.native.restoreLocal(saved));
    fixture.native.saveLocal(saved);
    CHECK(fixture.native.equalLocal(saved, saved));
    CHECK_FALSE(fixture.replay.equalLocal(saved, saved));
    CHECK_FALSE(fixture.replay.restoreLocal(saved));
    REQUIRE(fixture.native.restoreLocal(saved));
    REQUIRE(fixture.native.restoreLocal(saved));
    REQUIRE(fixture.native.restore(*fixture.native.checkpoint()));
    CHECK_FALSE(fixture.native.equalLocal(saved, saved));
    CHECK_FALSE(fixture.native.restoreLocal(saved));
    fixture.native.saveLocal(saved);
    REQUIRE(fixture.native.add(fixture.device, fixture.archive, statueAt(90), nullptr));
    CHECK_FALSE(fixture.native.equalLocal(saved, saved));
    CHECK_FALSE(fixture.native.restoreLocal(saved));
    fixture.native.saveLocal(saved);
    fixture.native.clear();
    CHECK_FALSE(fixture.native.equalLocal(saved, saved));
    CHECK_FALSE(fixture.native.restoreLocal(saved));
}

TEST_CASE("native statue equality sees contact-grid policy before any actor moves",
          "[critter-statues-local-rewind][native-equality]") {
    StatueFixture fixture;
    CritterStatues::LocalSnapshot before;
    CritterStatues::LocalSnapshot after;
    fixture.native.saveLocal(before);
    fixture.native.setContactGridBounds({-500, 0, -500}, {700, 0, 700});
    fixture.native.saveLocal(after);
    CHECK_FALSE(fixture.native.equalLocal(before, after));
    REQUIRE(fixture.native.restoreLocal(before));
    fixture.native.saveLocal(after);
    CHECK(fixture.native.equalLocal(before, after));
}

} // namespace
