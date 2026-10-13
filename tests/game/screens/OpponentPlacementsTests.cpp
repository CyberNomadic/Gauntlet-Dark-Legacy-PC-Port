#include <array>
#include <bit>
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/screens/OpponentPlacements.h"
#include "game/world/ItemFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void parameter(ItemInstance& instance, usize offset, s16 value) {
    const u16 bits = std::bit_cast<u16>(value);
    instance.params[offset] = static_cast<u8>(bits);
    instance.params[offset + 1] = static_cast<u8>(bits >> 8);
}
void sight(ItemInstance& instance, f32 value) {
    const auto bits = std::bit_cast<u32>(value);
    for (u32 shift = 0; shift < 32; shift += 8) {
        instance.params[4 + shift / 8] = static_cast<u8>(bits >> shift);
    }
}

TEST_CASE("ordinary placement resolution keeps native tuning fields and load order",
          "[rollback][opponent-placements]") {
    const s16 strength = GENERATE(0, 1, 3, 4);
    const s16 interval = GENERATE(-1, 0, 1, 7);
    ItemInfo info;
    info.type = ItemInfo::kPlacedEnemy;
    info.name = "GRU";
    info.radius = 2;
    info.height = 3;
    ItemInstance instance;
    instance.position = {2, 3, 5};
    instance.rotation = {0.3f, -0.4f, 0.2f};
    parameter(instance, 0, strength);
    parameter(instance, 2, -123);
    parameter(instance, 8, interval);
    sight(instance, 13.75f);
    const std::array roster{LevelEnemy{6, kMediumClass, {}}, LevelEnemy{9, kMediumOtherClass, {}}};
    std::vector<s32> loads;
    const auto result = OpponentPlacements::ordinary(instance, info, roster, 1, [&](s32 kind) {
        loads.push_back(kind);
        return true;
    });
    REQUIRE(result);
    const s32 kind = strength >= 4 ? 9 : 6;
    CHECK(loads == std::vector{kind});
    CHECK(result->kind == kind);
    CHECK(result->spawn.kind == kind);
    CHECK(result->spawn.tier == (strength > 0 ? strength : 1));
    CHECK(result->spawn.algorithm == -123);
    CHECK(result->spawn.position == instance.position);
    CHECK(result->spawn.sight == 13.75f);
    CHECK(result->sight == 13.75f);
    CHECK(result->viewRadius == 6);
    CHECK(result->spawn.placed);
    CHECK(result->spawn.asleep == (strength == 0));
    CHECK(result->spawn.priority == EnemySpawn::Priority::Visible);
    CHECK_FALSE(result->carried);
    const Mat4 stood = itemPlacement(instance.position, instance.rotation);
    CHECK(result->spawn.direction == Vec3{stood[2][0], 0, stood[2][2]});
    CHECK(result->facing == std::atan2(stood[2][0], stood[2][2]));
    const f32 expectedInterval = interval <= 0   ? EnemySpawn{}.throwInterval
                                 : interval == 1 ? 0
                                                 : 0.7f;
    CHECK(result->spawn.throwInterval == Catch::Approx(expectedInterval));
}

TEST_CASE("ordinary placement filters never invent a fallback resource or a great creature",
          "[rollback][opponent-placements]") {
    ItemInfo info;
    info.type = ItemInfo::kPlacedEnemy;
    info.name = "GRU";
    ItemInstance instance;
    s32 players = 1;
    bool allowsResource = true;
    usize expectedLoads = 0;
    SECTION("not a placed enemy") {
        info.type = ItemInfo::kPowerup;
    }
    SECTION("party minimum") {
        instance.minPlayers = 2;
    }
    SECTION("exact three-player exclusion") {
        instance.minPlayers = 13;
        players = 1;
    }
    SECTION("unknown native name") {
        info.name = "NOT_AN_ENEMY";
    }
    SECTION("golem belongs to the separate statue loader") {
        info.name = enemyKind(kGolemEnemyKind).name;
    }
    SECTION("gargoyle belongs to the separate statue loader") {
        info.name = enemyKind(kGargoyleEnemyKind).name;
    }
    SECTION("general belongs to the carried-item loader") {
        info.name = enemyKind(kGeneralEnemyKind).name;
    }
    SECTION("missing resource stays missing") {
        allowsResource = false;
        expectedLoads = 1;
    }
    usize loads = 0;
    CHECK_FALSE(OpponentPlacements::ordinary(instance, info, {}, players, [&](s32) {
        ++loads;
        return allowsResource;
    }));
    CHECK(loads == expectedLoads);
}

} // namespace
