#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/enemies/SwarmAggression.h"
#include "game/players/MeleeStreak.h"

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
    pass(aggression, 60.0f, SwarmAggression::kFullRun + 30);
    CHECK(aggression.rage == 1.0f);
    CHECK(aggression.valid());
    // The run over, the heat cools at its own pace and the rage goes faster than it came.
    pass(aggression, 1.0f, 0);
    CHECK(aggression.heat == Approx(1.0f - SwarmAggression::kCool).margin(0.01f));
    CHECK(aggression.rage == Approx(1.0f - 1.0f / SwarmAggression::kCalmSeconds).margin(0.01f));
    pass(aggression, 30.0f, 0);
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
    SwarmAggression wrong;
    wrong.heat = 1.5f;
    CHECK_FALSE(wrong.valid());
    CHECK(SwarmAggression::kFullRun == MeleeStreak::kBravery);
}

} // namespace
