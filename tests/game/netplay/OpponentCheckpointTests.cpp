#include <array>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/OpponentCheckpoint.h"
#include "game/screens/OpponentSimulation.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void putWord(std::vector<u8>& bytes, usize offset, u32 value) {
    REQUIRE(offset + 4 <= bytes.size());
    for (u32 byte = 0; byte < 4; ++byte) {
        bytes[offset + byte] = static_cast<u8>(value >> (8 * byte));
    }
}

// Decoder safety is a value-level contract and must run on machines without game data.
// Trusted resource binding and actual continuation remain in the native Island test.
OpponentSimulation::Checkpoint syntheticState() {
    OpponentSimulation::Checkpoint state;
    Generators::Slot generator;
    generator.kind = kGruntKind;
    state.generators.roster.push_back(generator);
    state.generators.generators.push_back(generator);
    state.generators.bodies.push_back({kGruntKind, {}});
    // An inactive/recycled slot can retain its previous generator reference.
    state.enemies.enemies.front().generator = 0;
    state.enemies.generatorEvents.push_back({0, EnemyGeneratorEvent::Kind::PatrolHit});
    REQUIRE(state.valid());
    return state;
}

struct Island {
    test::FakeRenderDevice device;
    WorldLayout layout;
    WorldCollision collision;
    Enemies enemies;
    Generators generators;
    EnemyMissiles missiles{123};
    OpponentSimulation simulation{generators, enemies, missiles};
    std::array<EnemyView, 1> players;

    Island() {
        const auto root =
            test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
        const auto directory = test::scratchDirectory("opponent-checkpoint-wire");
        writeTextFile(directory / "world.json", R"({
            "objects":[{"name":"FLOOR","position":[0,0,0]}],
            "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":10}],
            "itemInstances":[{"info":0,"minPlayers":1,"position":[30,0,30],
                              "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})");
        REQUIRE(layout.load(directory));
        collision.build(
            {{{0, 1, 0}, {Vec3{-200, 0, -200}, Vec3{200, 0, 200}, Vec3{200, 0, -200}}, 0, 4},
             {{0, 1, 0}, {Vec3{-200, 0, -200}, Vec3{-200, 0, 200}, Vec3{200, 0, 200}}, 0, 4}});
        enemies.open(device, root, &collision, Enemies::kMost, {}, 456);
        REQUIRE(enemies.loadKind(kGruntKind));
        REQUIRE(generators.bind(device, layout, enemies, &collision, {}, 1));
        players[0].player = 0;
        players[0].position = {0, 0, 0};
        REQUIRE(enemies.spawn(
            {.kind = kGruntKind, .algorithm = kChaseWay, .position = {0, 0, 22}, .placed = true},
            players));
        missiles.launch(EnemyMissileKind::arrow(), {-20, 6, 20}, {-20, 6, 60}, 1, nullptr, -1);
    }

    void advance() {
        OpponentSimulation::Frame frame;
        frame.players = players;
        frame.collision = &collision;
        simulation.advance(frame);
    }
};

TEST_CASE("opponent wire checkpoint restores all three native pools before continuation",
          "[rollback][opponent-checkpoint][assets]") {
    Island island;
    for (s32 tick = 0; tick < 17; ++tick) {
        island.advance();
    }
    const auto initial = island.simulation.checkpoint();
    REQUIRE(initial);
    const auto bytes = OpponentCheckpoint::encode(*initial);
    REQUIRE(bytes);
    const auto decoded = OpponentCheckpoint::decode(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == *initial);
    CHECK(OpponentCheckpoint::encode(*decoded) == bytes);
    const auto uploads = island.device.texturesCreated;
    std::vector<OpponentSimulation::Checkpoint> future;
    for (s32 tick = 0; tick < 40; ++tick) {
        island.advance();
        const auto current = island.simulation.checkpoint();
        REQUIRE(current);
        future.push_back(*current);
    }
    REQUIRE(island.simulation.restore(*decoded));
    for (const auto& expected : future) {
        island.advance();
        CHECK(island.simulation.checkpoint() == expected);
    }
    CHECK(island.device.texturesCreated == uploads);
}

TEST_CASE("opponent wire checkpoint rejects incomplete oversized or unknown envelopes",
          "[rollback][opponent-checkpoint]") {
    const auto initial = syntheticState();
    const auto bytes = OpponentCheckpoint::encode(initial);
    REQUIRE(bytes);
    const auto decoded = OpponentCheckpoint::decode(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == initial);
    for (usize length = 0; length < bytes->size(); ++length) {
        CAPTURE(length);
        CHECK_FALSE(OpponentCheckpoint::decode(std::span{*bytes}.first(length)));
    }
    auto bad = *bytes;
    bad.push_back(0);
    CHECK_FALSE(OpponentCheckpoint::decode(bad));
    bad.assign(OpponentCheckpoint::kMaxBytes + 1, 0);
    CHECK_FALSE(OpponentCheckpoint::decode(bad));
    for (const usize offset : {0U, 4U, 8U, 12U, 16U}) {
        for (const u32 value : {0U, std::numeric_limits<u32>::max()}) {
            bad = *bytes;
            putWord(bad, offset, value);
            CHECK_FALSE(OpponentCheckpoint::decode(bad));
        }
    }
    const usize generatorsSize = readU32LE(*bytes, 8);
    const usize enemiesSize = readU32LE(*bytes, 12);
    for (const usize offset :
         {OpponentCheckpoint::kHeaderBytes, OpponentCheckpoint::kHeaderBytes + generatorsSize,
          OpponentCheckpoint::kHeaderBytes + generatorsSize + enemiesSize}) {
        bad = *bytes;
        putWord(bad, offset, 0); // malformed child with a structurally unchanged outer envelope
        CHECK_FALSE(OpponentCheckpoint::decode(bad));
    }
    CHECK(OpponentCheckpoint::encode(initial) == bytes);
}

TEST_CASE("opponent wire rejects individually valid components with inconsistent references",
          "[rollback][opponent-checkpoint]") {
    auto state = syntheticState();
    REQUIRE(state.generators.generators.size() == 1);
    const auto original = state;
    const bool event = GENERATE(false, true);
    if (event) {
        state.enemies.generatorEvents.push_back({1, EnemyGeneratorEvent::Kind::PatrolHit});
    } else {
        state.enemies.enemies.front().generator = 1;
    }
    REQUIRE(state.enemies.valid());
    CHECK_FALSE(state.valid());
    CHECK_FALSE(OpponentCheckpoint::encode(state));
    // Construct a structurally correct outer message from individually valid
    // components. The bundle still must reject the cross-pool reference.
    const auto generators = GeneratorCheckpoint::encode(state.generators);
    const auto enemies = EnemyPoolCheckpoint::encode(state.enemies);
    const auto missiles = EnemyMissilesCheckpoint::encode(state.missiles);
    REQUIRE(generators);
    REQUIRE(enemies);
    REQUIRE(missiles);
    const auto valid = OpponentCheckpoint::encode(original);
    REQUIRE(valid);
    std::vector<u8> bytes(valid->begin(), valid->begin() + OpponentCheckpoint::kHeaderBytes);
    putWord(bytes, 8, static_cast<u32>(generators->size()));
    putWord(bytes, 12, static_cast<u32>(enemies->size()));
    putWord(bytes, 16, static_cast<u32>(missiles->size()));
    bytes.insert(bytes.end(), generators->begin(), generators->end());
    bytes.insert(bytes.end(), enemies->begin(), enemies->end());
    bytes.insert(bytes.end(), missiles->begin(), missiles->end());
    CHECK_FALSE(OpponentCheckpoint::decode(bytes));
    CHECK(OpponentCheckpoint::decode(*valid) == original);
}

TEST_CASE("opponent wire carries complete optional placement state with an exact tail",
          "[rollback][opponent-checkpoint][opponent-placements]") {
    auto state = syntheticState();
    PlacedEnemies::Placement row;
    row.kind = row.spawn.kind = kGruntKind;
    row.spawn.generator = 0;
    row.spawn.position = {1, 2, 3};
    row.spawn.placed = true;
    row.viewRadius = 2;
    state.placements.emplace();
    state.placements->roster = {row, row};
    state.placements->pending = {1};
    state.placements->view.emplace();
    state.placements->attention = {5, 7, 11};
    REQUIRE(state.valid());
    const auto encoded = OpponentCheckpoint::encode(state);
    REQUIRE(encoded);
    CHECK(readU32LE(*encoded, 4) == 2);
    CHECK(OpponentCheckpoint::decode(*encoded) == state);
    const usize tailSize = readU32LE(*encoded, 20);
    const usize tail = encoded->size() - tailSize;
    REQUIRE(tailSize > 0);
    CHECK(PlacedEnemyCheckpoint::decode(std::span{*encoded}.subspan(tail)) == state.placements);

    for (const u32 length : {0U, 1U, static_cast<u32>(tailSize - 1), static_cast<u32>(tailSize + 1),
                             std::numeric_limits<u32>::max()}) {
        auto bad = *encoded;
        putWord(bad, 20, length);
        CHECK_FALSE(OpponentCheckpoint::decode(bad));
    }
    for (usize length = tail; length < encoded->size(); ++length) {
        CHECK_FALSE(OpponentCheckpoint::decode(std::span{*encoded}.first(length)));
    }
    auto bad = *encoded;
    putWord(bad, 4, 1);
    CHECK_FALSE(OpponentCheckpoint::decode(bad));
    bad = *encoded;
    putWord(bad, tail, 0);
    CHECK_FALSE(OpponentCheckpoint::decode(bad));

    // Every component is individually well-formed, but the placed spawn must
    // not name a generator that does not exist in this same authoritative tick.
    auto crossPool = state;
    crossPool.placements->roster[0].spawn.generator = 1;
    REQUIRE(crossPool.placements->valid());
    CHECK_FALSE(crossPool.valid());
    CHECK_FALSE(OpponentCheckpoint::encode(crossPool));
    const auto wrongTail = PlacedEnemyCheckpoint::encode(*crossPool.placements);
    REQUIRE(wrongTail);
    bad.assign(encoded->begin(), encoded->begin() + static_cast<std::ptrdiff_t>(tail));
    bad.insert(bad.end(), wrongTail->begin(), wrongTail->end());
    CHECK_FALSE(OpponentCheckpoint::decode(bad));

    // Absence is explicit and remains usable by isolated three-pool callers.
    bad.assign(encoded->begin(), encoded->begin() + static_cast<std::ptrdiff_t>(tail));
    putWord(bad, 20, 0);
    state.placements.reset();
    CHECK(OpponentCheckpoint::decode(bad) == state);
}
} // namespace
