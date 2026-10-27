#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "game/hud/StreakMeter.h"
#include "game/players/MeleeStreak.h"
#include "game/players/StreakGrades.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::vector<BitmapGlyph> glyphs() {
    constexpr std::string_view kChars = "ABCDSXMONEWRT!0123456789";
    std::vector<BitmapGlyph> cells;
    for (usize i = 0; i < kChars.size(); ++i) {
        cells.push_back({kChars[i], 8, static_cast<s32>(i) * 8, 0});
    }
    return cells;
}

struct Fixture {
    BitmapFont font = BitmapFont::fromGlyphs(32, 8, glyphs());
    test::FakeTexture sheet{256, 32};
    test::FakeTexture glow{256, 32};
    TextPainter painter;
    test::FakeRenderDevice device;
    Canvas canvas;
    StreakMeter meter;
    Fixture() { painter.setFont(&font, &sheet); }

    /** Frames of a twentieth of a second, the way the scene feeds the meter. */
    void pass(f32 seconds) {
        const auto frames = static_cast<s32>(std::lround(seconds / 0.05f));
        for (s32 frame = 0; frame < frames; ++frame) {
            meter.animate(0.05f);
        }
    }
    void see(s32 player, s32 count, bool running, s32 record = 0, f32 quiet = 0.0f) {
        const std::array<StreakView, 1> views{StreakView{player, count, quiet, running, record}};
        meter.observe(views);
    }
    usize drawn() {
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        meter.draw(canvas, painter, &glow, nullptr);
        canvas.end();
        return device.draws.size();
    }
    Vec2 lowest() const {
        Vec2 corner{-1e9f, -1e9f};
        for (const auto& draw : device.draws) {
            const Vec2 far = test::maxCorner(draw);
            corner.x = std::max(corner.x, far.x);
            corner.y = std::max(corner.y, far.y);
        }
        return corner;
    }
};

TEST_CASE("streak grades follow the narrator's thresholds and the bar climbs then drains",
          "[game][hud][streak]") {
    CHECK(StreakMeter::gradeOf(0) == -1);
    CHECK(StreakMeter::gradeOf(1) == 0);
    CHECK(StreakMeter::gradeOf(24) == 0);
    CHECK(StreakMeter::gradeOf(25) == 1);
    CHECK(StreakMeter::gradeOf(50) == 2);
    CHECK(StreakMeter::gradeOf(75) == 3);
    CHECK(StreakMeter::gradeOf(100) == 4);
    CHECK(StreakMeter::gradeOf(200) == 4);
    CHECK(StreakMeter::kLetters[4] == 'S');
    CHECK(StreakMeter::kGradeCounts == StreakGrades::kCounts);
    CHECK(StreakMeter::bandProgress(0) == 0.0f);
    CHECK(StreakMeter::bandProgress(13) == Approx(0.5f));
    CHECK(StreakMeter::bandProgress(100) == 1.0f);
    CHECK(StreakMeter::bandProgress(150) == 1.0f);
    // The climb through the grade sets the reach; the grade's quiet drains it to nothing,
    // three seconds at D and one and a half at S.
    const f32 full = StreakMeter::fillOf(13, 0.0f);
    CHECK(full == Approx(0.12f + 0.88f * 0.5f));
    CHECK(StreakMeter::fillOf(13, 1.5f) == Approx(full * 0.5f));
    CHECK(StreakMeter::fillOf(13, 3.0f) == 0.0f);
    CHECK(StreakMeter::fillOf(0, 0.0f) == 0.0f);
    CHECK(StreakMeter::fillOf(100, 0.0f) == 1.0f);
    CHECK(StreakMeter::fillOf(100, 0.75f) == Approx(0.5f));
    CHECK(StreakMeter::fillOf(100, 1.5f) == 0.0f);
    CHECK(StreakMeter::shakeOf(-1) == 0.0f);
    CHECK(StreakMeter::shakeOf(4) > StreakMeter::shakeOf(1));
    CHECK(StreakMeter::letterScaleOf(4) > StreakMeter::letterScaleOf(0));
}

TEST_CASE("a run brings the meter in and every blow punches it while a new grade flashes",
          "[game][hud][streak]") {
    Fixture f;
    CHECK_FALSE(f.meter.visible());
    CHECK_FALSE(f.meter.look(0).shown);
    f.see(0, 1, true);
    auto look = f.meter.look(0);
    CHECK(look.shown);
    CHECK(look.grade == 0);
    CHECK(look.count == 1);
    CHECK(look.punch == 1.0f);
    CHECK(look.rankFlash == 0.0f); // the first grade comes in, it is not climbed to
    CHECK(look.presence == 0.0f);
    f.meter.animate(0.1f);
    CHECK(f.meter.look(0).presence == Approx(0.6f));
    CHECK(f.meter.look(0).fill == Approx(StreakMeter::fillOf(1, 0.0f)));
    f.meter.animate(0.1f);
    CHECK(f.meter.look(0).presence == 1.0f);
    CHECK(f.meter.look(0).punch == 0.0f);
    // An unchanged count is no blow: the punch keeps decaying.
    f.see(0, 1, true);
    CHECK(f.meter.look(0).punch == 0.0f);
    f.see(0, 4, true);
    CHECK(f.meter.look(0).punch == 1.0f);
    f.meter.animate(0.05f);
    CHECK(f.meter.look(0).punch == Approx(0.75f));
    const f32 shakeBefore = f.meter.look(0).shake;
    f.see(0, 25, true);
    look = f.meter.look(0);
    CHECK(look.grade == 1);
    CHECK(look.rankFlash == StreakMeter::kRankFlash);
    CHECK(look.shake > shakeBefore);
    // A correction can take blows back: the grade follows without celebrating.
    f.pass(0.5f);
    f.see(0, 8, true);
    CHECK(f.meter.look(0).grade == 0);
    CHECK(f.meter.look(0).punch == 0.0f);
    // The bar drains at the quiet's pace and leaps back with the next blow.
    f.see(0, 8, true, 0, 1.0f);
    f.pass(0.3f);
    CHECK(f.meter.look(0).fill == Approx(StreakMeter::fillOf(8, 1.0f)));
    f.see(0, 9, true, 0, 0.0f);
    f.meter.animate(0.05f);
    CHECK(f.meter.look(0).fill == Approx(StreakMeter::fillOf(9, 0.0f)));
}

TEST_CASE("the run's end lingers the verdict then the meter fades and forgets the run",
          "[game][hud][streak]") {
    Fixture f;
    f.see(0, 30, true, 30);
    f.pass(0.5f);
    f.see(0, 0, false, 30);
    auto look = f.meter.look(0);
    CHECK(look.ended);
    CHECK(look.grade == 1);
    CHECK(look.count == 30);
    CHECK(look.punch == 1.0f);
    f.pass(StreakMeter::kLinger * 0.5f);
    look = f.meter.look(0);
    CHECK(look.shown);
    CHECK(look.presence == 1.0f);
    CHECK(look.fill == Approx(0.0f).margin(1e-6f));
    f.pass(StreakMeter::kLinger * 0.5f + 0.05f);
    CHECK(f.meter.look(0).presence < 1.0f);
    f.pass(1.0f);
    look = f.meter.look(0);
    CHECK_FALSE(look.shown);
    CHECK_FALSE(look.ended);
    CHECK(look.grade == -1);
    CHECK(look.count == 0);
    CHECK(look.best == 30);
    CHECK_FALSE(f.meter.visible());
    // A seat no longer watched leaves without a verdict.
    f.see(1, 5, true);
    f.pass(0.3f);
    f.meter.observe({});
    CHECK(f.meter.look(1).shown);
    CHECK_FALSE(f.meter.look(1).ended);
    f.pass(1.0f);
    CHECK_FALSE(f.meter.look(1).shown);
    f.meter.clear();
    CHECK(f.meter.look(0).best == 0);
}

TEST_CASE("a record is announced once the run passes the one standing when it began",
          "[game][hud][streak]") {
    Fixture f;
    // The scene shows the meter the record before any run, as it opens.
    f.see(0, 0, false, 10);
    f.see(0, 3, true, 10);
    f.see(0, 10, true, 10);
    CHECK(f.meter.look(0).recordShow == 0.0f);
    f.see(0, 11, true, 11);
    auto look = f.meter.look(0);
    CHECK(look.recordShow == StreakMeter::kRecordShow);
    CHECK(look.best == 11);
    f.see(0, 14, true, 14);
    CHECK(f.meter.look(0).recordShow == StreakMeter::kRecordShow); // once a run
    f.pass(0.2f);
    CHECK(f.meter.look(0).recordShow == Approx(StreakMeter::kRecordShow - 0.2f));
    // The next run has the new record to beat.
    f.see(0, 0, false, 14);
    f.pass(StreakMeter::kLinger + 1.0f);
    f.see(0, 5, true, 14);
    f.see(0, 14, true, 14);
    CHECK(f.meter.look(0).recordShow == 0.0f);
    f.see(0, 15, true, 15);
    CHECK(f.meter.look(0).recordShow == StreakMeter::kRecordShow);
    // A character without a record yet has nothing to beat on the first run.
    Fixture fresh;
    fresh.see(2, 0, false, 0);
    fresh.see(2, 1, true, 1);
    fresh.see(2, 30, true, 30);
    CHECK(fresh.meter.look(2).recordShow == 0.0f);
}

TEST_CASE("the meter draws each local seat's run stacked in the upper left",
          "[game][hud][streak]") {
    Fixture f;
    CHECK(f.drawn() == 0);
    f.see(0, 12, true, 12);
    f.pass(StreakMeter::kRankFlash + 0.2f); // past the grade's burst of shards
    const usize single = f.drawn();
    REQUIRE(single > 0);
    const Vec2 oneRow = f.lowest();
    CHECK(oneRow.x < 200.0f);
    CHECK(oneRow.y < static_cast<f32>(StreakMeter::kY + StreakMeter::kRowHeight + 40));
    for (const auto& draw : f.device.draws) {
        CHECK(test::minCorner(draw).x >= 0.0f);
        CHECK(test::minCorner(draw).y >= 0.0f);
    }
    // A second seat's meter goes under the first; a record announcement adds its shower.
    const std::array<StreakView, 2> both{StreakView{0, 12, 0.0f, true, 12},
                                         StreakView{1, 100, 0.0f, true, 100}};
    f.meter.observe(both);
    f.pass(0.5f);
    const usize two = f.drawn();
    CHECK(two > single);
    const Vec2 twoRows = f.lowest();
    CHECK(twoRows.y > oneRow.y + static_cast<f32>(StreakMeter::kRowHeight) * 0.5f);
    f.see(1, 100, true, 100); // the first seat gone, the second row climbs
    f.pass(1.0f);
    CHECK(f.drawn() > 0);
    CHECK(f.lowest().y < twoRows.y - static_cast<f32>(StreakMeter::kRowHeight) * 0.5f);
    // Without a font there is nothing to draw with.
    const TextPainter bare;
    f.device.draws.clear();
    f.canvas.begin(f.device, Mat4{1});
    f.meter.draw(f.canvas, bare, nullptr, nullptr);
    f.canvas.end();
    CHECK(f.device.draws.empty());
}

} // namespace
