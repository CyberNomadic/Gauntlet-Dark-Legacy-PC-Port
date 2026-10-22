#include <array>
#include <bit>
#include <limits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "game/characters/Inventory.h"
#include "game/combat/DamageTypes.h"
#include "game/rollback/PlayerPotionCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

PlayerPotionState potionState(usize count = 9) {
    PlayerPotionState result;
    for (usize i = 0; i < count; ++i) {
        result.potions.push_back(static_cast<s32>(i % 4 + 1));
    }
    result.powers.fill(20.5f);
    result.bonuses[3] = damage::kOwnColourBonus;
    result.powers[3] *= result.bonuses[3];
    result.damageFlags = damage::kHeal;
    result.ticks = PlayerPotionState::kMaxTicks;
    return result;
}

void writeWord(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize byte = 0; byte < 4; ++byte) {
        bytes[offset + byte] = static_cast<u8>(value >> (byte * 8));
    }
}

TEST_CASE("potion checkpoints preserve ordered native stack and exact colour tuning",
          "[netplay][potion-checkpoint]") {
    for (usize count = 0; count <= static_cast<usize>(Inventory::kMostPotions); ++count) {
        auto state = potionState(count);
        for (const u32 ticks : {0U, 12U, PlayerPotionState::kMaxTicks}) {
            state.textureFrame = ticks == 0 ? std::nullopt : std::optional{27.5f};
            state.ticks = ticks;
            const auto bytes = PlayerPotionCheckpoint::encode(state);
            REQUIRE(bytes);
            CHECK(bytes->size() == PlayerPotionCheckpoint::kHeaderBytes + count * 4);
            const auto restored = PlayerPotionCheckpoint::decode(*bytes);
            REQUIRE(restored);
            CHECK(*restored == state);
            CHECK(PlayerPotionCheckpoint::encode(*restored) == bytes);
        }
    }
    const auto maximum = PlayerPotionCheckpoint::encode(potionState());
    REQUIRE(maximum);
    CHECK(maximum->size() == PlayerPotionCheckpoint::kMaxBytes);
}

TEST_CASE("potion checkpoint decoding rejects malformed counts flags tuning and every truncation",
          "[netplay][potion-checkpoint]") {
    const auto state = potionState();
    const auto encoded = PlayerPotionCheckpoint::encode(state);
    REQUIRE(encoded);
    const auto& bytes = *encoded;
    for (usize size = 0; size < bytes.size(); ++size) {
        CHECK_FALSE(PlayerPotionCheckpoint::decode(std::span{bytes}.first(size)));
    }
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK_FALSE(PlayerPotionCheckpoint::decode(trailing));
    for (const auto [offset, value] :
         std::array<std::pair<usize, u32>, 16>{{{0, PlayerPotionState::kMaxTicks + 1},
                                                {4, 10},
                                                {4, 0xFFFFFFFF},
                                                {8, 1},
                                                {8, damage::kHeal | 1U},
                                                {12, std::bit_cast<u32>(7.9f)},
                                                {12, std::bit_cast<u32>(32.1f)},
                                                {16, std::bit_cast<u32>(21.0f)},
                                                {32, std::bit_cast<u32>(damage::kOwnColourBonus)},
                                                {52, 2},
                                                {56, 1},
                                                {56, 0x80000000},
                                                {56, 0x7FC00000},
                                                {60, 0},
                                                {60, 5},
                                                {60, 0xFFFFFFFF}}}) {
        auto malformed = bytes;
        writeWord(malformed, offset, value);
        CHECK_FALSE(PlayerPotionCheckpoint::decode(malformed));
    }
    for (usize offset = 12; offset < 52; offset += 4) {
        for (const f32 value :
             {std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity(),
              -std::numeric_limits<f32>::infinity()}) {
            auto malformed = bytes;
            writeWord(malformed, offset, std::bit_cast<u32>(value));
            CHECK_FALSE(PlayerPotionCheckpoint::decode(malformed));
        }
    }
    auto malformed = state;
    malformed.potions.push_back(1);
    CHECK_FALSE(PlayerPotionCheckpoint::encode(malformed));
    malformed = state;
    malformed.bonuses[1] = damage::kOwnColourBonus;
    malformed.powers[1] *= malformed.bonuses[1];
    CHECK_FALSE(PlayerPotionCheckpoint::encode(malformed));
    malformed = state;
    malformed.textureFrame = 0.0f;
    const auto zeroClock = PlayerPotionCheckpoint::encode(malformed);
    REQUIRE(zeroClock);
    const auto restoredClock = PlayerPotionCheckpoint::decode(*zeroClock);
    REQUIRE(restoredClock);
    CHECK(restoredClock->textureFrame == std::optional{0.0f});
    for (const f32 badClock : {-1.0f, 1'000'000'128.0f, std::numeric_limits<f32>::quiet_NaN(),
                               std::numeric_limits<f32>::infinity()}) {
        malformed.textureFrame = badClock;
        CHECK_FALSE(PlayerPotionCheckpoint::encode(malformed));
        auto badBytes = *zeroClock;
        writeWord(badBytes, 56, std::bit_cast<u32>(badClock));
        CHECK_FALSE(PlayerPotionCheckpoint::decode(badBytes));
    }
}

} // namespace
