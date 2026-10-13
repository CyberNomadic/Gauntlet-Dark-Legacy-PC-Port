#include <bit>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/netplay/CombatSnapshot.h"
#include "game/netplay/PlayerMeleeCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

PlayerMeleeState state() {
    PlayerMeleeState value;
    value.selection.feet = {1.5f, -2.25f, 3.75f};
    value.selection.reach = 1.75f;
    value.selection.range = 200;
    value.selection.facingDot = 0.5f;
    value.captureYaw = -1.25f;
    value.assistedAim = Vec3{4.5f, 6.25f, -7.75f};
    value.selection.targets = {{{8, 9, 10}, {1, 2, 3}, 0.25f, 1.5f, true},
                               {{11, 12, 13}, {-1, -2, -3}, 1.75f, 3.5f, false}};
    value.selection.contact =
        MeleeSelection::Contact{{{-4, -5, -6}, {2.25f, 3.5f, 4.75f}, -0.25f, -0.75f, true}, true};
    return value;
}
PlayerMeleeState movingState() {
    auto value = state();
    PlayerMeleeState::Moving moving;
    moving.origin = value.selection.feet;
    moving.radius = PlayerMeleeState::kMaxRadius;
    moving.ticks = PlayerMeleeState::kMaxTicks;
    MeleeGeometry::Target body;
    body.id = 1007;
    body.instance = 0xFEDCBA9876543210ULL;
    body.base = moving.origin + Vec3{2, 0, 3};
    body.radius = 1.25f;
    body.height = 5.75f;
    body.acquisition = {body.base + Vec3{0, 2.75f, 0}, 1.5f, 0.9f, 9.5f, false};
    body.contactCell = 4095;
    body.contact = true;
    body.requireFacing = true;
    body.low = true;
    body.blocksMovement = false;
    moving.targets = {body};
    Obstacle obstacle;
    obstacle.centre = moving.origin + Vec3{4, 0, -2};
    obstacle.yaw = -1.25f;
    obstacle.halfAcross = 2.5f;
    obstacle.halfAlong = 3.75f;
    obstacle.height = 6.25f;
    obstacle.solid = false;
    obstacle.cylinderRadius = 1.75f;
    obstacle.enemyItem = Obstacle::ItemQuery{true, 2.25f};
    moving.obstacles = {obstacle};
    value.moving = std::move(moving);
    return value;
}
std::vector<u8> encode(const PlayerMeleeState& value) {
    const auto bytes = PlayerMeleeCheckpoint::encode(value);
    REQUIRE(bytes);
    return *bytes;
}
void word(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (8 * i));
    }
}
CombatSnapshot snapshot() {
    CombatSnapshot result;
    result.motion.epoch = 1;
    result.motion.tick = 20;
    result.motion.cameraContinuity = 1;
    result.motion.players[1] = SeatMotion{1, 1, state().selection.feet, 0};
    PlayerCombatState player;
    player.health = 100;
    player.movement.enabled = true;
    player.rollback = PlayerAnimator::Checkpoint{};
    player.melee = state();
    result.players[1] = player;
    return result;
}

TEST_CASE("melee checkpoint retains ordered targets, contact priority and assisted facing",
          "[netplay][rollback][melee-checkpoint]") {
    auto value = state();
    for (const usize count : {usize{0}, usize{2}, PlayerMeleeState::kMaxTargets}) {
        value.selection.targets.resize(count);
        const auto bytes = encode(value);
        CHECK(bytes.size() ==
              PlayerMeleeCheckpoint::kHeaderBytes + count * PlayerMeleeCheckpoint::kTargetBytes);
        const auto decoded = PlayerMeleeCheckpoint::decode(bytes);
        REQUIRE(decoded);
        CHECK(encode(*decoded) == bytes);
        CHECK(decoded->selection.feet == value.selection.feet);
        CHECK(decoded->captureYaw == value.captureYaw);
        CHECK(decoded->assistedAim == value.assistedAim);
        REQUIRE(decoded->selection.contact);
        CHECK(decoded->selection.contact->requireFacing);
        CHECK(decoded->selection.contact->target.distance == -0.25f);
        if (count >= 2) {
            CHECK(decoded->selection.targets[0].base == value.selection.targets[0].base);
            CHECK(decoded->selection.targets[1].base == value.selection.targets[1].base);
        }
    }
    value.selection.targets.clear();
    value.selection.contact.reset();
    value.assistedAim.reset();
    const auto decoded = PlayerMeleeCheckpoint::decode(encode(value));
    REQUIRE(decoded);
    CHECK_FALSE(decoded->selection.contact);
    CHECK_FALSE(decoded->assistedAim);
    value.selection.targets.resize(PlayerMeleeState::kMaxTargets + 1);
    CHECK_FALSE(PlayerMeleeCheckpoint::encode(value));
}

TEST_CASE("melee checkpoint rejects malformed dimensions, flags, absent fields and truncation",
          "[netplay][rollback][melee-checkpoint]") {
    const auto bytes = encode(state());
    for (usize size = 0; size < bytes.size(); ++size) {
        CHECK_FALSE(PlayerMeleeCheckpoint::decode(std::span{bytes}.first(size)));
    }
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK_FALSE(PlayerMeleeCheckpoint::decode(trailing));
    for (const usize offset :
         {usize{0}, usize{12}, usize{16}, usize{20}, usize{24}, usize{36}, usize{48}, usize{60},
          usize{72}, usize{76}, usize{88}, usize{100}, usize{112}, usize{116}}) {
        CAPTURE(offset);
        auto bad = bytes;
        word(bad, offset, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()));
        CHECK_FALSE(PlayerMeleeCheckpoint::decode(bad));
        word(bad, offset, std::bit_cast<u32>(std::numeric_limits<f32>::infinity()));
        CHECK_FALSE(PlayerMeleeCheckpoint::decode(bad));
    }
    for (const auto [offset, value] :
         {std::pair<usize, u32>{28, 4}, {32, 65}, {80, 2}, {84, 2}, {120, 2}, {28, 0}}) {
        auto bad = bytes;
        word(bad, offset, value);
        CHECK_FALSE(PlayerMeleeCheckpoint::decode(bad));
    }
    auto empty = state();
    empty.selection.contact.reset();
    empty.assistedAim.reset();
    for (const usize offset : {usize{36}, usize{48}, usize{84}}) {
        auto bad = encode(empty);
        word(bad, offset, 1);
        CHECK_FALSE(PlayerMeleeCheckpoint::decode(bad));
    }
}

TEST_CASE("combat packets bind melee contexts to present seats and their checkpoint feet",
          "[netplay][rollback][melee-checkpoint]") {
    const auto input = snapshot();
    const auto bytes = CombatPacket::encode(input);
    REQUIRE(bytes);
    const auto decoded = CombatPacket::decode(*bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->players[1]->melee);
    CHECK(encode(*decoded->players[1]->melee) == encode(*input.players[1]->melee));
    CHECK(CombatPacket::encode(*decoded) == bytes);
    const usize start = bytes->size() - 8 - encode(state()).size();
    for (const auto [offset, value] : {std::pair<usize, u32>{start, 0},
                                       {start, 1},
                                       {start, 16},
                                       {start + 4, 0},
                                       {start + 4, std::numeric_limits<u32>::max()},
                                       {48, 3}}) {
        auto bad = *bytes;
        word(bad, offset, value);
        CHECK_FALSE(CombatPacket::decode(bad));
    }
    auto misplaced = input;
    misplaced.players[1]->melee->selection.feet.x += 1;
    CHECK_FALSE(CombatPacket::encode(misplaced));
    auto badPosition = *bytes;
    word(badPosition, start + 8, std::bit_cast<u32>(100.0f));
    CHECK_FALSE(CombatPacket::decode(badPosition));
    auto disabled = input;
    disabled.players[1]->movement.enabled = false;
    CHECK_FALSE(disabled.valid());
    disabled = input;
    disabled.players[1]->rollback.reset();
    CHECK_FALSE(disabled.valid());
}

TEST_CASE("melee contexts preserve every seat mask and the maximum candidate roster",
          "[netplay][rollback][melee-checkpoint]") {
    const auto seed = snapshot();
    for (u32 mask = 0; mask < 16; ++mask) {
        for (const usize count : {usize{0}, PlayerMeleeState::kMaxTargets}) {
            CAPTURE(mask, count);
            auto input = seed;
            for (usize seat = 0; seat < input.players.size(); ++seat) {
                input.players[seat].reset();
                input.motion.players[seat].reset();
                if ((mask & (1U << seat)) != 0) {
                    auto& player = input.players[seat];
                    player = seed.players[1];
                    player->melee->selection.targets.resize(count);
                    player->melee->selection.feet.x += static_cast<f32>(seat);
                    input.motion.players[seat] =
                        SeatMotion{static_cast<u32>(seat + 1), 1, player->melee->selection.feet, 0};
                }
            }
            const auto bytes = CombatPacket::encode(input);
            REQUIRE(bytes);
            CHECK(bytes->size() <= CombatPacket::kMaxBytes);
            const auto decoded = CombatPacket::decode(*bytes);
            REQUIRE(decoded);
            CHECK(CombatPacket::encode(*decoded) == bytes);
            for (usize seat = 0; seat < input.players.size(); ++seat) {
                CHECK(decoded->players[seat].has_value() == ((mask & (1U << seat)) != 0));
                if (decoded->players[seat]) {
                    REQUIRE(decoded->players[seat]->melee);
                    CHECK(decoded->players[seat]->melee->selection.targets.size() == count);
                    CHECK(decoded->players[seat]->melee->selection.feet ==
                          input.motion.players[seat]->position);
                }
            }
        }
    }
}

TEST_CASE("moving melee checkpoint retains raw lifetime acquisition contact and blocker data",
          "[netplay][rollback][melee-checkpoint]") {
    const auto input = movingState();
    const auto bytes = encode(input);
    const auto decoded = PlayerMeleeCheckpoint::decode(bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->moving);
    CHECK(encode(*decoded) == bytes);
    const auto& moving = *decoded->moving;
    CHECK(moving.origin == input.selection.feet);
    CHECK(moving.radius == PlayerMeleeState::kMaxRadius);
    CHECK(moving.ticks == PlayerMeleeState::kMaxTicks);
    REQUIRE(moving.targets.size() == 1);
    CHECK(moving.targets[0].id == 1007);
    CHECK(moving.targets[0].instance == 0xFEDCBA9876543210ULL);
    CHECK(moving.targets[0].contactCell == 4095);
    CHECK(moving.targets[0].contact);
    CHECK(moving.targets[0].requireFacing);
    CHECK(moving.targets[0].low);
    CHECK_FALSE(moving.targets[0].blocksMovement);
    CHECK_FALSE(moving.targets[0].acquisition.enabled);
    REQUIRE(moving.obstacles.size() == 1);
    CHECK_FALSE(moving.obstacles[0].solid);
    REQUIRE(moving.obstacles[0].enemyItem);
    CHECK(moving.obstacles[0].enemyItem->cylindrical);
    CHECK(moving.obstacles[0].enemyItem->radius == 2.25f);
    auto cleared = input;
    cleared.moving->targets[0].acquisition.enabled = true;
    cleared.moving->targets[0].contact = false;
    cleared.moving->targets[0].contactCell = 0;
    cleared.moving->targets[0].requireFacing = false;
    cleared.moving->targets[0].low = false;
    cleared.moving->targets[0].blocksMovement = true;
    cleared.moving->targets[0].instance = 0;
    cleared.moving->obstacles[0].solid = true;
    cleared.moving->obstacles[0].enemyItem.reset();
    CHECK(PlayerMeleeCheckpoint::decode(encode(cleared)));
}

TEST_CASE("moving melee checkpoint rejects corrupt counts flags dimensions and identity aliases",
          "[netplay][rollback][melee-checkpoint]") {
    const auto input = movingState();
    const auto bytes = encode(input);
    const usize start = PlayerMeleeCheckpoint::kHeaderBytes +
                        input.selection.targets.size() * PlayerMeleeCheckpoint::kTargetBytes;
    const usize body = start + PlayerMeleeCheckpoint::kMovingHeaderBytes;
    const usize box = body + PlayerMeleeCheckpoint::kGeometryBytes;
    for (usize size = 0; size < bytes.size(); ++size) {
        CHECK_FALSE(PlayerMeleeCheckpoint::decode(std::span{bytes}.first(size)));
    }
    auto extra = bytes;
    extra.push_back(0);
    CHECK_FALSE(PlayerMeleeCheckpoint::decode(extra));
    for (const auto [offset, value] : {std::pair<usize, u32>{28, 8},
                                       {start + 16, 0},
                                       {start + 16, PlayerMeleeState::kMaxTicks + 1},
                                       {start + 20, 65},
                                       {start + 24, 65},
                                       {start + 20, 0xFFFFFFFFU},
                                       {start + 24, 0xFFFFFFFFU},
                                       {body, 0xFFFFFFFFU},
                                       {body, 65536},
                                       {body + 56, 32},
                                       {body + 60, 4096},
                                       {body + 60, 0xFFFFFFFFU},
                                       {body + 56, 4},
                                       {box + 32, 8},
                                       {box + 32, 4},
                                       {box + 32, 1}}) {
        CAPTURE(offset, value);
        auto bad = bytes;
        word(bad, offset, value);
        CHECK_FALSE(PlayerMeleeCheckpoint::decode(bad));
    }
    for (const usize offset :
         {start,     start + 4, start + 8, start + 12, body + 12, body + 16, body + 20, body + 24,
          body + 28, body + 32, body + 36, body + 40,  body + 44, body + 48, body + 52, box,
          box + 4,   box + 8,   box + 12,  box + 16,   box + 20,  box + 24,  box + 28,  box + 36}) {
        for (const f32 value :
             {std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity(),
              -std::numeric_limits<f32>::infinity()}) {
            CAPTURE(offset, value);
            auto bad = bytes;
            word(bad, offset, std::bit_cast<u32>(value));
            CHECK_FALSE(PlayerMeleeCheckpoint::decode(bad));
        }
    }
    for (const usize offset : {start + 12, body + 24, body + 28, body + 48}) {
        auto bad = bytes;
        word(bad, offset, 0);
        CHECK_FALSE(PlayerMeleeCheckpoint::decode(bad));
    }
    auto bad = input;
    bad.moving->origin.x += 1;
    CHECK_FALSE(bad.valid());
    bad = input;
    bad.moving->targets[0].instance = 0;
    CHECK_FALSE(bad.valid());
    bad = input;
    bad.moving->targets.push_back(bad.moving->targets[0]);
    CHECK_FALSE(bad.valid());
    bad.moving->targets[1].id += 1;
    CHECK_FALSE(bad.valid());
    bad.moving->targets[1].instance += 1;
    CHECK(bad.valid());
    bad.moving->targets[1].acquisition.point.x += 257;
    CHECK_FALSE(bad.valid());
}

TEST_CASE("moving melee payload has exact bounded sizes with empty and full rosters",
          "[netplay][rollback][melee-checkpoint]") {
    auto input = movingState();
    input.selection.targets.resize(PlayerMeleeState::kMaxTargets);
    input.moving->targets.resize(PlayerMeleeState::kMaxTargets, input.moving->targets.front());
    input.moving->obstacles.resize(PlayerMeleeState::kMaxObstacles,
                                   input.moving->obstacles.front());
    for (usize index = 0; index < input.moving->targets.size(); ++index) {
        input.moving->targets[index].id = static_cast<s32>(1000 + index);
        input.moving->targets[index].instance = index + 1;
    }
    auto bytes = encode(input);
    CHECK(bytes.size() == PlayerMeleeCheckpoint::kMaxBytes);
    CHECK(PlayerMeleeCheckpoint::decode(bytes));
    input.moving->targets.push_back(input.moving->targets.front());
    CHECK_FALSE(input.valid());
    input.moving->targets.pop_back();
    input.moving->obstacles.push_back(input.moving->obstacles.front());
    CHECK_FALSE(input.valid());
    input.moving->targets.clear();
    input.moving->obstacles.clear();
    input.selection.targets.clear();
    bytes = encode(input);
    CHECK(bytes.size() ==
          PlayerMeleeCheckpoint::kHeaderBytes + PlayerMeleeCheckpoint::kMovingHeaderBytes);
    CHECK(PlayerMeleeCheckpoint::decode(bytes));
}
} // namespace
