#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/TurboMove.h"
#include "game/rollback/ShotIdentity.h"

namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("native releases require an exact epoch grant seat and animation generation",
          "[netplay][shot-identity]") {
    ShotIdentity shot{1, 0, 1, 1, 0, false};
    CHECK(shot.valid());
    CHECK_FALSE(ShotIdentity{}.valid());
    auto other = shot;
    other.epoch = 0;
    CHECK_FALSE(other.valid());
    other = shot;
    other.generation = 0;
    CHECK_FALSE(other.valid());
    other = shot;
    other.grant = 0;
    CHECK_FALSE(other.valid());
    for (u8 seat = 0; seat < 5; ++seat) {
        other = shot;
        other.seat = seat;
        CHECK(other.valid() == (seat < 4));
    }
    other = shot;
    CHECK(other == shot);
    ++other.tick;
    CHECK(other != shot);
    other = shot;
    ++other.generation;
    CHECK(other != shot);
    other = shot;
    ++other.grant;
    CHECK(other != shot);
    other = shot;
    ++other.epoch;
    CHECK(other != shot);
    other = shot;
    ++other.seat;
    CHECK(other != shot);
    other = shot;
    other.strong = true;
    CHECK(other != shot);
    for (u8 ordinal = 0; ordinal <= ShotIdentity::kMaxShots; ++ordinal) {
        other = shot;
        other.ordinal = ordinal;
        CHECK(other.valid() == (ordinal < ShotIdentity::kMaxShots));
        CHECK((other == shot) == (ordinal == 0));
    }
    other.ordinal = 255;
    CHECK_FALSE(other.valid());
}

TEST_CASE("turbo releases distinguish chain slots and volley counters from fan ordinals",
          "[netplay][shot-identity][turbo-volley-identity]") {
    const ShotIdentity ordinary{1, 10, 2, 1, 0, false};
    for (const usize slot : {usize{0}, usize{1}, TurboMove::kMaxCheckpointRows - 1}) {
        for (const s32 index : {0, 1, TurboMove::kMaxCheckpointVolleys - 1}) {
            auto shot = ordinary;
            shot.emission = TurboMove::Volley{{}, slot, index}.emissionIdentity();
            REQUIRE(shot.emission != 0);
            CHECK(shot.valid());
            CHECK(shot != ordinary);
            auto changed = shot;
            changed.strong = true;
            CHECK_FALSE(changed.valid());
            changed = shot;
            changed.ordinal = 1;
            CHECK_FALSE(changed.valid());
        }
    }
    for (const u32 emission : {1U, 65535U, 0x00810000U, 0xFFFF0000U, 0xFFFFFFFFU}) {
        auto shot = ordinary;
        shot.emission = emission;
        CHECK_FALSE(shot.valid());
    }
}

TEST_CASE("release identity ordering includes every field in declaration order",
          "[netplay][shot-identity]") {
    const std::array ordered{ShotIdentity{1, 1, 1, 1, 0, false},
                             ShotIdentity{1, 1, 1, 1, 0, false, 0, 0x00010000U},
                             ShotIdentity{1, 1, 1, 1, 0, false, 0, 0x00010001U},
                             ShotIdentity{1, 1, 1, 1, 0, false, 0, 0x00020000U},
                             ShotIdentity{1, 1, 1, 1, 0, false, 1},
                             ShotIdentity{1, 1, 1, 1, 0, false, 4},
                             ShotIdentity{1, 1, 1, 1, 0, true},
                             ShotIdentity{1, 1, 1, 1, 1, false},
                             ShotIdentity{1, 1, 1, 2, 0, false},
                             ShotIdentity{1, 1, 2, 1, 0, false},
                             ShotIdentity{1, 2, 1, 1, 0, false},
                             ShotIdentity{2, 1, 1, 1, 0, false}};
    for (usize left = 0; left < ordered.size(); ++left) {
        for (usize right = 0; right < ordered.size(); ++right) {
            CHECK((ordered[left] < ordered[right]) == (left < right));
            CHECK((ordered[left] > ordered[right]) == (left > right));
            CHECK((ordered[left] == ordered[right]) == (left == right));
        }
    }
}
} // namespace
