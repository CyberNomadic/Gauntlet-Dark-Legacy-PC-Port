#include <array>
#include <limits>
#include <numeric>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/rollback/NativeProbeTiming.h"

namespace {
using namespace gdl;

TEST_CASE("native timing summaries preserve samples and use nearest-rank percentiles",
          "[native-timing]") {
    std::array<f64, 100> samples{};
    std::iota(samples.rbegin(), samples.rend(), 1.0);
    const auto original = samples;
    const auto summary = test::summarizeNativeTimings(samples);
    REQUIRE(summary);
    CHECK(samples == original);
    CHECK(summary->samples == 100);
    CHECK(summary->mean == Catch::Approx(50.5));
    CHECK(summary->p50 == 50);
    CHECK(summary->p95 == 95);
    CHECK(summary->p99 == 99);
    CHECK(summary->maximum == 100);
}

TEST_CASE("native timing summaries handle single samples and retain tail spikes",
          "[native-timing]") {
    const auto single = test::summarizeNativeTimings(std::array{0.0});
    REQUIRE(single);
    CHECK(single->samples == 1);
    CHECK(single->mean == 0);
    CHECK(single->p50 == 0);
    CHECK(single->p95 == 0);
    CHECK(single->p99 == 0);
    CHECK(single->maximum == 0);
    const auto tail = test::summarizeNativeTimings(std::array{0.1, 10.0, 0.1, 0.1});
    REQUIRE(tail);
    CHECK(tail->mean == Catch::Approx(2.575));
    CHECK(tail->p50 == 0.1);
    CHECK(tail->p95 == 10);
    CHECK(tail->p99 == 10);
    CHECK(tail->maximum == 10);
}

TEST_CASE("native timing summaries reject empty and invalid measurements", "[native-timing]") {
    CHECK_FALSE(test::summarizeNativeTimings({}));
    for (const f64 invalid :
         {-1.0, std::numeric_limits<f64>::infinity(), std::numeric_limits<f64>::quiet_NaN()}) {
        CHECK_FALSE(test::summarizeNativeTimings(std::array{0.1, invalid}));
    }
}
} // namespace
