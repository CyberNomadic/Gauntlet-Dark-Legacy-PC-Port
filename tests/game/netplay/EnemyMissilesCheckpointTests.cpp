#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/ByteReader.h"

#include "game/netplay/EnemyMissilesCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using State = EnemyMissiles::Checkpoint;
using Codec = EnemyMissilesCheckpoint;

void writeWord(std::vector<u8>& bytes, usize offset, u32 value) {
    REQUIRE(offset + 4 <= bytes.size());
    for (usize byte = 0; byte < 4; ++byte) {
        bytes[offset + byte] = static_cast<u8>(value >> (byte * 8));
    }
}

State richState() {
    State state;
    state.nextInstance = 0x1122334455667788ULL;
    Mt19937 random(4321);
    for (usize index = 0; index < 623; ++index) {
        random();
    }
    state.random = random.checkpoint();
    state.shrink = 0.667f;
    state.ricochetIn = 0.6875f;
    State::Flight flight;
    flight.instance = 0x0102030405060708ULL;
    flight.continuity = 7;
    flight.position = {1.25f, -2.5f, 3.75f};
    flight.velocity = {4.5f, -5.25f, 6.875f};
    flight.turned = {-0.125f, 0.25f, 0.5f};
    flight.kind = EnemyMissileKind::deathShot();
    flight.kind.flags = 0xfedcba98U; // native damage flags are opaque, not packet-control flags
    flight.kind.damage = 12.125f;
    flight.kind.speed = 23.875f;
    flight.kind.radius = 0.75f;
    flight.kind.burstRadius = 3.25f;
    flight.kind.spin = {1, 2, -0.5f};
    flight.kind.weight = 0.25f;
    flight.kind.held = 0.5f;
    flight.resource = 0xaabbccddU;
    flight.shooter = Enemies::kMost - 1;
    flight.secondsLeft = 2.25f;
    flight.lived = 0.123f;
    flight.heldLeft = 0.125f;
    flight.scale = 0.667f;
    flight.reflected = true;
    flight.pierced = {{0, 0.125f}, {3, 0.25f}};
    state.missiles.push_back(flight);

    EnemyMissiles::Burst burst;
    burst.instance = 0x0102030405060709ULL;
    burst.blast.position = {-10, 3, 0.5f};
    burst.blast.radius = 7.5f;
    burst.blast.damage = 11.125f;
    burst.blast.flags = 0x89abcdefU;
    burst.blast.stages = {0.25f, 0.5f, 1.25f};
    burst.blast.spared = 2;
    burst.blast.lit = true;
    burst.stage = 1;
    burst.stageLeft = 0.375f;
    burst.players = {{2, std::numeric_limits<f32>::infinity()}, {1, 0.125f}};
    burst.swarm = {{41, 0.33f}, {42, 0.66f}};
    burst.scenery = {5, 6, 5}; // callback-owned order and duplicates must survive intact
    state.bursts.push_back(burst);

    EnemyMissileHit hit;
    hit.instance = 0x010203040506070aULL;
    hit.player = 3;
    hit.shooter = Enemies::kMost - 1;
    hit.damage = 1.75f;
    hit.flags = 0x76543210U;
    hit.burstRadius = 2.25f;
    hit.position = {1, 2, 3};
    hit.direction = {-0.5f, 0.25f, 0.75f};
    hit.worldContact = true;
    hit.fromBurst = true;
    hit.ricochet = true;
    hit.target = 999;
    hit.worldObject = 234;
    hit.liquid = true;
    state.hits.push_back(hit);
    state.rockHits.push_back({1234, 2.5f});
    state.pickupBlasts.push_back({{-3, 2, 1}, 3.75f, 4.25f, 0xabcdef01U});
    REQUIRE(state.valid());
    return state;
}

State capture(const EnemyMissiles& pool) {
    const auto state = pool.checkpoint();
    REQUIRE(state);
    return *state;
}

TEST_CASE("enemy missile packets preserve every scalar vector queue and opaque damage flag",
          "[netplay][enemy-missiles-wire]") {
    const auto state = richState();
    const auto bytes = Codec::encode(state);
    REQUIRE(bytes);
    REQUIRE(bytes->size() == Codec::kHeaderBytes + Codec::kFlightBytes + 16 + Codec::kBurstBytes +
                                 12 + 16 + 16 + 12 + Codec::kHitBytes + Codec::kRockBytes +
                                 Codec::kPickupBytes);
    CHECK(readU32LE(*bytes, 0) == fourcc("GDEM"));
    CHECK(readU32LE(*bytes, 4) == 2);
    CHECK(readU32LE(*bytes, 8) == 0x55667788U);
    CHECK(readU32LE(*bytes, 12) == 0x11223344U);
    CHECK(readU32LE(*bytes, Codec::kHeaderBytes) == 0x05060708U);
    CHECK(readU32LE(*bytes, Codec::kHeaderBytes + 4) == 0x01020304U);
    const auto decoded = Codec::decode(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(Codec::encode(*decoded) == bytes);
    CHECK(decoded->bursts[0].players[0].secondsLeft == std::numeric_limits<f32>::infinity());
    CHECK(decoded->missiles[0].resource == 0xaabbccddU);
    // Decoding never grants access to a process pointer or manufactures missing artwork.
    EnemyMissiles pool;
    const auto before = capture(pool);
    CHECK_FALSE(pool.restore(*decoded));
    CHECK(capture(pool) == before);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<EnemyMissiles>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<EnemyMissiles>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<EnemyMissiles>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<EnemyMissiles>);
}

TEST_CASE("enemy missile packet decoding rejects every truncation trailing byte and hostile count",
          "[netplay][enemy-missiles-wire]") {
    const auto bytes = Codec::encode(richState());
    REQUIRE(bytes);
    for (usize size = 0; size < bytes->size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(Codec::decode(std::span{*bytes}.first(size)));
    }
    auto trailing = *bytes;
    trailing.push_back(0);
    CHECK_FALSE(Codec::decode(trailing));
    CHECK_FALSE(Codec::decode(std::vector<u8>(Codec::kMaxBytes + 1, 0)));

    constexpr usize kFlight = Codec::kHeaderBytes;
    constexpr usize kBurst = kFlight + Codec::kFlightBytes + 16;
    constexpr usize kHit = kBurst + Codec::kBurstBytes + 12 + 16 + 16 + 12;
    const std::array<std::pair<usize, u32>, 24> edits{{
        {0, 0},
        {4, 0},
        {4, 3},
        {24, static_cast<u32>(Codec::kMaxMissiles + 1)},
        {28, static_cast<u32>(Codec::kMaxBursts + 1)},
        {32, static_cast<u32>(Codec::kMaxEvents + 1)},
        {36, static_cast<u32>(Codec::kMaxEvents)}, // other queues make the aggregate too large
        {40, std::numeric_limits<u32>::max()},
        {44 + RandomCheckpoint::kBytes - 4, 625},
        {kFlight + 8, 0},
        {kFlight + 48, 4},
        {kFlight + 96, Enemies::kMost},
        {kFlight + 116, 4},
        {kFlight + 120, 5},
        {kFlight + 120, std::numeric_limits<u32>::max()},
        {kFlight + Codec::kFlightBytes, 4},
        {kBurst + 28, 3},
        {kBurst + 36, 2},
        {kBurst + 40, 0},
        {kBurst + 40, static_cast<u32>(Codec::kMaxStages + 1)},
        {kBurst + 44, 5},
        {kBurst + 48, static_cast<u32>(Codec::kMaxContacts + 1)},
        {kBurst + 52, std::numeric_limits<u32>::max()},
        {kHit + 44, 16},
    }};
    for (const auto& [offset, value] : edits) {
        CAPTURE(offset, value);
        auto malformed = *bytes;
        writeWord(malformed, offset, value);
        CHECK_FALSE(Codec::decode(malformed));
    }
}

TEST_CASE("enemy missile diagnostic identities preserve retired sources and reject collisions",
          "[enemy-missiles-wire][native-combat-audio]") {
    const auto baseline = richState();
    auto state = baseline;
    state.missiles.clear();
    // An impact remains valid after its flight was removed or its shooter reused.
    REQUIRE(state.valid());
    const auto retired = Codec::encode(state);
    REQUIRE(retired);
    CHECK(Codec::decode(*retired) == state);
    state = baseline;
    state.bursts[0].instance = state.missiles[0].instance;
    CHECK_FALSE(state.valid());
    CHECK_FALSE(Codec::encode(state));
    state = baseline;
    state.bursts[0].instance = state.nextInstance;
    CHECK_FALSE(state.valid());
    state = baseline;
    state.hits[0].instance = state.nextInstance;
    CHECK_FALSE(state.valid());
    const auto bytes = Codec::encode(baseline);
    REQUIRE(bytes);
    constexpr usize kBurst = Codec::kHeaderBytes + Codec::kFlightBytes + 16;
    constexpr usize kHit = kBurst + Codec::kBurstBytes + 12 + 16 + 16 + 12;
    for (const usize offset : {kBurst + 56, kHit + 56}) {
        auto malformed = *bytes;
        writeWord(malformed, offset, static_cast<u32>(baseline.nextInstance));
        writeWord(malformed, offset + 4, static_cast<u32>(baseline.nextInstance >> 32));
        CHECK_FALSE(Codec::decode(malformed));
    }
    // Old synthetic/offline event fixtures have no emitter. Decoding these does
    // not authorize a recorded runtime to substitute a current live source.
    state = baseline;
    state.bursts[0].instance = 0;
    state.hits[0].instance = 0;
    REQUIRE(state.valid());
    REQUIRE(Codec::encode(state));
    CHECK(Codec::decode(*Codec::encode(state)) == state);
}

TEST_CASE("enemy missile packets allow infinity only for the spared burst player",
          "[netplay][enemy-missiles-wire]") {
    const auto encoded = Codec::encode(richState());
    REQUIRE(encoded);
    constexpr usize kFlight = Codec::kHeaderBytes;
    constexpr usize kBurst = kFlight + Codec::kFlightBytes + 16;
    constexpr usize kPlayers = kBurst + Codec::kBurstBytes + 12;
    constexpr usize kSwarm = kPlayers + 16;
    constexpr usize kHit = kSwarm + 16 + 12;
    const std::array<usize, 21> finiteOffsets{
        16,
        20,
        kFlight + 12,
        kFlight + 24,
        kFlight + 36,
        kFlight + 56,
        kFlight + 60,
        kFlight + 64,
        kFlight + 68,
        kFlight + 72,
        kFlight + 84,
        kFlight + 88,
        kFlight + 100,
        kFlight + 104,
        kFlight + 108,
        kFlight + 112,
        kFlight + Codec::kFlightBytes + 4,
        kBurst + 32,
        kBurst + Codec::kBurstBytes,
        kPlayers + 12,
        kSwarm + 4,
    };
    for (const usize offset : finiteOffsets) {
        for (const u32 bits : {0x7f800000U, 0xff800000U, 0x7fc00001U}) {
            CAPTURE(offset, bits);
            auto malformed = *encoded;
            writeWord(malformed, offset, bits);
            CHECK_FALSE(Codec::decode(malformed));
        }
    }
    for (const usize offset : {kHit + 8, kHit + Codec::kHitBytes + 4,
                               kHit + Codec::kHitBytes + Codec::kRockBytes + 16}) {
        auto malformed = *encoded;
        writeWord(malformed, offset, 0x7f800000U);
        CHECK_FALSE(Codec::decode(malformed));
    }
    for (const u32 bits : {0U, std::bit_cast<u32>(1.0f), 0xff800000U, 0x7fc00001U}) {
        auto malformed = *encoded;
        writeWord(malformed, kPlayers + 4, bits);
        CHECK_FALSE(Codec::decode(malformed));
    }
    auto wrongSpared = *encoded;
    writeWord(wrongSpared, kBurst + 24, 3);
    CHECK_FALSE(Codec::decode(wrongSpared));
}

TEST_CASE("enemy missile wire bounds are exact and refusal leaves oversized native state intact",
          "[netplay][enemy-missiles-wire]") {
    State maximum;
    auto flight = richState().missiles.front();
    flight.pierced.clear();
    flight.resource = 0;
    maximum.nextInstance = Codec::kMaxMissiles + 1;
    for (usize index = 0; index < Codec::kMaxMissiles; ++index) {
        flight.instance = index + 1;
        maximum.missiles.push_back(flight);
    }
    EnemyMissiles::Burst burst;
    burst.blast.stages = {1};
    burst.stageLeft = 1;
    maximum.bursts.resize(Codec::kMaxBursts, burst);
    maximum.bursts[0].blast.stages.resize(Codec::kMaxStages - Codec::kMaxBursts + 1, 1);
    for (usize index = 0; index < Codec::kMaxContacts; ++index) {
        maximum.bursts[0].swarm.push_back({static_cast<s32>(index), 0.5f});
    }
    maximum.hits.resize(Codec::kMaxEvents);
    REQUIRE(maximum.valid());
    const auto bytes = Codec::encode(maximum);
    REQUIRE(bytes);
    REQUIRE(bytes->size() == Codec::kMaxBytes);
    const auto decoded = Codec::decode(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == maximum);

    const u32 extra = GENERATE(0U, 1U, 2U, 3U, 4U);
    CAPTURE(extra);
    auto oversized = maximum;
    switch (extra) {
    case 0:
        flight.instance = oversized.nextInstance++;
        oversized.missiles.push_back(flight);
        break;
    case 1: oversized.bursts.push_back(burst); break;
    case 2: oversized.rockHits.push_back({0, 0}); break;
    case 3: oversized.bursts[0].scenery.push_back(0); break;
    case 4: oversized.bursts[0].blast.stages.push_back(1); break;
    }
    REQUIRE(oversized.valid());
    const auto before = oversized;
    CHECK_FALSE(Codec::encode(oversized));
    CHECK(oversized == before);
}

TEST_CASE("enemy missile wire rejection preserves duplicate identity and exhausted counter rules",
          "[netplay][enemy-missiles-wire]") {
    auto state = richState();
    state.missiles.push_back(state.missiles[0]);
    CHECK_FALSE(Codec::encode(state));
    state.missiles.pop_back();
    state.missiles[0].pierced.push_back(state.missiles[0].pierced[0]);
    CHECK_FALSE(Codec::encode(state));
    state.missiles[0].pierced.pop_back();
    state.bursts[0].swarm.push_back(state.bursts[0].swarm[0]);
    CHECK_FALSE(Codec::encode(state));
    state.bursts[0].swarm.pop_back();
    state.nextInstance = 0;
    state.missiles[0].instance = std::numeric_limits<u64>::max();
    const auto bytes = Codec::encode(state);
    REQUIRE(bytes);
    const auto decoded = Codec::decode(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == state); // zero is native counter exhaustion, not an invalid birth ID
    state.missiles[0].instance = 0;
    CHECK_FALSE(Codec::encode(state));
}

TEST_CASE("native enemy missiles continue identically after a transported correction",
          "[netplay][enemy-missiles-wire]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    EnemyMissiles native(125);
    auto held = EnemyMissileKind::deathShot();
    held.held = 0.437f;
    native.launch(held, {0, 3, 0}, {0, 3, 60}, 1, nullptr, 5);
    native.launch(EnemyMissileKind::bolt(20, 25, 0.3f), {-20, 3, 0}, {-20, 3, 20}, 1, nullptr, 2);
    native.launch(EnemyMissileKind::bomb(), {30, 8, 0}, {30, 0, 25}, 1, nullptr, 3);
    EnemyBlast gas;
    gas.position = {12, 3, 0};
    gas.radius = 7.5f;
    gas.damage = 10;
    gas.flags = EnemyBlast::kGas;
    gas.stages = {0.27f, 0.43f, 0.33f};
    gas.spared = 2;
    gas.lit = true;
    native.blast(gas);
    std::array<EnemyView, 4> players;
    const std::array<Vec3, 4> positions{{{0, 0, 0.5f}, {-20, 0, 5}, {12, 0, 0}, {13, 0, 0}}};
    for (usize index = 0; index < players.size(); ++index) {
        players[index].player = static_cast<s32>(index);
        players[index].position = positions[index];
    }
    players[1].reflects = true;
    const std::array<MissileTarget, 2> swarm{MissileTarget{7, {-20, 0, -6}, 1, 6},
                                             MissileTarget{9, {12, 0, 1}, 1, 6}};
    native.update(0.173f, nullptr, players, swarm);
    const auto baseline = capture(native);
    REQUIRE(baseline.missiles[0].heldLeft > 0);
    REQUIRE_FALSE(baseline.missiles[0].pierced.empty());
    REQUIRE(baseline.missiles[1].reflected);
    REQUIRE_FALSE(baseline.hits.empty());
    const auto bytes = Codec::encode(baseline);
    REQUIRE(bytes);
    const auto decoded = Codec::decode(*bytes);
    REQUIRE(decoded);
    EnemyMissiles replay(999);
    REQUIRE(replay.restore(*decoded));
    for (s32 tick = 0; tick < 90; ++tick) {
        CAPTURE(tick, seconds);
        if (tick % 9 == 0) {
            EnemyMissileLaunch launch;
            launch.body = {50, 3, 0};
            launch.target = {50, 3, 20};
            launch.aimError = 0.35f;
            launch.facing = tick % 18 == 0 ? kPi : 0;
            REQUIRE(native.launch(EnemyMissileKind::bomb(), launch) ==
                    replay.launch(EnemyMissileKind::bomb(), launch));
        }
        native.update(seconds, nullptr, players, swarm);
        replay.update(seconds, nullptr, players, swarm);
        const auto expected = capture(native);
        CHECK(capture(replay) == expected);
        const auto transmitted = Codec::encode(expected);
        REQUIRE(transmitted);
        const auto correction = Codec::decode(*transmitted);
        REQUIRE(correction);
        REQUIRE(replay.restore(*correction));
        CHECK(replay.takeHits() == native.takeHits());
        CHECK(replay.takeRockHits() == native.takeRockHits());
        CHECK(replay.takePickupBlasts() == native.takePickupBlasts());
    }
}
} // namespace
