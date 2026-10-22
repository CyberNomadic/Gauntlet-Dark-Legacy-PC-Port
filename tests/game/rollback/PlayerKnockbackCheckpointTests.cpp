#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/rollback/PlayerKnockbackCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using State = Knockback::Checkpoint;

static_assert(std::is_trivially_copyable_v<State>);
constexpr f32 kFrame = 1.0f / Knockback::kFrameRate;

State populated() {
    State state;
    state.force = {1.25f, -2.5f, 3.75f};
    state.flags = 0x01020304;
    state.damage = 7.5f;
    state.velocity = {32, -0.5f, 9};
    state.fastSeconds = kFrame / 2;
    state.frameSeconds = kFrame / 4;
    state.shove = {-0.5f, 0, 1.25f};
    state.shoved = true;
    state.pushed = true;
    return state;
}

std::vector<u8> encode(const State& state) {
    const auto bytes = PlayerKnockbackCheckpoint::encode(state);
    REQUIRE(bytes);
    REQUIRE(bytes->size() == PlayerKnockbackCheckpoint::kBytes);
    return *bytes;
}

void word(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}

TEST_CASE("knockback wire preserves each impulse, clock and independent shove latch",
          "[netplay][rollback][knockback-checkpoint]") {
    const auto state = populated();
    const auto bytes = encode(state);
    CHECK(bytes[0] == 0);
    CHECK(bytes[2] == 0xA0); // force.x is little-endian IEEE754 1.25
    CHECK(bytes[3] == 0x3F);
    CHECK(bytes[12] == 4);
    CHECK(bytes[15] == 1);
    CHECK(bytes[52] == 3);
    const auto decoded = PlayerKnockbackCheckpoint::decode(bytes);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(encode(*decoded) == bytes);
    for (const bool shoved : {false, true}) {
        for (const bool pushed : {false, true}) {
            auto changed = state;
            changed.shoved = shoved;
            changed.pushed = pushed;
            const auto packed = encode(changed);
            CHECK(packed[52] == ((shoved ? 1U : 0U) | (pushed ? 2U : 0U)));
            const auto restored = PlayerKnockbackCheckpoint::decode(packed);
            REQUIRE(restored);
            CHECK(*restored == changed);
        }
    }
    auto signedZero = state;
    signedZero.force.x = -0.0f;
    const auto packedZero = encode(signedZero);
    const auto restoredZero = PlayerKnockbackCheckpoint::decode(packedZero);
    REQUIRE(restoredZero);
    CHECK(std::signbit(restoredZero->force.x));
    CHECK(encode(*restoredZero) == packedZero);
    const State empty;
    CHECK(encode(empty) == std::vector<u8>(PlayerKnockbackCheckpoint::kBytes, 0));
    const auto restoredEmpty = PlayerKnockbackCheckpoint::decode(encode(empty));
    REQUIRE(restoredEmpty);
    CHECK(*restoredEmpty == empty);
}

TEST_CASE("knockback wire rejects every truncated packet and unknown latch bit",
          "[netplay][rollback][knockback-checkpoint]") {
    const auto bytes = encode(populated());
    for (usize size = 0; size < bytes.size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(PlayerKnockbackCheckpoint::decode(std::span{bytes}.first(size)));
    }
    auto extra = bytes;
    extra.push_back(0);
    CHECK_FALSE(PlayerKnockbackCheckpoint::decode(extra));
    for (u32 bit = 2; bit < 32; ++bit) {
        auto changed = bytes;
        word(changed, 52, 3U | (1U << bit));
        CHECK_FALSE(PlayerKnockbackCheckpoint::decode(changed));
    }
    // Damage flags are not this codec's latch flags. Native queue retains all bits.
    for (u32 bit = 0; bit < 32; ++bit) {
        auto state = populated();
        state.flags = 1U << bit;
        const auto restored = PlayerKnockbackCheckpoint::decode(encode(state));
        REQUIRE(restored);
        CHECK(*restored == state);
    }
}

TEST_CASE("knockback wire rejects nonfinite values and invalid native decay intervals",
          "[netplay][rollback][knockback-checkpoint]") {
    const auto bytes = encode(populated());
    constexpr std::array<usize, 12> kReals{0, 4, 8, 16, 20, 24, 28, 32, 36, 40, 44, 48};
    for (const usize offset : kReals) {
        for (const f32 value :
             {std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity(),
              -std::numeric_limits<f32>::infinity(), std::numeric_limits<f32>::max(),
              -std::numeric_limits<f32>::max()}) {
            CAPTURE(offset, value);
            auto changed = bytes;
            word(changed, offset, std::bit_cast<u32>(value));
            CHECK_FALSE(PlayerKnockbackCheckpoint::decode(changed));
        }
    }
    for (const usize offset : {16U, 32U, 36U}) {
        auto changed = bytes;
        word(changed, offset, std::bit_cast<u32>(-0.001f));
        CHECK_FALSE(PlayerKnockbackCheckpoint::decode(changed));
    }
    auto invalidFrame = bytes;
    word(invalidFrame, 36, std::bit_cast<u32>(kFrame));
    CHECK_FALSE(PlayerKnockbackCheckpoint::decode(invalidFrame));
    auto invalidFast = bytes;
    word(invalidFast, 32, std::bit_cast<u32>(std::nextafter(kFrame, 1.0f)));
    CHECK_FALSE(PlayerKnockbackCheckpoint::decode(invalidFast));
    auto verticalShove = bytes;
    word(verticalShove, 44, std::bit_cast<u32>(std::numeric_limits<f32>::denorm_min()));
    CHECK_FALSE(PlayerKnockbackCheckpoint::decode(verticalShove));

    auto invalid = populated();
    invalid.frameSeconds = kFrame;
    CHECK_FALSE(PlayerKnockbackCheckpoint::encode(invalid));
    invalid = populated();
    invalid.fastSeconds = -1;
    CHECK_FALSE(PlayerKnockbackCheckpoint::encode(invalid));
    invalid = populated();
    invalid.damage = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(PlayerKnockbackCheckpoint::encode(invalid));
}

TEST_CASE("knockback wire retains exact transport bounds without clamping live motion",
          "[netplay][rollback][knockback-checkpoint]") {
    auto state = populated();
    state.force = Vec3{Knockback::kCheckpointMagnitude};
    state.velocity = Vec3{-Knockback::kCheckpointMagnitude};
    state.damage = Knockback::kCheckpointMagnitude;
    state.shove = {Knockback::kCheckpointMagnitude, 0, -Knockback::kCheckpointMagnitude};
    state.fastSeconds = kFrame;
    state.frameSeconds = std::nextafter(kFrame, 0.0f);
    state.flags = 0xFFFFFFFFU;
    const auto bytes = encode(state);
    const auto restored = PlayerKnockbackCheckpoint::decode(bytes);
    REQUIRE(restored);
    CHECK(*restored == state);
    const f32 outside =
        std::nextafter(Knockback::kCheckpointMagnitude, std::numeric_limits<f32>::infinity());
    for (const usize offset : {0U, 4U, 8U, 16U, 20U, 24U, 28U, 40U, 48U}) {
        auto changed = bytes;
        word(changed, offset, std::bit_cast<u32>(outside));
        CHECK_FALSE(PlayerKnockbackCheckpoint::decode(changed));
    }
    Knockback live;
    live.addVelocity({outside, 0, 0});
    CHECK(live.velocity().x == outside);
    CHECK_FALSE(PlayerKnockbackCheckpoint::encode(live.checkpoint()));
}

TEST_CASE("wire restored knockback replays pending kick and shove through partial native frames",
          "[netplay][rollback][knockback-checkpoint][cadence]") {
    for (const s32 frequency : {30, 60, 120, 144}) {
        const f32 seconds = 1.0f / static_cast<f32>(frequency);
        for (const f32 phase : {0.0f, kFrame / 8, kFrame / 2, std::nextafter(kFrame, 0.0f)}) {
            CAPTURE(frequency, phase);
            auto state = populated();
            state.frameSeconds = phase;
            state.flags = Knockback::kKnockDown | Knockback::kKnockBack;
            Knockback original;
            REQUIRE(original.restore(state));
            for (s32 tick = 0; tick < 80; ++tick) {
                const auto restored =
                    PlayerKnockbackCheckpoint::decode(encode(original.checkpoint()));
                REQUIRE(restored);
                Knockback replayed;
                REQUIRE(replayed.restore(*restored));
                // The boundary is before native motion consumes the queued impulse.
                CHECK(replayed.step(seconds, 10) == original.step(seconds, 10));
                CHECK(replayed.kick(-0.5f, tick % 2 == 0) == original.kick(-0.5f, tick % 2 == 0));
                for (Knockback* knock : {&original, &replayed}) {
                    if (tick % 3 == 0) {
                        knock->shove({0.2f, 0, -0.4f}, seconds);
                    }
                    knock->endFrame();
                    if (tick % 9 == 0) {
                        knock->queue({-1, 4, 2}, Knockback::kBlownAway, 12);
                    }
                }
                CHECK(replayed.checkpoint() == original.checkpoint());
            }
        }
    }
}
} // namespace
