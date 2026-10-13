#include <array>
#include <bit>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/TurboEffectCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void overwrite(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize i = 0; i < sizeof(value); ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}

TEST_CASE("turbo effect clocks distinguish absent banks from existing frame zero",
          "[turbo-effect-checkpoint]") {
    for (u32 flags = 0; flags < 4; ++flags) {
        CAPTURE(flags);
        TurboEffectState state;
        state.meleeDamage = 127.125f;
        if ((flags & 1U) != 0) {
            state.classBank = TextureAnimator::Checkpoint{1267, 0.0075f, 0.5f};
        }
        if ((flags & 2U) != 0) {
            state.weaponBank.emplace();
        }
        REQUIRE(state.valid());
        const auto bytes = TurboEffectCheckpoint::encode(state);
        REQUIRE(bytes);
        CHECK(bytes->size() ==
              TurboEffectCheckpoint::kHeaderBytes +
                  static_cast<usize>(std::popcount(flags)) * TurboEffectCheckpoint::kBankBytes);
        CHECK(TurboEffectCheckpoint::decode(*bytes) == state);
        for (usize length = 0; length < bytes->size(); ++length) {
            CHECK_FALSE(TurboEffectCheckpoint::decode(std::span{*bytes}.first(length)));
        }
        auto trailing = *bytes;
        trailing.push_back(0);
        CHECK_FALSE(TurboEffectCheckpoint::decode(trailing));
    }
}

TEST_CASE("turbo effect bank packets reject invalid clocks damage and flag combinations",
          "[turbo-effect-checkpoint]") {
    TurboEffectState state{12.5f, TextureAnimator::Checkpoint{57, 0.003f, 0.5f},
                           TextureAnimator::Checkpoint{31, 0.01f, 0}};
    const auto bytes = TurboEffectCheckpoint::encode(state);
    REQUIRE(bytes);
    REQUIRE(bytes->size() == TurboEffectCheckpoint::kMaxBytes);
    for (const auto value : {4U, 0x80000003U, 0U, 1U, 2U}) {
        auto broken = *bytes;
        overwrite(broken, 0, value);
        CHECK_FALSE(TurboEffectCheckpoint::decode(broken));
    }
    for (const auto value :
         {-1.0f, TurboEffectState::kMaxDamage + 1, std::numeric_limits<f32>::infinity(),
          std::numeric_limits<f32>::quiet_NaN()}) {
        auto broken = *bytes;
        overwrite(broken, 4, std::bit_cast<u32>(value));
        CHECK_FALSE(TurboEffectCheckpoint::decode(broken));
        auto invalid = state;
        invalid.meleeDamage = value;
        CHECK_FALSE(TurboEffectCheckpoint::encode(invalid));
    }
    for (const usize offset : {12U, 16U, 24U, 28U}) {
        auto broken = *bytes;
        overwrite(broken, offset, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()));
        CHECK_FALSE(TurboEffectCheckpoint::decode(broken));
    }
}
} // namespace
