#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/players/TurboMeter.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("the turbo meter climbs by itself to full and says when it gets there",
          "[game][players][turbo]") {
    TurboMeter meter;
    REQUIRE(meter.held() == 0.0f);
    REQUIRE_FALSE(meter.fill(10.0f)); // two a second
    REQUIRE(meter.held() == Approx(20.0f));
    REQUIRE_FALSE(meter.fill(39.0f));
    REQUIRE(meter.held() == Approx(98.0f));
    REQUIRE(meter.fill(1.0f)); // the moment it comes full
    REQUIRE(meter.held() == TurboMeter::kFull);
    REQUIRE_FALSE(meter.fill(5.0f)); // and only then
    REQUIRE(meter.held() == TurboMeter::kFull);
}

TEST_CASE("turbo moves are paid for out of the meter, a shove by the second",
          "[game][players][turbo]") {
    TurboMeter meter;
    meter.add(60.0f);
    REQUIRE_FALSE(meter.spend(TurboMeter::kFullCost));
    REQUIRE(meter.held() == 60.0f);
    REQUIRE(meter.spend(TurboMeter::kStrongCost));
    REQUIRE(meter.held() == 20.0f);
    REQUIRE_FALSE(meter.spend(TurboMeter::kStrongCost));
    meter.drain(0.5f); // twenty a second
    REQUIRE(meter.held() == Approx(10.0f));
    meter.drain(5.0f);
    REQUIRE(meter.held() == 0.0f);
    meter.add(500.0f);
    REQUIRE(meter.held() == TurboMeter::kFull);
    REQUIRE(meter.spend(TurboMeter::kFullCost));
    REQUIRE(meter.held() == 0.0f);
    meter.add(30.0f);
    meter.reset();
    REQUIRE(meter.held() == 0.0f);
    REQUIRE(meter.shown() == 0.0f);
}

TEST_CASE("what the meter shows chases what it holds, a point a tick up and two down",
          "[game][players][turbo]") {
    TurboMeter meter;
    meter.add(50.0f);
    meter.step(2);
    REQUIRE(meter.shown() == 2.0f);
    meter.step(100);
    REQUIRE(meter.shown() == 50.0f);
    REQUIRE(meter.spend(TurboMeter::kStrongCost));
    meter.step(2);
    REQUIRE(meter.shown() == 46.0f);
    meter.step(100);
    REQUIRE(meter.shown() == 10.0f);
}

TEST_CASE("the meter reads yellow on black, then red on yellow, then red and glowing",
          "[game][players][turbo]") {
    REQUIRE(TurboMeter::zoneOf(0.0f) == TurboMeter::Zone::Low);
    REQUIRE(TurboMeter::zoneOf(39.9f) == TurboMeter::Zone::Low);
    REQUIRE(TurboMeter::zoneOf(40.0f) == TurboMeter::Zone::High);
    REQUIRE(TurboMeter::zoneOf(98.9f) == TurboMeter::Zone::High);
    REQUIRE(TurboMeter::zoneOf(99.0f) == TurboMeter::Zone::Full);
    REQUIRE(TurboMeter::zoneFraction(20.0f) == Approx(0.5f));
    REQUIRE(TurboMeter::zoneFraction(70.0f) == Approx(0.5f));
    REQUIRE(TurboMeter::zoneFraction(100.0f) == 1.0f);

    TurboMeter meter;
    meter.add(20.0f);
    meter.step(40);
    TurboMeterLook look = meter.look();
    REQUIRE(look.fill == Approx(0.5f));
    REQUIRE(look.front == Color::rgba(191, 191, 0)); // half way to its brightest
    REQUIRE(look.back == Color::black());
    REQUIRE(look.glow == 0);
    REQUIRE(look.gleam == -1);

    // Into the second zone it gleams: five frames up and back, four ticks each.
    meter.add(30.0f);
    meter.step(40);
    REQUIRE(meter.zone() == TurboMeter::Zone::High);
    REQUIRE(meter.flash() == TurboMeter::Flash::Gleam);
    REQUIRE(meter.look().gleam == 0);
    meter.step(8);
    REQUIRE(meter.look().gleam == 2);
    meter.step(10);
    REQUIRE(meter.look().gleam == 4);
    meter.step(2);
    REQUIRE(meter.look().gleam == 4); // the top frame twice, on the turn
    meter.step(16);
    REQUIRE(meter.look().gleam == 0);
    meter.step(4);
    REQUIRE(meter.flash() == TurboMeter::Flash::None);
    look = meter.look();
    REQUIRE(look.gleam == -1);
    REQUIRE(look.front.r > 128);
    REQUIRE(look.front.g == 0);
    REQUIRE(look.back == Color::rgba(255, 255, 0));

    // Full, it is red and its glow pulses bright, out and bright again for as long as it is.
    meter.add(100.0f);
    meter.step(100);
    REQUIRE(meter.zone() == TurboMeter::Zone::Full);
    REQUIRE(meter.flash() == TurboMeter::Flash::Glow);
    look = meter.look();
    REQUIRE(look.fill == 1.0f);
    REQUIRE(look.front == Color::rgba(255, 0, 0));
    REQUIRE(look.back == look.front);
    REQUIRE(look.glow == 255);
    meter.step(TurboMeter::kGlowTicks / 2);
    REQUIRE(meter.look().glow < 8);
    meter.step(TurboMeter::kGlowTicks / 2);
    REQUIRE(meter.look().glow > 247);
    meter.step(TurboMeter::kGlowTicks);
    REQUIRE(meter.flash() == TurboMeter::Flash::Glow);
    // Spent, it gleams on its way down.
    REQUIRE(meter.spend(TurboMeter::kFullCost));
    meter.step(2);
    REQUIRE(meter.flash() == TurboMeter::Flash::Gleam);
    REQUIRE(meter.look().glow == 0);
}

TEST_CASE("turbo meter checkpoints preserve charge, display lag and every flash phase",
          "[game][players][turbo][rollback][turbo-checkpoint]") {
    TurboMeter meter;
    for (s32 tick = 0; tick < 900; ++tick) {
        if (tick == 10 || tick == 300 || tick == 650) {
            meter.add(100);
        }
        if (tick == 200 || tick == 550) {
            meter.spend(100);
        }
        const auto state = meter.checkpoint();
        REQUIRE(TurboMeter::valid(state));
        TurboMeter replayed;
        REQUIRE(replayed.restore(state));
        REQUIRE(replayed.checkpoint() == state);
        CHECK(replayed.look().fill == meter.look().fill);
        CHECK(replayed.look().front == meter.look().front);
        CHECK(replayed.look().back == meter.look().back);
        CHECK(replayed.look().glow == meter.look().glow);
        CHECK(replayed.look().gleam == meter.look().gleam);
        CHECK(replayed.fill(1.0f / 60) == meter.fill(1.0f / 60));
        replayed.step(1);
        meter.step(1);
        CHECK(replayed.checkpoint() == meter.checkpoint());
    }
    TurboMeter::Checkpoint idle;
    idle.flashTicks = 1000000; // Native idle counts after a gleam finishes.
    REQUIRE(meter.restore(idle));
    CHECK(meter.checkpoint() == idle);
}

TEST_CASE("turbo meter malformed checkpoints leave live state unchanged",
          "[game][players][turbo][rollback][turbo-checkpoint]") {
    TurboMeter meter;
    meter.add(70);
    meter.step(50);
    const auto original = meter.checkpoint();
    for (const f32 value : {-1.0f, 100.01f, std::numeric_limits<f32>::quiet_NaN(),
                            std::numeric_limits<f32>::infinity()}) {
        for (const bool held : {false, true}) {
            auto invalid = original;
            (held ? invalid.held : invalid.shown) = value;
            CHECK_FALSE(TurboMeter::valid(invalid));
            CHECK_FALSE(meter.restore(invalid));
            CHECK(meter.checkpoint() == original);
        }
    }
    for (const auto invalid :
         {TurboMeter::Checkpoint{0, 0, static_cast<TurboMeter::Flash>(255), 0},
          TurboMeter::Checkpoint{0, 0, TurboMeter::Flash::None, -1},
          TurboMeter::Checkpoint{0, 0, TurboMeter::Flash::None, std::numeric_limits<s32>::max()},
          TurboMeter::Checkpoint{100, 100, TurboMeter::Flash::Gleam, 0},
          TurboMeter::Checkpoint{0, 0, TurboMeter::Flash::Gleam, 40},
          TurboMeter::Checkpoint{0, 0, TurboMeter::Flash::Glow, 0},
          TurboMeter::Checkpoint{100, 100, TurboMeter::Flash::Glow, 121}}) {
        CHECK_FALSE(meter.restore(invalid));
        CHECK(meter.checkpoint() == original);
    }
    REQUIRE(meter.restore({100, 100, TurboMeter::Flash::Glow, TurboMeter::kGlowTicks}));
    meter.step(1);
    CHECK(meter.checkpoint().flashTicks == 0);
}

} // namespace
