#include <bit>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/netplay/OpponentFrame.h"

namespace {
using namespace gdl;
using namespace gdl::game;

OpponentFrame example() {
    OpponentFrame value;
    value.ticks = 2;
    value.seconds = 1.0f / 30;
    value.missileSpeed = 1.75f;
    value.difficultyGain = 0.667f;
    value.initialPlayers = 4;
    value.pendingPlacements = 23;
    value.held = true;
    value.timeStopped = true;
    value.unsupportedCombatants = true;
    for (const u8 seat : {u8{3}, u8{1}, u8{0}, u8{2}}) {
        EnemyView view;
        view.player = seat;
        view.position = {10.5f + seat, -6.25f, 9.125f};
        view.radius = 1.125f;
        view.height = 6.5f;
        view.level = 83;
        view.hidden = true;
        view.captured = true;
        view.invisible = true;
        view.meleeWard = EnemyMeleeWard::HealthVamp;
        view.antiDeath = true;
        view.reflects = true;
        view.it = true;
        view.damageable = false;
        view.recentlyHit = true;
        view.collisionHeight = 4.125f;
        view.blockableAttack = true;
        view.decoy = Vec3{-29.5f, 8.5f, 31.25f};
        view.collisionRadius = 2.5f;
        view.heldDeath = 17;
        value.players.push_back({seat, view});
    }
    Obstacle box;
    box.centre = {-2.5f, 6.5f, 3.5f};
    box.yaw = 0.375f;
    box.halfAcross = 1.5f;
    box.halfAlong = 4.25f;
    box.height = 9.25f;
    box.solid = false;
    box.cylinderRadius = 2.75f;
    box.enemyItem = Obstacle::ItemQuery{true, 3.125f};
    value.fixtures.push_back(box);
    value.stops.push_back({box, 72, 125, 19});
    REQUIRE(value.valid());
    return value;
}

void putWord(std::vector<u8>& bytes, usize offset, u32 value) {
    REQUIRE(offset + 4 <= bytes.size());
    for (u32 byte = 0; byte < 4; ++byte) {
        bytes[offset + byte] = static_cast<u8>(value >> (byte * 8));
    }
}

TEST_CASE("opponent frame preserves every player field and exact obstacle observations",
          "[netplay][opponent-frame]") {
    const auto source = example();
    const auto encoded = OpponentFrameCodec::encode(source);
    REQUIRE(encoded);
    CHECK(encoded->size() == OpponentFrameCodec::kHeaderBytes +
                                 source.players.size() * OpponentFrameCodec::kPlayerBytes +
                                 OpponentFrameCodec::kObstacleBytes +
                                 OpponentFrameCodec::kStopBytes);
    const auto decoded = OpponentFrameCodec::decode(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == source);
    CHECK(decoded->players.front().seat == 3);
    CHECK(decoded->players.front().view.player == 3);

    OpponentFrame defaults;
    defaults.players.push_back({0, EnemyView{.player = 0}});
    defaults.fixtures.emplace_back();
    defaults.stops.emplace_back();
    const auto plain = OpponentFrameCodec::encode(defaults);
    REQUIRE(plain);
    const auto roundtrip = OpponentFrameCodec::decode(*plain);
    REQUIRE(roundtrip);
    CHECK(*roundtrip == defaults);
    CHECK_FALSE(roundtrip->players.front().view.decoy);
    CHECK_FALSE(roundtrip->players.front().view.collisionRadius);
    CHECK_FALSE(roundtrip->players.front().view.collisionHeight);
    CHECK_FALSE(roundtrip->fixtures.front().enemyItem);
}

TEST_CASE("opponent frame rejects malformed headers counts enums and scalar state",
          "[netplay][opponent-frame]") {
    const auto encoded = OpponentFrameCodec::encode(example());
    REQUIRE(encoded);
    const auto mutation = GENERATE(Catch::Generators::values<std::pair<usize, u32>>(
        {{0, 0},
         {4, 2},
         {8, 0},
         {8, 3},
         {12, 0x7fc00000},
         {12, 0x7f800000},
         {16, 0x7fc00000},
         {20, std::bit_cast<u32>(1.001f)},
         {24, 0},
         {24, 5},
         {28, 8},
         {32, OpponentFrame::kMaxPendingPlacements + 1},
         {36, 5},
         {40, 513},
         {44, 513},
         {48, 256},
         {52, 9},
         {56, 0x7fc00000},
         {68, std::bit_cast<u32>(-1.0f)},
         {72, 0},
         {76, 100},
         {80, 4096},
         {84, 3},
         {88, 0x7fc00000},
         {92, 0x7fc00000},
         {104, 0},
         {108, Enemies::kMost}}));
    CAPTURE(mutation.first, mutation.second);
    auto bytes = *encoded;
    putWord(bytes, mutation.first, mutation.second);
    CHECK_FALSE(OpponentFrameCodec::decode(bytes));
}

TEST_CASE("opponent frame rejects truncation trailing data and hidden optional payloads",
          "[netplay][opponent-frame]") {
    const auto encoded = OpponentFrameCodec::encode(example());
    REQUIRE(encoded);
    for (usize size = 0; size < encoded->size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(OpponentFrameCodec::decode(std::span(*encoded).first(size)));
    }
    auto trailing = *encoded;
    trailing.push_back(0);
    CHECK_FALSE(OpponentFrameCodec::decode(trailing));
    auto hidden = *encoded;
    putWord(hidden, OpponentFrameCodec::kHeaderBytes + 32, 0);
    CHECK_FALSE(OpponentFrameCodec::decode(hidden));
    auto duplicate = *encoded;
    putWord(duplicate, OpponentFrameCodec::kHeaderBytes + OpponentFrameCodec::kPlayerBytes, 3);
    putWord(duplicate, OpponentFrameCodec::kHeaderBytes + OpponentFrameCodec::kPlayerBytes + 4, 3);
    CHECK_FALSE(OpponentFrameCodec::decode(duplicate));
    auto wrongIdentity = *encoded;
    putWord(wrongIdentity, OpponentFrameCodec::kHeaderBytes + 4, 2);
    CHECK_FALSE(OpponentFrameCodec::decode(wrongIdentity));
}

TEST_CASE("opponent frame bounds complete geometry rosters without truncation",
          "[netplay][opponent-frame]") {
    auto frame = example();
    frame.fixtures.resize(OpponentFrame::kMaxFixtures, frame.fixtures.front());
    frame.stops.resize(OpponentFrame::kMaxStops, frame.stops.front());
    const auto full = OpponentFrameCodec::encode(frame);
    REQUIRE(full);
    CHECK(full->size() == OpponentFrameCodec::kMaxBytes);
    const auto roundtrip = OpponentFrameCodec::decode(*full);
    REQUIRE(roundtrip);
    CHECK(*roundtrip == frame);
    SECTION("fixture overflow") {
        frame.fixtures.push_back(frame.fixtures.front());
    }
    SECTION("missile stop overflow") {
        frame.stops.push_back(frame.stops.front());
    }
    CHECK_FALSE(frame.valid());
    CHECK_FALSE(OpponentFrameCodec::encode(frame));
}

TEST_CASE("opponent frame obstacle option flags and rocks are validated independently",
          "[netplay][opponent-frame]") {
    auto frame = example();
    frame.players.clear();
    const auto encoded = OpponentFrameCodec::encode(frame);
    REQUIRE(encoded);
    const auto mutation =
        GENERATE(Catch::Generators::values<std::pair<usize, u32>>({{48 + 36, 8},
                                                                   {48 + 36, 4},
                                                                   {48 + 20, 0x7fc00000},
                                                                   {48 + 32, 0x7f800000},
                                                                   {88 + 40, 65536},
                                                                   {88 + 44, 0xffffffff},
                                                                   {88 + 48, 0xffffffff}}));
    CAPTURE(mutation.first, mutation.second);
    auto bytes = *encoded;
    putWord(bytes, mutation.first, mutation.second);
    CHECK_FALSE(OpponentFrameCodec::decode(bytes));
}
} // namespace
