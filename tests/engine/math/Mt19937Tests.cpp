#include <array>
#include <bit>
#include <limits>
#include <random>
#include <span>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/math/Mt19937.h"
#include "engine/math/RandomDistribution.h"

namespace {

using namespace gdl;

template <u32 Minimum, u32 Maximum, class Word = u32> struct ScriptedRandom {
    using result_type = Word;
    std::span<const u32> words;
    usize draws = 0;

    static constexpr result_type min() { return Minimum; }
    static constexpr result_type max() { return Maximum; }
    result_type operator()() {
        REQUIRE(draws < words.size());
        return words[draws++];
    }
};

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

TEST_CASE("portable index mapping retains known MT words and exact draw positions",
          "[math][mt19937][random-distribution]") {
    const u64 count = GENERATE(4ULL, 10ULL);
    constexpr std::array<u32, 10> kFour{3, 0, 3, 3, 0, 3, 3, 0, 2, 1};
    constexpr std::array<u32, 10> kTen{8, 1, 9, 8, 1, 9, 9, 2, 6, 3};
    const auto& expected = count == 4 ? kFour : kTen;
    Mt19937 actual;
    Mt19937 words;
    for (usize index = 0; index < expected.size(); ++index) {
        CAPTURE(count, index);
        REQUIRE(random_distribution::boundedIndex(actual, count) == expected[index]);
        words();
        CHECK(actual.checkpoint() == words.checkpoint());
    }
}

TEST_CASE("portable index bounds preserve invalid singleton and full-width draw semantics",
          "[math][random-distribution]") {
    constexpr std::array<u32, 4> kWords{42, 0, 1, 0xffffffffU};
    ScriptedRandom<0, 0xffffffffU> random{kWords};
    REQUIRE_FALSE(random_distribution::boundedIndex(random, 0));
    REQUIRE_FALSE(random_distribution::boundedIndex(random, (u64{1} << 32) + 1));
    REQUIRE(random.draws == 0);
    REQUIRE(random_distribution::boundedIndex(random, 1) == 0U);
    REQUIRE(random.draws == 1);
    for (usize index = 1; index < kWords.size(); ++index) {
        REQUIRE(random_distribution::boundedIndex(random, u64{1} << 32) == kWords[index]);
        CHECK(random.draws == index + 1);
    }
}

TEST_CASE("portable index rejection consumes each refused raw word or biased candidate",
          "[math][random-distribution]") {
    SECTION("full-width candidate rejection") {
        constexpr std::array<u32, 2> kWords{0, 1};
        ScriptedRandom<0, 0xffffffffU> random{kWords};
        REQUIRE(random_distribution::boundedIndex(random, 10) == 0U);
        CHECK(random.draws == 2);
    }
    SECTION("near-half-range rejection and accepted threshold equality") {
        constexpr std::array<u32, 3> kWords{2, 0, 0xffffffffU};
        ScriptedRandom<0, 0xffffffffU> random{kWords};
        REQUIRE(random_distribution::boundedIndex(random, 0x80000001ULL) == 0x80000000U);
        CHECK(random.draws == 3);
    }
    SECTION("minstd range rejection precedes multiply reduction") {
        constexpr std::array<u32, 4> kWords{2147483646U, 1073741825U, 1U, 2U};
        ScriptedRandom<1, 2147483646U> random{kWords};
        REQUIRE(random_distribution::boundedIndex(random, 7) == 0U);
        CHECK(random.draws == 4);
    }
}

TEST_CASE("portable index concatenates narrow sources with specified low-word arithmetic",
          "[math][random-distribution]") {
    constexpr std::array<u32, 11> kWords{12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12};
    SECTION("count larger than one source sample") {
        ScriptedRandom<5, 12> random{kWords};
        REQUIRE(random_distribution::boundedIndex(random, 10) == 9U);
        CHECK(random.draws == 2);
    }
    SECTION("full u32 range truncates the 33rd concatenated bit") {
        ScriptedRandom<5, 12> random{kWords};
        REQUIRE(random_distribution::boundedIndex(random, u64{1} << 32) == 0xffffffffU);
        CHECK(random.draws == 11);
    }
    SECTION("wider result storage never changes the declared raw range mapping") {
        ScriptedRandom<5, 12, u64> random{kWords};
        REQUIRE(random_distribution::boundedIndex(random, u64{1} << 32) == 0xffffffffU);
        CHECK(random.draws == 11);
    }
}

TEST_CASE("portable minstd index mapping has fixed output and raw-engine position fixtures",
          "[math][random-distribution]") {
    struct Sample {
        u32 index;
        u32 draws;
    };
    // minstd_rand seed 1, multiplier 48271, modulus 2147483647. Rejected source
    // samples are observable stream positions, not an implementation detail.
    constexpr std::array<Sample, 12> kSamples{{
        {0, 1},
        {1, 2},
        {2, 6},
        {5, 8},
        {3, 9},
        {1, 11},
        {4, 21},
        {5, 22},
        {2, 27},
        {4, 30},
        {5, 32},
        {4, 33},
    }};
    std::minstd_rand actual(1);
    std::minstd_rand raw(1);
    u32 draws = 0;
    for (const auto& expected : kSamples) {
        CAPTURE(expected.index, expected.draws);
        REQUIRE(random_distribution::boundedIndex(actual, 7) == expected.index);
        raw.discard(expected.draws - draws);
        draws = expected.draws;
        CHECK(actual == raw);
    }
}

#if defined(_MSVC_STL_VERSION)
// The fixed fixtures above are the portable contract. This platform-specific
// oracle additionally pins preservation of the existing Windows-native streams;
// other standard libraries are allowed to implement their distributions differently.
TEST_CASE("portable index mapping preserves current MSVC native engine streams",
          "[math][random-distribution]") {
    const u32 seed = GENERATE(0U, 1U, 0xf00dU);
    const u64 count = GENERATE(1ULL, 2ULL, 3ULL, 4ULL, 7ULL, 10ULL, 0x40000000ULL, 0x40000001ULL,
                               0x80000001ULL, 0xffffffffULL, 0x100000000ULL);
    CAPTURE(seed, count);
    std::uniform_int_distribution<u32> oracle(0, static_cast<u32>(count - 1));
    std::mt19937 mt(seed);
    auto mtOracle = mt;
    std::minstd_rand linear(seed);
    auto linearOracle = linear;
    for (u32 index = 0; index < 512; ++index) {
        CAPTURE(index);
        REQUIRE(random_distribution::boundedIndex(mt, count) == oracle(mtOracle));
        REQUIRE(mt == mtOracle);
        REQUIRE(random_distribution::boundedIndex(linear, count) == oracle(linearOracle));
        REQUIRE(linear == linearOracle);
    }
}

TEST_CASE("portable real mapping preserves native boss and debris intervals and streams",
          "[math][mt19937][random-distribution]") {
    const u32 seed = GENERATE(0U, 0xc01eU, 0x534b4f52U);
    const f32 spread = GENERATE(0.1f, 10.0f, 15.0f, 100.0f);
    CAPTURE(seed, spread);
    std::mt19937 actual(seed);
    auto expected = actual;
    std::uniform_real_distribution<f32> oracle(0.0f, spread);
    for (u32 index = 0; index < 1024; ++index) {
        const f32 value = Mt19937::float24(static_cast<u32>(actual())) * spread;
        CHECK(std::bit_cast<u32>(value) == std::bit_cast<u32>(oracle(expected)));
        REQUIRE(actual == expected);
    }
}
#endif

} // namespace
