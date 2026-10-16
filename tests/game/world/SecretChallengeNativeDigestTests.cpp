#include <algorithm>
#include <array>
#include <numeric>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"

#include "game/world/SecretChallenge.h"

namespace {
using namespace gdl;
using namespace gdl::game;

auto digestOf(const SecretChallenge& challenge) {
    NativeStateDigest digest;
    digest.value(challenge);
    REQUIRE(digest.good());
    return digest.finish();
}

TEST_CASE("native challenge digest canonicalizes unordered coin identities, not just their count",
          "[native-state-digest][secret-native-digest]") {
    std::vector<usize> coins(257);
    std::iota(coins.begin(), coins.end(), 0);
    SecretChallenge a, b;
    REQUIRE(a.begin(3, 45, coins));
    std::ranges::reverse(coins);
    REQUIRE(b.begin(3, 45, coins));
    CHECK(digestOf(a) == digestOf(b));
    CHECK_FALSE(a.collect(3));
    CHECK_FALSE(b.collect(4));
    CHECK(a.coinsLeft() == b.coinsLeft());
    CHECK(a.remaining() == b.remaining());
    CHECK(digestOf(a) != digestOf(b));
    CHECK_FALSE(a.collect(4));
    CHECK_FALSE(b.collect(3));
    CHECK(digestOf(a) == digestOf(b));
}

TEST_CASE("native challenge digest includes reward total duration and subsecond clock",
          "[native-state-digest][secret-native-digest]") {
    SecretChallenge base, changed;
    REQUIRE(base.begin(0, 30, std::array<usize, 1>{3}));
    const auto expected = digestOf(base);
    changed = base;
    changed.step(0.001f);
    CHECK(digestOf(changed) != expected);
    changed = base;
    changed.step(10, true);
    CHECK(digestOf(changed) == expected);

    REQUIRE(changed.begin(1, 30, std::array<usize, 1>{3}));
    CHECK(changed.coinsLeft() == base.coinsLeft());
    CHECK(digestOf(changed) != expected);
    REQUIRE(changed.begin(0, 30, std::array<usize, 2>{1, 3}));
    CHECK_FALSE(changed.collect(1));
    CHECK(changed.coinsLeft() == base.coinsLeft());
    CHECK(changed.remaining() == base.remaining());
    CHECK(changed.state() == base.state());
    CHECK(digestOf(changed) != expected); // The original goal still differs.

    REQUIRE(changed.begin(0, 60, std::array<usize, 1>{3}));
    REQUIRE(base.collect(3));
    REQUIRE(changed.collect(3));
    CHECK(changed.remaining() == base.remaining());
    CHECK(changed.state() == base.state());
    CHECK(changed.totalCoins() == base.totalCoins());
    CHECK(digestOf(changed) != digestOf(base)); // Only original duration differs.
}

TEST_CASE("native challenge value restore reproduces corrected one through eight tick branches",
          "[native-state-digest][secret-native-digest][local-rewind]") {
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        SecretChallenge reference, predicted;
        const std::array<usize, 4> coins{5, 10, 15, 20};
        REQUIRE(reference.begin(0, 10, coins));
        REQUIRE(predicted.begin(0, 10, coins));
        const auto before = predicted;
        const auto advance = [](SecretChallenge& challenge, s32 tick) {
            if (tick < 4) {
                challenge.collect(static_cast<usize>((tick + 1) * 5));
            }
            return challenge.step(0.3f, tick == 4);
        };
        std::vector<decltype(digestOf(reference))> expected;
        std::vector<std::vector<s32>> cues;
        for (s32 tick = 0; tick < distance; ++tick) {
            cues.push_back(advance(reference, tick));
            expected.push_back(digestOf(reference));
            predicted.step(3);
        }
        CHECK(digestOf(predicted) != digestOf(reference));
        predicted = before;
        CHECK(digestOf(predicted) == digestOf(before));
        for (s32 tick = 0; tick < distance; ++tick) {
            CHECK(advance(predicted, tick) == cues[static_cast<usize>(tick)]);
            CHECK(digestOf(predicted) == expected[static_cast<usize>(tick)]);
        }
        CHECK(predicted == reference);
    }
}
} // namespace
