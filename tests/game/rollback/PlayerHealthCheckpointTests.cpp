#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/ByteReader.h"

#include "game/rollback/PlayerHealthCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("health checkpoint transports the exact shared native cue random stream",
          "[rollback][player-health-checkpoint]") {
    Mt19937 random{0x1234ABCD};
    for (usize index = 0; index < 641; ++index) {
        random();
    }
    const auto state = random.checkpoint();
    const auto encoded = PlayerHealthCheckpoint::encode(state);
    REQUIRE(encoded);
    CHECK(encoded->size() == PlayerHealthCheckpoint::kBytes);
    CHECK(readU32LE(*encoded, 0) == state.words[0]);
    const auto decoded = PlayerHealthCheckpoint::decode(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(PlayerHealthCheckpoint::encode(*decoded) == encoded);
    Mt19937 replay;
    REQUIRE(replay.restore(*decoded));
    for (usize index = 0; index < 2000; ++index) {
        CHECK(replay() == random());
    }
    for (usize size = 0; size < encoded->size(); ++size) {
        CHECK_FALSE(PlayerHealthCheckpoint::decode(std::span{*encoded}.first(size)));
    }
    auto malformed = *encoded;
    malformed.back() = 0xFF;
    CHECK_FALSE(PlayerHealthCheckpoint::decode(malformed));
    malformed = *encoded;
    malformed.push_back(0);
    CHECK_FALSE(PlayerHealthCheckpoint::decode(malformed));
    auto bad = state;
    bad.cursor = static_cast<u32>(Mt19937::state_size + 1);
    CHECK_FALSE(PlayerHealthCheckpoint::encode(bad));
}
} // namespace
