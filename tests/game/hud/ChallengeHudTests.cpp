#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "game/hud/ChallengeHud.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("the hourglass crops its two sand reservoirs as time elapses", "[secret][hud]") {
    const auto full = ChallengeHud::sand(60, 60);
    CHECK(full.upper.y == 24);
    CHECK(full.upper.height == 41);
    CHECK(full.upperUv.y == Approx(23.0f / 128));
    CHECK(full.lower.y == 106);
    CHECK(full.lower.height == 23);
    const auto empty = ChallengeHud::sand(0, 60);
    CHECK(empty.upper.y == 63);
    CHECK(empty.upper.height == 2);
    CHECK(empty.upperUv.height == 0);
    CHECK(empty.lower.y == 68);
    CHECK(empty.lower.height == 61);
    CHECK(empty.lowerUv.y == Approx(67.0f / 128));
    CHECK(ChallengeHud::sand(-10, 60).lower.height == 61);
    CHECK(ChallengeHud::sand(80, 60).upper.height == 41);
}

TEST_CASE("hourglass uses the shipped sand animation and draws both cropped halves",
          "[secret][hud][assets]") {
    const auto directory = test::assetOrSkip("POWERUPS/textures.ngc").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(directory));
    ChallengeHud hud;
    REQUIRE(hud.bind(device, archive));
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    hud.draw(canvas, 30, 60, true);
    canvas.end();
    REQUIRE(device.draws.size() == 3);
    CHECK(device.draws.back().vertices.size() == 12);
    const auto* first = device.draws[1].texture;
    hud.step(1.0f / 30);
    device.draws.clear();
    canvas.begin(device, Mat4{1});
    hud.draw(canvas, 29, 60, true);
    canvas.end();
    REQUIRE(device.draws.size() == 3);
    CHECK(device.draws[1].texture != first);
    hud.clear();
}

TEST_CASE("native hourglass rewind retains subframe sand clock without binding textures again",
          "[secret][hud][challenge-hud-local-rewind][assets]") {
    const auto directory = test::assetOrSkip("POWERUPS/textures.ngc").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(directory));
    ChallengeHud expected;
    ChallengeHud corrected;
    REQUIRE(expected.bind(device, archive));
    REQUIRE(corrected.bind(device, archive));
    const auto seconds = [](s32 t) { return t % 3 == 0 ? 1.0f / 120 : 1.0f / 40; };
    s32 at = 0;
    for (s32 window = 0; window < 96; ++window) {
        const s32 delay = 1 + window % 8;
        CAPTURE(window, at, delay);
        ChallengeHud::LocalSnapshot saved;
        corrected.saveLocal(saved);
        CHECK_FALSE(expected.restoreLocal(saved));
        for (s32 t = at; t < at + delay; ++t) {
            expected.step(seconds(t));
            corrected.step(0.73f);
        }
        const auto created = device.texturesCreated;
        const auto uploaded = device.textureUpdates;
        REQUIRE(corrected.restoreLocal(saved));
        for (s32 t = at; t < at + delay; ++t) {
            corrected.step(seconds(t));
        }
        CHECK(device.texturesCreated == created);
        CHECK(device.textureUpdates == uploaded);
        const auto a = corrected.look(30, 60, true);
        const auto b = expected.look(30, 60, true);
        CHECK(a.elapsed == b.elapsed);
        CHECK(a.fallingFrame == b.fallingFrame);
        const auto draw = [&](ChallengeHud& hud) {
            device.draws.clear();
            Canvas canvas;
            canvas.begin(device, Mat4{1});
            hud.draw(canvas, 30, 60, true);
            canvas.end();
            return device.draws;
        };
        const auto reference = draw(expected);
        const auto actual = draw(corrected);
        REQUIRE(actual.size() == reference.size());
        for (usize i = 0; i < actual.size(); ++i) {
            test::checkRecordedDraw(actual[i], reference[i]);
        }
        at += delay;
    }
    ChallengeHud::LocalSnapshot stale;
    corrected.saveLocal(stale);
    corrected.clear();
    REQUIRE_FALSE(corrected.restoreLocal(stale));
}
} // namespace
