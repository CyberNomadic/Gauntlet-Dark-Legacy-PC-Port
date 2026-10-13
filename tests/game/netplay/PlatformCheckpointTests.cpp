#include <bit>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/CombatSnapshot.h"

namespace {
using namespace gdl;
using namespace gdl::game;
PlatformState platformState() {
    PlatformState state;
    state.ticks = 12;
    auto& motion = state.motion.emplace();
    motion.collision = {1, 7, 0};
    motion.animations.push_back({1, 9, 2.5f, false, false, false, false, false});
    motion.triggers.targets.push_back({2, true, false, true, true, false, false, 0.5f, -2});
    motion.triggers.frameRemainder = 0.5f;
    motion.rotators.push_back({8, 0, 0.25f, true, false});
    state.floors[0] = PlayerFloorState{1, 0x1000, glm::translate(Mat4{1}, Vec3{3, -4, 2})};
    state.floors[1] = PlayerFloorState{0, 4, std::nullopt};
    state.floors[2] = PlayerFloorState{};
    return state;
}
void writeWord(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<u8>(value >> (8 * index));
    }
}
TEST_CASE("platform checkpoints preserve prior rider contacts and independent motion clocks",
          "[netplay][platform-checkpoint]") {
    for (u32 mask = 0; mask < 16; ++mask) {
        for (const bool clocks : {false, true}) {
            auto source = platformState();
            source.motion->pauseLoops = (mask & 1U) != 0;
            const auto floor = *source.floors[0];
            for (usize seat = 0; seat < 4; ++seat) {
                source.floors[seat] =
                    (mask & (1U << seat)) != 0 ? std::optional{floor} : std::nullopt;
            }
            if (!clocks) {
                source.motion.reset();
                source.ticks = 0;
            }
            const auto bytes = PlatformCheckpoint::encode(source);
            REQUIRE(bytes);
            const auto restored = PlatformCheckpoint::decode(*bytes);
            REQUIRE(restored);
            CHECK(restored->ticks == source.ticks);
            CHECK(restored->motion == source.motion);
            for (usize seat = 0; seat < 4; ++seat) {
                REQUIRE(restored->floors[seat].has_value() == source.floors[seat].has_value());
                if (source.floors[seat]) {
                    CHECK(restored->floors[seat]->object == source.floors[seat]->object);
                    CHECK(restored->floors[seat]->flags == source.floors[seat]->flags);
                    CHECK(restored->floors[seat]->placement == source.floors[seat]->placement);
                }
            }
            CHECK(PlatformCheckpoint::encode(*restored) == bytes);
        }
    }
}
TEST_CASE("platform wire rejects malformed counts flags clocks and singular carry matrices",
          "[netplay][platform-checkpoint]") {
    const auto source = platformState();
    const auto bytes = PlatformCheckpoint::encode(source);
    REQUIRE(bytes);
    for (usize length = 0; length < bytes->size(); ++length) {
        CHECK_FALSE(PlatformCheckpoint::decode(std::span{*bytes}.first(length)));
    }
    auto extra = *bytes;
    extra.push_back(0);
    CHECK_FALSE(PlatformCheckpoint::decode(extra));
    const usize animation = PlatformCheckpoint::kHeaderBytes + 3 * PlatformCheckpoint::kFloorBytes;
    for (const auto [offset, value] :
         std::array<std::pair<usize, u32>, 17>{{{0, 0},
                                                {4, 0},
                                                {8, 13},
                                                {12, 16},
                                                {16, 2},
                                                {16, 4},
                                                {20, 2049},
                                                {24, 2049},
                                                {28, 2049},
                                                {32, 65536},
                                                {36, 0x7FC00000U},
                                                {48, 2},
                                                {52, 0x7F800000U},
                                                {animation + 12, 32},
                                                {animation + 8, 0x7FC00000U},
                                                {animation + 20, 64},
                                                {animation + 44, 4}}}) {
        auto broken = *bytes;
        writeWord(broken, offset, value);
        CHECK_FALSE(PlatformCheckpoint::decode(broken));
    }
    auto bad = source;
    bad.floors[0]->placement = Mat4{0};
    CHECK_FALSE(PlatformCheckpoint::encode(bad));
    bad = source;
    bad.floors[0]->flags = 0;
    CHECK_FALSE(PlatformCheckpoint::encode(bad));
    bad = source;
    bad.motion->collision[0] = 8;
    CHECK_FALSE(PlatformCheckpoint::encode(bad));
    bad = source;
    bad.motion->animations.push_back(bad.motion->animations[0]);
    CHECK_FALSE(PlatformCheckpoint::encode(bad));
    auto absent = PlatformCheckpoint::encode(PlatformState{});
    REQUIRE(absent);
    writeWord(*absent, 36, 0x80000000U);
    CHECK_FALSE(PlatformCheckpoint::decode(*absent));
}
TEST_CASE("combat snapshots carry exact floors without requiring speculative platform actions",
          "[netplay][platform-checkpoint][combat-snapshot]") {
    const auto platform = platformState();
    CombatSnapshot source;
    source.motion.epoch = 9;
    source.motion.tick = 10;
    source.motion.cameraContinuity = 1;
    source.motion.players[0] = SeatMotion{1, 1, {0, 0, 0}, 0};
    source.players[0] = PlayerCombatState{};
    source.players[0]->floor = platform.floors[0];
    source.worldMotion = platform.motion;
    source.worldMotionTicks = 12;
    source.geometry = SceneGeometry{123, 3, 0, {}};
    auto bytes = CombatPacket::encode(source);
    REQUIRE(bytes);
    const auto restored = CombatPacket::decode(*bytes);
    REQUIRE(restored);
    CHECK(restored->worldMotion == source.worldMotion);
    CHECK(restored->worldMotionTicks == 12);
    REQUIRE(restored->players[0]->floor);
    CHECK(restored->players[0]->floor->placement == source.players[0]->floor->placement);
    CHECK_FALSE(restored->players[0]->movement.platform);
    CHECK(CombatPacket::encode(*restored) == bytes);
    auto noWorld = source;
    noWorld.worldMotion.reset();
    CHECK_FALSE(CombatPacket::encode(noWorld));
    noWorld.worldMotionTicks = 0;
    REQUIRE(CombatPacket::encode(noWorld));
    auto unsupported = source;
    unsupported.players[0]->movement.platform = true;
    CHECK_FALSE(CombatPacket::encode(unsupported));
    auto mismatch = source;
    mismatch.geometry->objectCount = 2;
    CHECK_FALSE(CombatPacket::encode(mismatch));
    (*bytes)[4] = 32;
    CHECK_FALSE(CombatPacket::decode(*bytes));
}
TEST_CASE("combat packets omit empty platform sections canonically",
          "[netplay][platform-checkpoint][combat-snapshot]") {
    CombatSnapshot source;
    source.motion.epoch = 9;
    source.motion.tick = 10;
    source.motion.cameraContinuity = 1;
    const auto canonical = CombatPacket::encode(source);
    REQUIRE(canonical);
    const auto empty = PlatformCheckpoint::encode(PlatformState{});
    REQUIRE(empty);
    auto broken = *canonical;
    writeWord(broken, CombatPacket::kHeaderBytes - 4, static_cast<u32>(empty->size()));
    broken.insert(broken.end(), empty->begin(), empty->end());
    CHECK_FALSE(CombatPacket::decode(broken));
}
} // namespace
