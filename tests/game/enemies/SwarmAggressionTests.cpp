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
    CHECK(aggression.surge == 0.0f);
    // A run halfway to S heats the swarm halfway, at the rise's pace.
    constexpr s32 kHalfRun = SwarmAggression::kFullRun / 2;
    pass(aggression, 0.5f, kHalfRun);
    CHECK(aggression.heat == Approx(0.25f).margin(0.01f));
    pass(aggression, 1.0f, kHalfRun);
    const f32 half = static_cast<f32>(kHalfRun) / static_cast<f32>(SwarmAggression::kFullRun);
    CHECK(aggression.heat == Approx(half).margin(0.01f));
    CHECK(aggression.surge == 0.0f);
    // At S the heat is full and stays so.
    pass(aggression, 2.0f, SwarmAggression::kFullRun);
    CHECK(aggression.run == SwarmAggression::kFullRun);
    CHECK(aggression.heat == 1.0f);
    CHECK(aggression.surge == 0.0f);
    CHECK(aggression.frenzied());
    // Past S each kill adds a hundredth of a percent, at once, with no end to it.
    pass(aggression, 1.0f, SwarmAggression::kFullRun + 30);
    CHECK(aggression.heat == 1.0f);
    CHECK(aggression.surge == Approx(30.0f * SwarmAggression::kSurgePerKill));
    pass(aggression, 1.0f, SwarmAggression::kFullRun + 1000);
    CHECK(aggression.surge == Approx(1000.0f * SwarmAggression::kSurgePerKill));
    CHECK(aggression.valid());
    // A shorter run still under way eases the heat down and ends the surge at once.
    aggression.step(1.0f / 60.0f, SwarmAggression::kFullRun - 1);
    CHECK(aggression.heat == Approx(1.0f - SwarmAggression::kCool / 60.0f).margin(0.001f));
    CHECK(aggression.surge == 0.0f);
    CHECK(aggression.frenzied()); // any run under way
    // The run over, the swarm is itself again at once.
    pass(aggression, 1.0f, SwarmAggression::kFullRun + 30);
    REQUIRE(aggression.surge > 0.0f);
    aggression.step(1.0f / 60.0f, 0);
    CHECK(aggression.run == 0);
    CHECK(aggression.heat == 0.0f);
    CHECK(aggression.surge == 0.0f);
    CHECK_FALSE(aggression.frenzied());
    CHECK(aggression.elementalChance() == 0.0f);
    // A frame that takes no time changes nothing.
    aggression.step(0.0f, SwarmAggression::kFullRun);
    CHECK(aggression.heat == 0.0f);
}

TEST_CASE("the swarm's measures, colour and aura grow with the run and surge on past S",
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
    // Halfway to S every measure stands halfway, the colour and the aura with them.
    SwarmAggression warm;
    warm.heat = 0.5f;
    CHECK(warm.breedingPace() == Approx(1.0f + 0.5f * SwarmAggression::kBreedingFull));
    CHECK(warm.healthScale() == Approx(1.0f + 0.5f * SwarmAggression::kHealthFull));
    CHECK(warm.speedScale() == Approx(1.3f));
    CHECK(warm.attackPace() == Approx(1.4f));
    CHECK(warm.aura() == 0.5f);
    const Color tint = warm.tint();
    CHECK(tint.r == 210);
    CHECK(tint.g == 143);
    CHECK(tint.b == 143);
    CHECK(tint.a == 255);
    // At S every measure is full, the colour the rage's red, the aura whole.
    SwarmAggression hot;
    hot.heat = 1.0f;
    hot.run = SwarmAggression::kFullRun;
    CHECK(hot.breedingPace() == Approx(2.6f));
    CHECK(hot.broodScale() == Approx(2.5f));
    CHECK(hot.speedScale() == Approx(1.6f));
    CHECK(hot.attackPace() == Approx(1.8f));
    CHECK(hot.healthScale() == Approx(2.4f));
    CHECK(hot.tint() == SwarmAggression::kRageTint);
    CHECK(hot.aura() == 1.0f);
    CHECK(hot.frenzied());
    // Past S the surge adds to every measure alike; the colour and the aura stay full.
    SwarmAggression beyond;
    beyond.heat = 1.0f;
    beyond.surge = 0.5f;
    CHECK(beyond.valid());
    CHECK(beyond.breedingPace() == Approx(3.1f));
    CHECK(beyond.healthScale() == Approx(2.9f));
    CHECK(beyond.broodScale() == Approx(3.0f));
    CHECK(beyond.speedScale() == Approx(2.1f));
    CHECK(beyond.attackPace() == Approx(2.3f));
    CHECK(beyond.tint() == SwarmAggression::kRageTint);
    CHECK(beyond.aura() == 1.0f);
    // The paces stop at five times, however long the run; the brood and the health do not.
    SwarmAggression extreme;
    extreme.heat = 1.0f;
    extreme.surge = 10.0f;
    CHECK(extreme.breedingPace() == Approx(SwarmAggression::kBreedingCap));
    CHECK(extreme.speedScale() == Approx(SwarmAggression::kSpeedCap));
    CHECK(extreme.attackPace() == Approx(SwarmAggression::kAttackCap));
    CHECK(extreme.healthScale() == Approx(12.4f));
    CHECK(extreme.broodScale() == Approx(12.5f));
    // The odds of an elemental one: one in fifty at D, one in ten at S, none without a run.
    SwarmAggression running;
    CHECK(running.elementalChance() == 0.0f);
    running.run = 1;
    CHECK(running.elementalChance() == Approx(0.02f));
    running.run = 75;
    CHECK(running.elementalChance() == Approx(0.04f));
    running.run = 249;
    CHECK(running.elementalChance() == Approx(0.08f));
    running.run = 250;
    CHECK(running.elementalChance() == Approx(0.10f));
    running.run = 500;
    CHECK(running.elementalChance() == Approx(0.10f));
    CHECK(running.frenzied());
    SwarmAggression wrong;
    wrong.heat = -1.0f;
    CHECK_FALSE(wrong.valid());
    wrong.heat = 0.0f;
    wrong.run = -1;
    CHECK_FALSE(wrong.valid());
    wrong.heat = 2.0f; // past S the heat holds at one; the surge carries the rest
    CHECK_FALSE(wrong.valid());
    wrong.heat = 1.0f;
    wrong.surge = SwarmAggression::kMostMeasure * 2.0f;
    CHECK_FALSE(wrong.valid());
    CHECK(SwarmAggression::kFullRun == StreakGrades::kTop);
}

} // namespace
