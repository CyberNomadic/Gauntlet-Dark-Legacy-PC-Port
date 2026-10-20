#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/AnimationCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("animation wire clocks preserve native loop and hold boundaries exactly",
          "[netplay][animation-checkpoint]") {
    TreeSequenceInfo sequence;
    sequence.frames = 9;
    sequence.frameRate = 30;
    for (const bool repeat : {false, true}) {
        for (const s32 rate : {30, 60, 120}) {
            AnimationPlayer source;
            source.start(sequence, 0, 0.13f);
            const f32 seconds = 1.0f / static_cast<f32>(rate);
            for (s32 tick = 0; tick < 100; ++tick) {
                source.advance(seconds, repeat);
                const auto saved = source.checkpoint();
                const auto bytes = AnimationCheckpoint::encode(saved);
                REQUIRE(bytes);
                CHECK(bytes->size() == AnimationCheckpoint::kBytes);
                const auto decoded = AnimationCheckpoint::decode(*bytes);
                REQUIRE(decoded);
                CHECK(*decoded == saved);
                AnimationPlayer resumed;
                REQUIRE(resumed.restore(*decoded, &sequence));
                auto reference = source;
                for (s32 future = 0; future < 12; ++future) {
                    CHECK(resumed.advance(seconds, repeat) == reference.advance(seconds, repeat));
                    CHECK(resumed.checkpoint() == reference.checkpoint());
                }
            }
        }
    }
}

TEST_CASE("animation wire clocks reject malformed sizes flags and floating point state",
          "[netplay][animation-checkpoint]") {
    const AnimationPlayer player;
    const auto encoded = AnimationCheckpoint::encode(player.checkpoint());
    REQUIRE(encoded);
    REQUIRE(AnimationCheckpoint::decode(*encoded));
    auto bytes = *encoded;
    bytes.back() = 0x80;
    CHECK_FALSE(AnimationCheckpoint::decode(bytes));
    bytes = *encoded;
    bytes.push_back(0);
    CHECK_FALSE(AnimationCheckpoint::decode(bytes));
    for (usize length = 0; length < encoded->size(); ++length) {
        CHECK_FALSE(AnimationCheckpoint::decode(std::span(*encoded).first(length)));
    }
    auto state = player.checkpoint();
    state.time = std::numeric_limits<f64>::quiet_NaN();
    CHECK_FALSE(AnimationCheckpoint::encode(state));
    state = player.checkpoint();
    state.speed = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(AnimationCheckpoint::encode(state));
}
} // namespace
