#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/MeleeStreak.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("a run of close blows earns the narrator's praise once it pauses",
          "[game][players][streak]") {
    MeleeStreak streak;
    CHECK(streak.step(5.0f).empty());
    // Twenty-nine hurts are not enough.
    for (s32 i = 0; i < 29; ++i) {
        streak.record(false);
        CHECK(streak.step(0.5f).empty());
    }
    CHECK(streak.step(3.1f).empty());
    CHECK(streak.count() == 0);
    // Thirty are heroic, but only once three seconds have passed without another.
    for (s32 i = 0; i < 30; ++i) {
        streak.record(false);
    }
    CHECK(streak.step(1.5f).empty());
    CHECK(streak.step(1.4f).empty());
    CHECK(streak.step(0.2f) == MeleeStreak::kHeroicVoice);
    CHECK(streak.count() == 0);
    // Kills count three: fifteen of them are bravery.
    for (s32 i = 0; i < 15; ++i) {
        streak.record(true);
    }
    CHECK(streak.count() == 45);
    CHECK(streak.step(3.5f) == MeleeStreak::kBraveryVoice);
    CHECK(streak.step(3.5f).empty());
}

TEST_CASE("a blow that is not counted gives a run under way a second of its quiet back",
          "[game][players][streak]") {
    MeleeStreak streak;
    streak.refresh(); // no run: nothing
    CHECK_FALSE(streak.running());
    CHECK(streak.quiet() == 0.0f);
    streak.record(false);
    CHECK(streak.step(2.0f).empty());
    REQUIRE(streak.quiet() == 2.0f);
    streak.refresh();
    CHECK(streak.quiet() == Approx(2.0f - MeleeStreak::kRefresh));
    CHECK(streak.count() == 1); // not counted
    streak.refresh();
    streak.refresh();
    CHECK(streak.quiet() == 0.0f); // never below nought
    CHECK(streak.step(MeleeStreak::kQuiet + 0.1f).empty());
    CHECK(streak.count() == 0);
}

} // namespace
