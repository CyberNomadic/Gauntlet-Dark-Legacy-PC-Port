#include <array>
#include <bit>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/GeneratorCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void word(std::vector<u8>& bytes, usize at, u32 value) {
    REQUIRE(at + 4 <= bytes.size());
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes[at + shift / 8] = static_cast<u8>(value >> shift);
    }
}

Generators::Checkpoint sample() {
    Generators::Checkpoint state;
    Generators::Slot slot;
    slot.minPlayers = -2;
    slot.presence = Generators::Slot::Presence::Hidden;
    slot.kind = kGruntKind;
    slot.tier = 3;
    slot.algorithm = kPatrolWay;
    slot.most = 7;
    slot.interval = 13;
    slot.health = 23;
    slot.threshold = 11;
    slot.armor = -2.5f;
    slot.state = 2;
    slot.countdown = -17;
    slot.ratio = 0.375f;
    slot.bred = 19;
    slot.living = 5;
    slot.position = {2, 3, -4};
    slot.collisionOffset = {-2, 1, 3};
    slot.targetRadius = 4;
    slot.targetHeight = 8;
    slot.placement = glm::rotate(glm::translate(Mat4{1}, Vec3{1, 3, 7}), 0.3f, Vec3{0, 1, 0});
    slot.yaw = -0.8f;
    slot.direction = {0.6f, 0, 0.8f};
    slot.clearance = 9;
    slot.patrolClearance = 3;
    slot.viewRadius = 32;
    slot.alwaysActive = true;
    slot.box.centre = {3, 5, 7};
    slot.box.yaw = 0.7f;
    slot.box.halfAcross = 4;
    slot.box.halfAlong = 5;
    slot.box.height = 6;
    slot.box.solid = false;
    slot.box.cylinderRadius = 2.5f;
    slot.box.enemyItem = Obstacle::ItemQuery{true, 3.5f};
    slot.support = 4;
    slot.supportLocal = glm::rotate(glm::translate(Mat4{1}, Vec3{6, -2, 8}), 0.4f, Vec3{1, 0, 0});
    slot.boxOffset = {9, -4, 5};
    slot.boxYaw = -0.9f;
    state.roster.push_back(slot);
    slot.position = {5, 9, 11};
    slot.placement[3] = Vec4{slot.position, 1};
    slot.presence = Generators::Slot::Presence::Suppressed;
    slot.health = 7;
    slot.state = 1;
    slot.tier = 2;
    slot.algorithm = kChaseWay;
    slot.countdown = 28;
    slot.bred = 30;
    slot.living = 8;
    slot.ratio = 0.75f;
    slot.support = -1;
    state.generators.push_back(slot);
    state.bodies.push_back({kGruntKind, {127, 0.007f, 0.5f}});
    state.scales = {1.75f, 0.5f, 2.25f};
    state.players = 3;
    state.specialBirth = 0xABCDEF12;
    state.view.emplace();
    state.view->position = {13, 17, -19};
    state.view->right = {0.8f, 0, -0.6f};
    state.view->up = {0, 1, 0};
    state.view->forward = {0.6f, 0, 0.8f};
    state.view->sinX = 0.6f;
    state.view->cosX = 0.8f;
    state.view->sinY = 0.8f;
    state.view->cosY = 0.6f;
    state.attention = Vec3{-7, 8, 9};
    REQUIRE(state.valid());
    return state;
}

TEST_CASE("generator checkpoint wire preserves every native field and optional context",
          "[rollback][netplay][generator-checkpoint]") {
    auto state = sample();
    const bool view = GENERATE(false, true);
    const bool attention = GENERATE(false, true);
    const bool query = GENERATE(false, true);
    if (!view) {
        state.view.reset();
    }
    if (!attention) {
        state.attention.reset();
    }
    if (!query) {
        state.roster.front().box.enemyItem.reset();
        state.generators.front().box.enemyItem.reset();
    }
    const auto bytes = GeneratorCheckpoint::encode(state);
    REQUIRE(bytes);
    CHECK(bytes->size() ==
          GeneratorCheckpoint::kHeaderBytes + (view ? GeneratorCheckpoint::kViewBytes : 0) +
              (attention ? GeneratorCheckpoint::kAttentionBytes : 0) +
              GeneratorCheckpoint::kBodyBytes + 2 * GeneratorCheckpoint::kSlotBytes);
    CHECK(readU32LE(*bytes, 0) == 1);
    CHECK(readU32LE(*bytes, 36) == state.specialBirth);
    CHECK((*bytes)[36] == 0x12);
    CHECK((*bytes)[37] == 0xEF);
    CHECK((*bytes)[38] == 0xCD);
    CHECK((*bytes)[39] == 0xAB);
    const auto decoded = GeneratorCheckpoint::decode(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(GeneratorCheckpoint::encode(*decoded) == bytes);

    Generators::Checkpoint empty;
    REQUIRE(empty.valid());
    const auto emptyBytes = GeneratorCheckpoint::encode(empty);
    REQUIRE(emptyBytes);
    CHECK(emptyBytes->size() == GeneratorCheckpoint::kHeaderBytes);
    CHECK(GeneratorCheckpoint::decode(*emptyBytes) == empty);
}

TEST_CASE("generator checkpoint rejects truncated extra oversized and inconsistent lengths",
          "[rollback][netplay][generator-checkpoint]") {
    const auto bytes = GeneratorCheckpoint::encode(sample());
    REQUIRE(bytes);
    for (usize count = 0; count < bytes->size(); ++count) {
        CAPTURE(count);
        CHECK_FALSE(GeneratorCheckpoint::decode(std::span{*bytes}.first(count)));
    }
    auto invalid = *bytes;
    invalid.push_back(0);
    CHECK_FALSE(GeneratorCheckpoint::decode(invalid));
    invalid.resize(GeneratorCheckpoint::kMaxBytes + 1);
    CHECK_FALSE(GeneratorCheckpoint::decode(invalid));
    const usize offset = GENERATE(usize{4}, usize{8}, usize{12});
    for (const u32 count : {0U, 2U, 513U, std::numeric_limits<u32>::max()}) {
        CAPTURE(offset, count);
        invalid = *bytes;
        word(invalid, offset, count);
        CHECK_FALSE(GeneratorCheckpoint::decode(invalid));
    }
}

TEST_CASE("generator checkpoint validates version scalar enums flags and absent fields",
          "[rollback][netplay][generator-checkpoint]") {
    auto state = sample();
    state.view.reset();
    state.attention.reset();
    state.roster.front().box.enemyItem.reset();
    state.generators.front().box.enemyItem.reset();
    const auto bytes = GeneratorCheckpoint::encode(state);
    REQUIRE(bytes);
    constexpr usize body = GeneratorCheckpoint::kHeaderBytes;
    constexpr usize roster = body + GeneratorCheckpoint::kBodyBytes;
    constexpr usize current = roster + GeneratorCheckpoint::kSlotBytes;
    const auto rejects = [&](usize offset, u32 value) {
        auto changed = *bytes;
        word(changed, offset, value);
        CHECK_FALSE(GeneratorCheckpoint::decode(changed));
    };
    rejects(0, 0);
    rejects(0, 2);
    rejects(16, 4);
    rejects(16, 1); // declared view lacks its bytes
    rejects(32, 5);
    rejects(20, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()));
    rejects(body + 8, std::bit_cast<u32>(std::numeric_limits<f32>::infinity()));
    rejects(current + 4, 3);     // presence enum
    rejects(current + 4, 0x100); // not a truncated u8 enum
    rejects(current + 8, static_cast<u32>(kEnemyKindCount));
    rejects(current + 48, std::bit_cast<u32>(1.1f));  // birth ratio
    rejects(current + 184, 32);                       // unknown packed boolean bit
    rejects(current + 184, 2);                        // uncheckpointed boss figure
    rejects(current + 184, 16);                       // cylindrical query without query presence
    rejects(current + 220, std::bit_cast<u32>(0.5f)); // absent query radius
    rejects(current + 220, 0x80000000);               // absent is canonical +0, not -0
    rejects(current + 32, std::bit_cast<u32>(12.0f)); // different immutable threshold
    rejects(current + 300, std::bit_cast<u32>(std::numeric_limits<f32>::infinity()));
    for (const usize offset : {roster + 92, current + 228}) {
        rejects(offset, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()));
    }
}

TEST_CASE("generator transport caps do not truncate larger locally valid checkpoints",
          "[rollback][netplay][generator-checkpoint]") {
    auto state = sample();
    state.roster.resize(GeneratorCheckpoint::kMaxGenerators, state.roster.front());
    state.generators.resize(GeneratorCheckpoint::kMaxGenerators, state.generators.front());
    state.bodies.clear();
    state.bodies.push_back({-2, {}});
    state.bodies.push_back({-3, {}});
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        state.bodies.push_back({kind, {}});
    }
    REQUIRE(state.valid());
    const auto maximum = GeneratorCheckpoint::encode(state);
    REQUIRE(maximum);
    CHECK(maximum->size() == GeneratorCheckpoint::kMaxBytes);
    CHECK(GeneratorCheckpoint::decode(*maximum) == state);
    state.roster.push_back(state.roster.front());
    state.generators.push_back(state.generators.front());
    REQUIRE(state.valid());
    CHECK_FALSE(GeneratorCheckpoint::encode(state));
    state.roster.resize(Generators::Checkpoint::kMaxGenerators, state.roster.front());
    state.generators.resize(Generators::Checkpoint::kMaxGenerators, state.generators.front());
    REQUIRE(state.valid());
    CHECK_FALSE(GeneratorCheckpoint::encode(state));
}

TEST_CASE("generator wire rejects malformed native values rather than normalizing them",
          "[rollback][netplay][generator-checkpoint]") {
    auto invalid = sample();
    const s32 field = GENERATE(0, 1, 2, 3, 4, 5);
    if (field == 0) {
        invalid.generators.front().health = -1;
    } else if (field == 1) {
        invalid.generators.front().boss = true;
    } else if (field == 2) {
        invalid.bodies.push_back(invalid.bodies.front());
    } else if (field == 3) {
        invalid.generators.front().box.halfAlong += 1;
    } else if (field == 4) {
        invalid.view->sinX = 2;
    } else {
        invalid.attention->y = std::numeric_limits<f32>::infinity();
    }
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(GeneratorCheckpoint::encode(invalid));
}

TEST_CASE("decoded generator checkpoint restores the native brood and texture continuation",
          "[rollback][netplay][generator-checkpoint][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    const auto directory = test::scratchDirectory("generator-wire");
    writeTextFile(directory / "world.json", R"({
      "objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,
                    "collisionType":1,"hitPoints":10}],
      "itemInstances":[
        {"info":0,"minPlayers":1,"position":[0,0,0],
         "params":[2,0,7,0,2,0,1,0,0,0,0,0]},
        {"info":0,"minPlayers":2,"position":[40,0,0],
         "params":[1,0,15,0,1,0,1,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 8, {}, 832);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {1.5f, 1.25f, 1.5f}, 1));
    const std::array players{EnemyView{.player = 0, .position = {0, 0, 25}}};
    generators.update(1, enemies, players);
    enemies.update(1, 1.0f / 60, players, generators.enemyObstacles());
    const auto baseline = generators.checkpoint();
    const auto baselineEnemies = enemies.checkpoint();
    REQUIRE(baseline);
    REQUIRE(baselineEnemies);
    const auto encoded = GeneratorCheckpoint::encode(*baseline);
    REQUIRE(encoded);
    const auto decoded = GeneratorCheckpoint::decode(*encoded);
    REQUIRE(decoded);
    REQUIRE(*decoded == *baseline);
    const auto uploads = device.texturesCreated;
    std::vector<Generators::Checkpoint> expected;
    std::vector<Enemies::Checkpoint> expectedEnemies;
    const auto step = [&](s32 tick) {
        generators.setPlayerCount(tick >= 8 ? 2 : 1);
        if (tick == 12) {
            EnemyHit hit;
            hit.player = 0;
            hit.damage = 1000;
            REQUIRE(enemies.hurt(0, hit));
        }
        const bool stopped = tick >= 16 && tick < 21;
        generators.update(1, enemies, players, {}, stopped);
        enemies.update(1, 1.0f / 60, players, generators.enemyObstacles(), nullptr, 1, stopped);
    };
    for (s32 tick = 0; tick < 48; ++tick) {
        step(tick);
        const auto state = generators.checkpoint();
        const auto swarm = enemies.checkpoint();
        REQUIRE(state);
        REQUIRE(swarm);
        expected.push_back(*state);
        expectedEnemies.push_back(*swarm);
    }
    REQUIRE(generators.restore(*decoded));
    REQUIRE(enemies.restore(*baselineEnemies));
    for (s32 tick = 0; tick < 48; ++tick) {
        CAPTURE(tick);
        step(tick);
        CHECK(generators.checkpoint() == expected[static_cast<usize>(tick)]);
        CHECK(enemies.checkpoint() == expectedEnemies[static_cast<usize>(tick)]);
    }
    CHECK(device.texturesCreated == uploads);
    auto foreign = *decoded;
    foreign.roster.front().position.x += 1;
    REQUIRE(foreign.valid());
    const auto foreignBytes = GeneratorCheckpoint::encode(foreign);
    REQUIRE(foreignBytes);
    const auto foreignDecoded = GeneratorCheckpoint::decode(*foreignBytes);
    REQUIRE(foreignDecoded);
    const auto before = generators.checkpoint();
    CHECK_FALSE(generators.restore(*foreignDecoded));
    CHECK(generators.checkpoint() == before);
}
} // namespace
