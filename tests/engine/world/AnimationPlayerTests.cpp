
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"

namespace {

using namespace gdl;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

TreeSequenceInfo sequence(s32 frames, s32 rate) {
    TreeSequenceInfo info;
    info.name = "TEST";
    info.frames = frames;
    info.frameRate = rate;
    return info;
}

TEST_CASE("native animation comparison preserves hidden clocks and resource binding",
          "[animation][local-rewind]") {
    const auto source = sequence(7, 24);
    const auto otherSource = source;
    AnimationPlayer player;
    player.start(source, 0);
    const auto saved = player;
    REQUIRE(player.equalLocal(saved));
    player.advance(0.001f, true);
    REQUIRE(player.frame() == saved.frame());
    CHECK_FALSE(player.equalLocal(saved));
    player = saved;
    player.setSpeed(2);
    CHECK_FALSE(player.equalLocal(saved));
    player = saved;
    player.setSmooth(true);
    CHECK_FALSE(player.equalLocal(saved));
    AnimationPlayer other;
    other.start(otherSource, 0);
    CHECK_FALSE(other.equalLocal(saved));
    other = saved;
    CHECK(other.equalLocal(saved));
}

TEST_CASE("animation checkpoints resume fractional clocks, transitions, loops and held endings",
          "[world][animation][rollback]") {
    const auto source = sequence(7, 24);
    for (const bool repeat : {false, true}) {
        for (const bool smooth : {false, true}) {
            AnimationPlayer original;
            original.setSmooth(smooth);
            original.setSpeed(4.0f / 3.0f);
            original.start(source, 5, 0.1f, 1.25f, 0.025f);
            for (s32 tick = 0; tick < 120; ++tick) {
                CAPTURE(repeat, smooth, tick);
                const auto saved = original.checkpoint();
                REQUIRE(saved.valid());
                AnimationPlayer replayed;
                REQUIRE(replayed.restore(saved, &source));
                REQUIRE(replayed.checkpoint() == saved);
                auto expected = original;
                for (s32 future = 0; future < 40; ++future) {
                    const f32 seconds = future % 2 == 0 ? 1.0f / 60.0f : 1.0f / 120.0f;
                    CHECK(replayed.advance(seconds, repeat) == expected.advance(seconds, repeat));
                    CHECK(replayed.checkpoint() == expected.checkpoint());
                    CHECK(replayed.presentationFrame() == expected.presentationFrame());
                }
                original.advance(1.0f / 60.0f, repeat);
            }
            original.stop();
            AnimationPlayer stopped;
            REQUIRE(stopped.restore(original.checkpoint(), nullptr));
            CHECK(stopped.checkpoint() == original.checkpoint());
        }
    }
}

TEST_CASE("invalid animation checkpoints leave the old clock intact", "[animation][rollback]") {
    const auto source = sequence(7, 24);
    AnimationPlayer original;
    original.start(source, 2);
    original.advance(0.05f, true);
    const auto saved = original.checkpoint();
    auto bad = saved;
    bad.time = std::numeric_limits<f64>::quiet_NaN();
    CHECK_FALSE(original.restore(bad, &source));
    bad = saved;
    bad.secondsPerFrame = 0;
    CHECK_FALSE(original.restore(bad, &source));
    bad = saved;
    bad.frame = 8;
    CHECK_FALSE(original.restore(bad, &source));
    bad = saved;
    bad.transitionTime = 1;
    CHECK_FALSE(original.restore(bad, &source));
    bad = saved;
    bad.held = true;
    CHECK_FALSE(original.restore(bad, &source));
    CHECK_FALSE(original.restore(saved, nullptr));
    CHECK(original.checkpoint() == saved);
}

TEST_CASE("a sequence steps a frame per tick, wraps when it repeats and holds when it does not",
          "[world][animation]") {
    const TreeSequenceInfo cycle = sequence(12, 30);
    AnimationPlayer player;
    REQUIRE_FALSE(player.playing());
    player.start(cycle, 3);
    REQUIRE(player.playing());
    REQUIRE(player.sequence() == 3);
    REQUIRE(player.frame() == 0.0f);
    REQUIRE(player.secondsPerFrame() == Approx(kStep));
    for (s32 i = 1; i <= 11; ++i) {
        REQUIRE_FALSE(player.advance(kStep, true));
        REQUIRE(player.frame() == Approx(static_cast<f32>(i)));
        REQUIRE_FALSE(player.finished());
    }
    // The step past the last frame wraps to the first and reports the loop.
    REQUIRE(player.advance(kStep, true));
    REQUIRE(player.finished());
    REQUIRE(player.frame() == 0.0f);
    REQUIRE_FALSE(player.advance(kStep, true));
    REQUIRE_FALSE(player.finished());
    REQUIRE(player.frame() == 1.0f);

    // Not repeating, the same step lands on the last frame and stays there.
    player.start(cycle, 3);
    for (s32 i = 0; i < 11; ++i) {
        player.advance(kStep, false);
    }
    REQUIRE(player.advance(kStep, false));
    REQUIRE(player.finished());
    REQUIRE(player.frame() == 11.0f);
    REQUIRE_FALSE(player.advance(kStep, false));
    REQUIRE(player.finished());
    REQUIRE(player.frame() == 11.0f);
}

TEST_CASE("a sequence's rate is 900 over its frames per second and frames snap to whole numbers",
          "[world][animation]") {
    const TreeSequenceInfo slow = sequence(150, 45); // twenty frames a second
    AnimationPlayer player;
    player.start(slow, 0);
    REQUIRE(player.secondsPerFrame() == Approx(0.05f));
    player.advance(kStep, false);
    REQUIRE(player.frame() == 1.0f); // two thirds of a frame rounds up
    player.advance(kStep, false);
    REQUIRE(player.frame() == 1.0f);
    player.advance(kStep, false);
    REQUIRE(player.frame() == 2.0f);

    // Smooth playback keeps the fraction when it is far from a whole frame.
    player.setSmooth(true);
    player.start(slow, 0);
    player.advance(kStep, false);
    REQUIRE(player.frame() == Approx(2.0f / 3.0f));
    player.advance(kStep, false);
    player.advance(kStep, false);
    REQUIRE(player.frame() == 2.0f);

    // A rate of 0 plays at thirty a second; a doubled speed halves the frame time.
    // Playback borrows the sequence, so the record must outlive every advance.
    const TreeSequenceInfo fallback = sequence(10, 0);
    const TreeSequenceInfo normal = sequence(10, 30);
    player.setSmooth(false);
    player.start(fallback, 0);
    REQUIRE(player.secondsPerFrame() == Approx(kStep));
    player.setSpeed(2.0f);
    player.start(normal, 0);
    REQUIRE(player.secondsPerFrame() == Approx(kStep / 2.0f));
    player.advance(kStep, false);
    REQUIRE(player.frame() == 2.0f);
}

TEST_CASE("long animation loops end on the same tick under split display updates",
          "[world][animation][asset-conformance][cadence]") {
    // Ninety frames is Sumner's standing loop. Single-precision accumulation
    // used to defer its loop notification until tick 91 even though frame 89 held.
    for (const s32 frames : {90, 900}) {
        const TreeSequenceInfo cycle = sequence(frames, 30);
        for (const s32 subdivisions : {1, 2, 4}) {
            CAPTURE(frames, subdivisions);
            AnimationPlayer player;
            player.start(cycle, 0);
            const f32 seconds = kStep / static_cast<f32>(subdivisions);
            for (s32 loop = 0; loop < 5; ++loop) {
                for (s32 step = 1; step < frames * subdivisions; ++step) {
                    REQUIRE_FALSE(player.advance(seconds, true));
                }
                REQUIRE(player.advance(seconds, true));
                REQUIRE(player.finished());
                CHECK(player.frame() == 0.0f);
            }
        }
    }
}

TEST_CASE("a transition holds the first frame while it blends in", "[world][animation]") {
    const TreeSequenceInfo cycle = sequence(12, 30);
    AnimationPlayer player;
    player.start(cycle, 0, 2.0f * kStep);
    REQUIRE(player.transitioning());
    REQUIRE(player.transition() == 0.0f);
    REQUIRE_FALSE(player.advance(kStep, true));
    REQUIRE(player.transition() == Approx(0.5f));
    REQUIRE(player.frame() == 0.0f);
    player.advance(kStep, true);
    REQUIRE_FALSE(player.transitioning());
    REQUIRE(player.transition() == 1.0f);
    REQUIRE(player.frame() == 0.0f);
    player.advance(kStep, true);
    REQUIRE(player.frame() == 1.0f);

    // Without a transition the blend is already complete.
    player.start(cycle, 0);
    REQUIRE_FALSE(player.transitioning());
    REQUIRE(player.transition() == 1.0f);
}

TEST_CASE("an empty sequence is finished at once and a stopped player plays nothing",
          "[world][animation]") {
    const TreeSequenceInfo empty = sequence(0, 30);
    AnimationPlayer player;
    player.start(empty, 0);
    REQUIRE_FALSE(player.advance(kStep, true));
    REQUIRE(player.finished());
    player.stop();
    REQUIRE_FALSE(player.playing());
    REQUIRE(player.frameCount() == 0);
    REQUIRE_FALSE(player.advance(kStep, true));
}

TEST_CASE("initial transition credit affects only the blend and never advances playback",
          "[world][animation][transition-credit]") {
    const auto cycle = sequence(12, 30);
    for (const f32 speed : {0.5f, 1.0f, 2.0f}) {
        CAPTURE(speed);
        AnimationPlayer player;
        player.setSpeed(speed);
        player.setSmooth(true);
        player.start(cycle, 0, 2.0f * kStep, 5.0f, kStep);
        const auto generation = player.generation();
        CHECK(player.frame() == 5.0f);
        CHECK(player.presentationFrame() == Approx(5.0f));
        CHECK(player.transition() == Approx(0.5f));
        CHECK_FALSE(player.finished());
        CHECK_FALSE(player.advance(kStep, false));
        CHECK_FALSE(player.transitioning());
        CHECK(player.frame() == 5.0f);
        CHECK(player.generation() == generation);
        CHECK_FALSE(player.advance(kStep, false));
        CHECK(player.frame() == Approx(5.0f + speed));
    }
    for (const f32 duration : {0.0f, kStep / 2.0f, kStep}) {
        CAPTURE(duration);
        AnimationPlayer player;
        player.start(cycle, 0, duration, 11.0f, kStep);
        CHECK_FALSE(player.transitioning());
        CHECK_FALSE(player.finished());
        CHECK(player.frame() == 11.0f);
        CHECK(player.presentationFrame() == Approx(11.0f));
    }
    AnimationPlayer player;
    player.start(cycle, 0, kStep, 0, -kStep);
    CHECK(player.transition() == 0.0f);
    player.start(cycle, 0, kStep);
    CHECK(player.transition() == 0.0f);
}

TEST_CASE("looping animation owns a full interval for its last frame and keeps overshoot",
          "[world][animation][cadence]") {
    const TreeSequenceInfo cycle = sequence(12, 30);
    AnimationPlayer player;
    player.start(cycle, 0);
    REQUIRE_FALSE(player.advance(11.5f / 30.0f, true));
    REQUIRE(player.frame() == 11.0f);
    REQUIRE(player.advance(2.5f / 30.0f, true));
    REQUIRE(player.frame() == 2.0f);
    REQUIRE(player.advance(26.0f / 30.0f, true));
    REQUIRE(player.frame() == 4.0f);
}

TEST_CASE("loop and transition clocks agree across update cadences",
          "[world][animation][cadence]") {
    const TreeSequenceInfo cycle = sequence(12, 30);
    for (const s32 rate : {30, 60, 144, 240}) {
        CAPTURE(rate);
        AnimationPlayer player;
        // Deliberately not a multiple of any update interval.
        player.start(cycle, 0, 0.073f);
        s32 wraps = 0;
        for (s32 frame = 0; frame < rate * 2; ++frame) {
            wraps += player.advance(1.0f / static_cast<f32>(rate), true) ? 1 : 0;
        }
        REQUIRE(wraps == 4);
        REQUIRE(player.transition() == 1.0f);
        REQUIRE(player.frame() == 10.0f);
    }
}

TEST_CASE("fractional presentation samples never advance rounded gameplay frames",
          "[world][animation][presentation]") {
    const TreeSequenceInfo cycle = sequence(12, 30);
    AnimationPlayer player;
    player.start(cycle, 0);
    const u64 generation = player.generation();
    player.advance(1.0f / 60.0f, true);
    REQUIRE(player.frame() == 1.0f);
    for (s32 draw = 0; draw < 8; ++draw) {
        REQUIRE(player.presentationFrame() == Approx(0.5f));
        REQUIRE(player.frame() == 1.0f);
        REQUIRE(player.generation() == generation);
        REQUIRE_FALSE(player.finished());
    }
    player.advance(11.5f / 30.0f, true);
    REQUIRE(player.generation() != generation);
    REQUIRE(player.presentationFrame() == Approx(0.0f).margin(0.00001f));
    const u64 wrapped = player.generation();
    player.start(cycle, 0, 0.1f);
    REQUIRE(player.generation() != wrapped);
    player.advance(0.05f, false);
    REQUIRE(player.presentationFrame() == 0.0f);
    player.advance(1.0f, false);
    REQUIRE(player.presentationFrame() == 11.0f);
    player.stop();
    REQUIRE(player.presentationFrame() == 0.0f);
}

} // namespace
