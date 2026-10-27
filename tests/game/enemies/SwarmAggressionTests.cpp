#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/enemies/SwarmAggression.h"
#include "game/players/MeleeStreak.h"
#include "game/players/StreakGrades.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

void pass(SwarmAggression& aggression, f32 seconds, s32 run) {
    const auto frames = static_cast<s32>(std::lround(seconds * 60.0f));
    for (s32 frame = 0; frame < frames; ++frame) {
        aggression.step(1.0f / 60.0f, run);
    }
}

TEST_CASE("the swarm's heat climbs toward the run under way and cools once it is over",
          "[game][enemies][streak][aggression]") {
    SwarmAggression aggression;
    CHECK(aggression.valid());
    CHECK(aggression.heat == 0.0f);
    CHECK(aggression.rage == 0.0f);
    // A run halfway to the bravery line heats the swarm halfway, at the rise's pace.
    constexpr s32 kHalfRun = SwarmAggression::kFullRun / 2;
    pass(aggression, 0.5f, kHalfRun);
    CHECK(aggression.heat == Approx(0.25f).margin(0.01f));
    pass(aggression, 1.0f, kHalfRun);
    const f32 half = static_cast<f32>(kHalfRun) / static_cast<f32>(SwarmAggression::kFullRun);
    CHECK(aggression.heat == Approx(half).margin(0.01f));
    CHECK(aggression.rage == 0.0f);
    // At the line the heat is full, and the rage comes slowly.
    pass(aggression, 2.0f, SwarmAggression::kFullRun);
    CHECK(aggression.heat == Approx(1.0f).margin(0.01f));
    CHECK(aggression.rage == Approx(2.0f / SwarmAggression::kRageSeconds).margin(0.01f));
    CHECK(aggression.frenzied());
    // Past the line there is no end to either: the heat follows the run on, the rage grows.
    pass(aggression, 60.0f, SwarmAggression::kFullRun + 30);
    const f32 beyond = static_cast<f32>(SwarmAggression::kFullRun + 30) /
                       static_cast<f32>(SwarmAggression::kFullRun);
    CHECK(aggression.heat == Approx(beyond).margin(0.01f));
    CHECK(aggression.rage == Approx(0.1f + 60.0f / SwarmAggression::kRageSeconds).margin(0.02f));
    CHECK(aggression.valid());
    // A shorter run still under way eases the heat down and ends the rage at once.
    aggression.step(1.0f / 60.0f, SwarmAggression::kFullRun - 1);
    CHECK(aggression.heat == Approx(beyond - SwarmAggression::kCool / 60.0f).margin(0.001f));
    CHECK(aggression.rage == 0.0f);
    CHECK_FALSE(aggression.frenzied());
    // The run over, the swarm is itself again at once.
    pass(aggression, 60.0f, SwarmAggression::kFullRun + 30);
    REQUIRE(aggression.rage > 1.0f);
    aggression.step(1.0f / 60.0f, 0);
    CHECK(aggression.heat == 0.0f);
    CHECK(aggression.rage == 0.0f);
    CHECK_FALSE(aggression.frenzied());
    // A frame that takes no time changes nothing.
    aggression.step(0.0f, SwarmAggression::kFullRun);
    CHECK(aggression.heat == 0.0f);
}

TEST_CASE("the swarm's measures grow with the heat and the rage within sane bounds",
          "[game][enemies][streak][aggression]") {
    const SwarmAggression calm;
    CHECK(calm.breedingPace() == 1.0f);
    CHECK(calm.broodScale() == 1.0f);
    CHECK(calm.speedScale() == 1.0f);
    CHECK(calm.attackPace() == 1.0f);
    CHECK(calm.healthScale() == 1.0f);
    CHECK(calm.tint() == Color::white());
    CHECK(calm.aura() == 0.0f);
    CHECK_FALSE(calm.frenzied());
    SwarmAggression warm;
    warm.heat = 0.5f;
    CHECK(warm.breedingPace() == Approx(1.0f + 0.5f * SwarmAggression::kBreedingHeat));
    CHECK(warm.healthScale() == Approx(1.0f + 0.5f * SwarmAggression::kHealthHeat));
    CHECK(warm.tint() == Color::white()); // the colour is the rage's alone
    SwarmAggression hot;
    hot.heat = 1.0f;
    hot.rage = 1.0f;
    CHECK(hot.breedingPace() == Approx(2.6f));
    CHECK(hot.broodScale() == Approx(2.5f));
    CHECK(hot.speedScale() == Approx(1.6f));
    CHECK(hot.attackPace() == Approx(1.8f));
    CHECK(hot.healthScale() == Approx(2.4f));
    SwarmAggression mild;
    mild.heat = 0.5f;
    CHECK(mild.breedingPace() == Approx(1.6f));
    CHECK(mild.speedScale() == Approx(1.2f));
    CHECK(mild.attackPace() == Approx(1.25f));
    CHECK(hot.tint() == SwarmAggression::kRageTint);
    CHECK(hot.aura() == 1.0f);
    CHECK(hot.frenzied());
    SwarmAggression halfway;
    halfway.heat = 1.0f;
    halfway.rage = 0.5f;
    const Color tint = halfway.tint();
    CHECK(tint.r == 210);
    CHECK(tint.g == 143);
    CHECK(tint.b == 143);
    CHECK(tint.a == 255);
    // Beyond the line the measures keep growing; the colour and the aura are full.
    SwarmAggression beyond;
    beyond.heat = 3.0f;
    beyond.rage = 2.0f;
    CHECK(beyond.valid());
    CHECK(beyond.breedingPace() == Approx(SwarmAggression::kBreedingCap)); // 5.4 capped
    CHECK(beyond.healthScale() == Approx(4.6f));
    CHECK(beyond.broodScale() == Approx(5.0f));
    CHECK(beyond.speedScale() == Approx(2.6f));
    CHECK(beyond.attackPace() == Approx(3.1f));
    // The paces stop at five times, however long the run; the brood and the health do not.
    SwarmAggression extreme;
    extreme.heat = 20.0f;
    extreme.rage = 10.0f;
    CHECK(extreme.breedingPace() == Approx(SwarmAggression::kBreedingCap));
    CHECK(extreme.speedScale() == Approx(SwarmAggression::kSpeedCap));
    CHECK(extreme.attackPace() == Approx(SwarmAggression::kAttackCap));
    CHECK(extreme.healthScale() == Approx(23.0f));
    CHECK(extreme.broodScale() == Approx(26.0f));
    CHECK(beyond.tint() == SwarmAggression::kRageTint);
    CHECK(beyond.aura() == 1.0f);
    SwarmAggression wrong;
    wrong.heat = -1.0f;
    CHECK_FALSE(wrong.valid());
    wrong.heat = SwarmAggression::kMostMeasure * 2.0f;
    CHECK_FALSE(wrong.valid());
    CHECK(SwarmAggression::kFullRun == StreakGrades::kTop);
}

} // namespace
