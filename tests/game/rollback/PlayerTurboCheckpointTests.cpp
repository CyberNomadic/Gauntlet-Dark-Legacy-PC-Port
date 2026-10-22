#include <bit>
#include <limits>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/rollback/PlayerTurboCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

PlayerTurboState populated() {
    PlayerTurboState state;
    state.meter = {81.25f, 52.5f, TurboMeter::Flash::Gleam, 17};
    state.move.pending = {7, 7};
    state.move.all = {2, 7, 7};
    state.move.owed = 40;
    state.move.named = true;
    state.move.weaponHidden = true;
    state.move.volleysShot = {3, 0, 0};
    return state;
}

std::vector<u8> encode(const PlayerTurboState& state) {
    const auto bytes = PlayerTurboCheckpoint::encode(state);
    REQUIRE(bytes);
    return *bytes;
}

void word(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<u8>(value >> (index * 8));
    }
}

TEST_CASE("turbo wire preserves exact meter and timeline fields without object layout",
          "[netplay][rollback][turbo-checkpoint]") {
    const auto state = populated();
    const auto bytes = encode(state);
    CHECK(bytes.size() == PlayerTurboCheckpoint::kHeaderBytes + usize{4} * (2 + 3 + 3));
    CHECK(bytes[0] == 0);
    CHECK(bytes[2] == 0xA2); // little-endian IEEE754 held=81.25
    CHECK(bytes[3] == 0x42);
    CHECK(bytes[8] == 1);
    CHECK(bytes[12] == 17);
    CHECK(bytes[21] == 0);
    CHECK(bytes[20] == 3);
    CHECK(bytes[24] == 2);
    CHECK(bytes[28] == 3);
    CHECK(bytes[32] == 3);
    CHECK(bytes[36] == 7);
    const auto restored = PlayerTurboCheckpoint::decode(bytes);
    REQUIRE(restored);
    CHECK(*restored == state);
    CHECK(encode(*restored) == bytes);
    CHECK(encode({}) == std::vector<u8>(PlayerTurboCheckpoint::kHeaderBytes, 0));
    for (const bool named : {false, true}) {
        for (const bool hidden : {false, true}) {
            auto changed = state;
            changed.move.named = named;
            changed.move.weaponHidden = hidden;
            const auto decoded = PlayerTurboCheckpoint::decode(encode(changed));
            REQUIRE(decoded);
            CHECK(*decoded == changed);
        }
    }
}

TEST_CASE("turbo wire rejects truncation, extensions, oversized counts and reserved flags",
          "[netplay][rollback][turbo-checkpoint]") {
    const auto bytes = encode(populated());
    for (usize size = 0; size < bytes.size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(PlayerTurboCheckpoint::decode(std::span{bytes}.first(size)));
    }
    auto extra = bytes;
    extra.push_back(0);
    CHECK_FALSE(PlayerTurboCheckpoint::decode(extra));
    for (const usize offset : {24U, 28U, 32U}) {
        for (const u32 count : {0U, 1U, 129U, 0xFFFFFFFFU}) {
            CAPTURE(offset, count);
            auto changed = bytes;
            word(changed, offset, count);
            CHECK_FALSE(PlayerTurboCheckpoint::decode(changed));
        }
    }
    for (u32 bit = 2; bit < 32; ++bit) {
        auto changed = bytes;
        word(changed, 20, 3U | (1U << bit));
        CHECK_FALSE(PlayerTurboCheckpoint::decode(changed));
    }
    for (const u32 flash : {3U, 256U, 0xFFFFFFFFU}) {
        auto changed = bytes;
        word(changed, 8, flash);
        CHECK_FALSE(PlayerTurboCheckpoint::decode(changed));
    }
}

TEST_CASE("turbo wire rejects nonfinite charge, invalid debt and contradictory row progress",
          "[netplay][rollback][turbo-checkpoint]") {
    const auto bytes = encode(populated());
    for (const usize offset : {0U, 4U, 16U}) {
        for (const f32 value :
             {-1.0f, 101.0f, std::numeric_limits<f32>::quiet_NaN(),
              std::numeric_limits<f32>::infinity(), -std::numeric_limits<f32>::infinity()}) {
            auto changed = bytes;
            word(changed, offset, std::bit_cast<u32>(value));
            CHECK_FALSE(PlayerTurboCheckpoint::decode(changed));
        }
    }
    for (const u32 ticks : {40U, 0xFFFFFFFFU}) {
        auto changed = bytes;
        word(changed, 12, ticks);
        CHECK_FALSE(PlayerTurboCheckpoint::decode(changed));
    }
    auto wrongDebt = bytes;
    word(wrongDebt, 16, std::bit_cast<u32>(39.0f));
    CHECK_FALSE(PlayerTurboCheckpoint::decode(wrongDebt));
    for (const usize offset : {36U, 44U, 56U}) {
        auto negative = bytes;
        word(negative, offset, 0xFFFFFFFFU);
        CHECK_FALSE(PlayerTurboCheckpoint::decode(negative));
    }
    auto nonmember = bytes;
    word(nonmember, 36, 12);
    CHECK_FALSE(PlayerTurboCheckpoint::decode(nonmember));
    auto excessVolley = bytes;
    word(excessVolley, 56, TurboMove::kMaxCheckpointVolleys + 1);
    CHECK_FALSE(PlayerTurboCheckpoint::decode(excessVolley));
    auto bad = populated();
    bad.move.pending = {7}; // Duplicate rows fire together, never one at a time.
    CHECK_FALSE(PlayerTurboCheckpoint::encode(bad));
    bad = populated();
    bad.move.volleysShot.pop_back();
    CHECK_FALSE(PlayerTurboCheckpoint::encode(bad));
}

TEST_CASE("turbo wire bounds all variable arrays and keeps interruption residue",
          "[netplay][rollback][turbo-checkpoint]") {
    PlayerTurboState state;
    state.meter = {100, 100, TurboMeter::Flash::Glow, TurboMeter::kGlowTicks};
    state.move.owed = 100;
    state.move.named = true;
    state.move.weaponHidden = true;
    state.move.pending.assign(TurboMove::kMaxCheckpointRows, 0);
    state.move.all = state.move.pending;
    state.move.volleysShot.assign(TurboMove::kMaxCheckpointRows, TurboMove::kMaxCheckpointVolleys);
    const auto bytes = encode(state);
    CHECK(bytes.size() == PlayerTurboCheckpoint::kMaxBytes);
    const auto restored = PlayerTurboCheckpoint::decode(bytes);
    REQUIRE(restored);
    CHECK(*restored == state);
    for (const s32 array : {0, 1, 2}) {
        auto changed = state;
        if (array == 0) {
            changed.move.pending.push_back(0);
        } else if (array == 1) {
            changed.move.all.push_back(0);
        } else {
            changed.move.volleysShot.push_back(0);
        }
        CHECK_FALSE(PlayerTurboCheckpoint::encode(changed));
    }
    state.move.pending.clear();
    state.move.all.clear();
    state.move.owed = 0;
    state.move.weaponHidden = false;
    const auto canceled = PlayerTurboCheckpoint::decode(encode(state));
    REQUIRE(canceled);
    CHECK(*canceled == state);
    TurboMove move;
    CHECK(move.restore(canceled->move, nullptr, PlayerAnimator::Action::Ready));
    CHECK(move.checkpoint() == state.move);
}

TEST_CASE("turbo decoding does not grant trust to remote class row references",
          "[netplay][rollback][turbo-checkpoint]") {
    auto state = populated();
    state.move.volleysShot = {0, 0, 0};
    const auto decoded = PlayerTurboCheckpoint::decode(encode(state));
    REQUIRE(decoded);
    ClassStats wrong;
    wrong.moves.turboB = 0;
    wrong.moveStrikes.resize(1);
    TurboMove move;
    const auto empty = move.checkpoint();
    CHECK_FALSE(move.restore(decoded->move, &wrong, PlayerAnimator::Action::TurboStrong));
    CHECK(move.checkpoint() == empty);
    wrong.moveStrikes.resize(8);
    // Valid indices alone still do not make them this action's native chain.
    CHECK(move.restore(decoded->move, &wrong));
    CHECK_FALSE(move.restore(decoded->move, &wrong, PlayerAnimator::Action::TurboStrong));
    CHECK(move.checkpoint() == decoded->move);
}

} // namespace
