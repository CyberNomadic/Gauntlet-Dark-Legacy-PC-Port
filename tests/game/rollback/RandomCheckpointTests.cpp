#include <algorithm>
#include <array>
#include <limits>
#include <random>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/rollback/RandomCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("random checkpoints encode explicit little-endian words then cursor",
          "[netplay][random-checkpoint]") {
    Mt19937::Checkpoint state;
    for (usize index = 0; index < state.words.size(); ++index) {
        state.words[index] = 0x12345678U + static_cast<u32>(index);
    }
    state.cursor = 623;
    const auto bytes = RandomCheckpoint::encode(state);
    REQUIRE(bytes);
    REQUIRE(bytes->size() == 2500);
    CHECK(
        std::ranges::equal(std::span{*bytes}.first(4), std::array<u8, 4>{0x78, 0x56, 0x34, 0x12}));
    CHECK(std::ranges::equal(std::span{*bytes}.last(4), std::array<u8, 4>{0x6f, 0x02, 0, 0}));
    const auto decoded = RandomCheckpoint::decode(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(RandomCheckpoint::encode(*decoded) == bytes);
}

TEST_CASE("random checkpoint packets preserve draw order across twist and restore",
          "[netplay][random-checkpoint]") {
    const u32 seed = GENERATE(0U, 5489U, 0xffffffffU);
    const u32 count = GENERATE(0U, 1U, 623U, 624U, 625U, 1248U);
    CAPTURE(seed, count);
    Mt19937 source(seed);
    std::mt19937 oracle(seed);
    for (u32 index = 0; index < count; ++index) {
        REQUIRE(source() == oracle());
    }
    const auto bytes = RandomCheckpoint::encode(source.checkpoint());
    REQUIRE(bytes);
    const auto state = RandomCheckpoint::decode(*bytes);
    REQUIRE(state);
    Mt19937 restored(1U);
    REQUIRE(restored.restore(*state));
    for (u32 index = 0; index < 1250; ++index) {
        const auto expected = oracle();
        REQUIRE(source() == expected);
        REQUIRE(restored() == expected);
    }
    CHECK(source.checkpoint() == restored.checkpoint());
}

TEST_CASE("random checkpoints reject truncated trailing and invalid states",
          "[netplay][random-checkpoint]") {
    const auto bytes = RandomCheckpoint::encode(Mt19937{0}.checkpoint());
    REQUIRE(bytes);
    for (usize size = 0; size < bytes->size(); ++size) {
        CHECK_FALSE(RandomCheckpoint::decode(std::span{*bytes}.first(size)));
    }
    auto trailing = *bytes;
    trailing.push_back(0);
    CHECK_FALSE(RandomCheckpoint::decode(trailing));
    for (const u32 cursor : {625U, std::numeric_limits<u32>::max()}) {
        auto malformed = Mt19937{}.checkpoint();
        malformed.cursor = cursor;
        CHECK_FALSE(RandomCheckpoint::encode(malformed));
        auto encoded = *bytes;
        for (usize byte = 0; byte < 4; ++byte) {
            encoded[RandomCheckpoint::kBytes - 4 + byte] = static_cast<u8>(cursor >> (byte * 8));
        }
        CHECK_FALSE(RandomCheckpoint::decode(encoded));
    }
    Mt19937::Checkpoint zero;
    CHECK_FALSE(RandomCheckpoint::encode(zero));
    CHECK_FALSE(RandomCheckpoint::decode(std::vector<u8>(RandomCheckpoint::kBytes, 0)));
    zero.words[0] = 0x7fffffffU;
    CHECK_FALSE(RandomCheckpoint::encode(zero));
    auto degenerate = std::vector<u8>(RandomCheckpoint::kBytes, 0);
    degenerate[0] = degenerate[1] = degenerate[2] = 0xff;
    degenerate[3] = 0x7f;
    CHECK_FALSE(RandomCheckpoint::decode(degenerate));
}
} // namespace
