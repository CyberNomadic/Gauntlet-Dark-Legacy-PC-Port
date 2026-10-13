#include <array>
#include <bit>
#include <limits>
#include <random>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/math/Mt19937.h"

namespace {

using namespace gdl;

TEST_CASE("portable MT19937 matches standard raw draws across seeded twist boundaries",
          "[math][mt19937]") {
    const u32 seed = GENERATE(0U, 1U, 5489U, 0x80000000U, 0xffffffffU);
    CAPTURE(seed);
    Mt19937 actual(seed);
    std::mt19937 expected(seed);
    STATIC_REQUIRE(std::uniform_random_bit_generator<Mt19937>);
    STATIC_REQUIRE(Mt19937::min() == std::mt19937::min());
    STATIC_REQUIRE(Mt19937::max() == std::mt19937::max());
    REQUIRE(actual.checkpoint().valid());
    REQUIRE(actual.checkpoint().cursor == 624);

    for (u32 draw = 0; draw < 5000; ++draw) {
        CAPTURE(draw);
        REQUIRE(actual() == expected());
        CHECK(actual.checkpoint().cursor == draw % 624 + 1);
    }

    actual.seed(seed);
    expected.seed(seed);
    for (u32 draw = 0; draw < 625; ++draw) {
        REQUIRE(actual() == expected());
    }
}

TEST_CASE("portable MT19937 retains published default seed word fixtures", "[math][mt19937]") {
    constexpr std::array<u32, 10> kFirst{
        3499211612U, 581869302U,  3890346734U, 3586334585U, 545404204U,
        4161255391U, 3922919429U, 949333985U,  2715962298U, 1323567403U,
    };
    Mt19937 random;
    for (const u32 word : kFirst) {
        CHECK(random() == word);
    }
    // The 10000th output is the required standard MT19937 reference value.
    for (usize draw = kFirst.size(); draw < 9999; ++draw) {
        random();
    }
    CHECK(random() == 4123659995U);

    random.seed();
    CHECK(random() == kFirst.front());
}

TEST_CASE("portable MT19937 checkpoints resume exact value state around every twist phase",
          "[math][mt19937]") {
    const u32 draws =
        GENERATE(0U, 1U, 226U, 227U, 396U, 397U, 623U, 624U, 625U, 1247U, 1248U, 4097U);
    CAPTURE(draws);
    Mt19937 source(131071U);
    std::mt19937 oracle(131071U);
    for (u32 draw = 0; draw < draws; ++draw) {
        REQUIRE(source() == oracle());
    }

    const auto saved = source.checkpoint();
    REQUIRE(saved.valid());
    STATIC_REQUIRE(std::is_trivially_copyable_v<Mt19937::Checkpoint>);

    // Copy the specified scalar fields rather than relying on an STL engine's memory
    // layout, stream operators, padding, native endian bytes or compiler ABI.
    Mt19937::Checkpoint value;
    value.words = saved.words;
    value.cursor = saved.cursor;
    Mt19937 restored(17U);
    REQUIRE(restored.restore(value));
    REQUIRE(restored.checkpoint() == saved);
    for (u32 draw = 0; draw < 2 * Mt19937::state_size + 1; ++draw) {
        CAPTURE(draw);
        const u32 expected = oracle();
        REQUIRE(source() == expected);
        REQUIRE(restored() == expected);
    }
    CHECK(restored.checkpoint() == source.checkpoint());
}

TEST_CASE("portable MT19937 refuses malformed state without consuming or replacing valid state",
          "[math][mt19937]") {
    Mt19937 random(47U);
    for (u32 draw = 0; draw < 623; ++draw) {
        random();
    }
    const auto before = random.checkpoint();
    auto malformed = before;
    SECTION("cursor just beyond the block") {
        malformed.cursor = 625;
    }
    SECTION("cursor overflow") {
        malformed.cursor = std::numeric_limits<u32>::max();
    }
    SECTION("all-zero state") {
        malformed.words.fill(0);
    }
    SECTION("only discarded bits survive") {
        malformed.words.fill(0);
        malformed.words[0] = 0x7fffffffU;
    }
    CHECK_FALSE(malformed.valid());
    CHECK_FALSE(random.restore(malformed));
    REQUIRE(random.checkpoint() == before);

    Mt19937 control;
    REQUIRE(control.restore(before));
    for (u32 draw = 0; draw < 625; ++draw) {
        REQUIRE(random() == control());
    }
}

TEST_CASE("portable MT19937 float mapping has fixed binary32 endpoint and rounding fixtures",
          "[math][mt19937]") {
    struct Sample {
        u32 word;
        u32 bits;
    };
    constexpr std::array<Sample, 9> kSamples{{
        {0x00000000U, 0x00000000U},
        {0x000000ffU, 0x00000000U},
        {0x00000100U, 0x33800000U},
        {0x000001ffU, 0x33800000U},
        {0x7fffffffU, 0x3efffffeU},
        {0x80000000U, 0x3f000000U},
        {0x80000100U, 0x3f000001U},
        {0xffffff00U, 0x3f7fffffU},
        {0xffffffffU, 0x3f7fffffU},
    }};
    for (const auto& sample : kSamples) {
        CAPTURE(sample.word);
        const f32 value = Mt19937::float24(sample.word);
        CHECK(std::bit_cast<u32>(value) == sample.bits);
        CHECK(value >= 0.0f);
        CHECK(value < 1.0f);
    }
}

TEST_CASE("portable MT19937 float draws consume exactly one word across restore and twists",
          "[math][mt19937]") {
    Mt19937 floats(0U);
    Mt19937 words(0U);
    for (u32 draw = 0; draw < 1250; ++draw) {
        const auto before = floats.checkpoint();
        const u32 word = words();
        const f32 value = floats.unitFloat24();
        REQUIRE(std::bit_cast<u32>(value) == std::bit_cast<u32>(Mt19937::float24(word)));
        REQUIRE(floats.checkpoint() == words.checkpoint());
        REQUIRE(floats.restore(before));
        REQUIRE(std::bit_cast<u32>(floats.unitFloat24()) == std::bit_cast<u32>(value));
        REQUIRE(floats.checkpoint() == words.checkpoint());
    }
}

} // namespace
