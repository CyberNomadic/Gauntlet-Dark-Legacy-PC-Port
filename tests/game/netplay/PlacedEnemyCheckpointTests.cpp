#include <array>
#include <bit>
#include <limits>
#include <numeric>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/netplay/PlacedEnemyCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void word(std::vector<u8>& bytes, usize at, u32 value) {
    REQUIRE(at + 4 <= bytes.size());
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes[at + shift / 8] = static_cast<u8>(value >> shift);
    }
}

PlacedEnemiesCheckpoint::State sample() {
    PlacedEnemies::Placement placement;
    auto& spawn = placement.spawn;
    spawn.zigZagSide = -1;
    spawn.kind = kGruntKind;
    spawn.tier = 4;
    spawn.algorithm = -32768;
    spawn.position = {2, -3, 5};
    spawn.direction = {0.25f, 0, 0.75f};
    spawn.clearance = 1.5f;
    spawn.generator = 2;
    spawn.placed = spawn.patrolBirth = spawn.allBirthDirections = spawn.asleep = true;
    spawn.throwInterval = 0.75f;
    spawn.priority = EnemySpawn::Priority::Visible;
    spawn.sight = 17.25f;
    placement.kind = kGruntKind;
    placement.facing = -0.625f;
    placement.viewRadius = 2.5f;
    placement.sight = -7;
    placement.carried = 37;
    PlacedEnemiesCheckpoint::State state;
    state.roster = {placement, placement, placement};
    state.roster[1].spawn.position.x = 23;
    state.pending = {0, 2};
    state.view.emplace();
    state.view->position = {13, 17, -19};
    state.view->right = {0.8f, 0, -0.6f};
    state.view->up = {0, 1, 0};
    state.view->forward = {0.6f, 0, 0.8f};
    state.view->sinX = 0.6f;
    state.view->cosX = 0.8f;
    state.view->sinY = 0.8f;
    state.view->cosY = 0.6f;
    state.attention = {-7, 8, 9};
    REQUIRE(state.valid());
    return state;
}

TEST_CASE("placed enemy wire preserves every field and ordered duplicate records",
          "[netplay][rollback][placed-enemy-checkpoint]") {
    auto state = sample();
    const bool watched = GENERATE(false, true);
    const bool carried = GENERATE(false, true);
    if (!watched) {
        state.view.reset();
    }
    if (!carried) {
        for (auto& entry : state.roster) {
            entry.carried.reset();
        }
    }
    const auto encoded = PlacedEnemyCheckpoint::encode(state);
    REQUIRE(encoded);
    CHECK(encoded->size() == PlacedEnemyCheckpoint::kHeaderBytes +
                                 (watched ? PlacedEnemyCheckpoint::kViewBytes : 0) +
                                 state.roster.size() * PlacedEnemyCheckpoint::kPlacementBytes +
                                 state.pending.size() * 4);
    CHECK((*encoded)[4] == 1);
    CHECK((*encoded)[5] == 0);
    const auto decoded = PlacedEnemyCheckpoint::decode(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    PlacedEnemies original;
    PlacedEnemies restored;
    original.bind(state.roster);
    restored.bind(state.roster);
    REQUIRE(PlacedEnemiesCheckpoint::restore(original, state));
    REQUIRE(PlacedEnemiesCheckpoint::restore(restored, *decoded));
    std::vector<PlacedEnemies::Placement> a;
    std::vector<PlacedEnemies::Placement> b;
    original.activate([&](const auto& entry) { a.push_back(entry); });
    restored.activate([&](const auto& entry) { b.push_back(entry); });
    CHECK(a == b);
    CHECK(PlacedEnemiesCheckpoint::capture(original) == PlacedEnemiesCheckpoint::capture(restored));
}

TEST_CASE("placed enemy wire refuses malformed fields counts and noncanonical optionals",
          "[netplay][rollback][placed-enemy-checkpoint]") {
    auto state = sample();
    state.view.reset();
    state.roster.front().carried.reset();
    auto encoded = PlacedEnemyCheckpoint::encode(state);
    REQUIRE(encoded);
    auto& bytes = *encoded;
    constexpr usize row = PlacedEnemyCheckpoint::kHeaderBytes;
    constexpr usize pending = row + 3 * PlacedEnemyCheckpoint::kPlacementBytes;
    SECTION("magic") {
        word(bytes, 0, 0);
    }
    SECTION("version") {
        word(bytes, 4, 2);
    }
    SECTION("unknown header flag") {
        word(bytes, 16, 2);
    }
    SECTION("oversized count") {
        word(bytes, 8, 513);
    }
    SECTION("pending exceeds roster") {
        word(bytes, 12, 4);
    }
    SECTION("nonfinite attention") {
        word(bytes, 20, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()));
    }
    SECTION("unknown spawn flag") {
        word(bytes, row + 48, 32);
    }
    SECTION("absent carried value") {
        word(bytes, row + 80, 1);
    }
    SECTION("invalid priority") {
        word(bytes, row + 56, 2);
    }
    SECTION("invalid algorithm") {
        word(bytes, row + 12, 32768);
    }
    SECTION("negative timer") {
        word(bytes, row + 52, std::bit_cast<u32>(-1.0f));
    }
    SECTION("invalid family") {
        word(bytes, row + 64, kEnemyKindCount);
    }
    SECTION("out-of-roster pending index") {
        word(bytes, pending, 3);
    }
    SECTION("duplicate pending index") {
        word(bytes, pending + 4, 0);
    }
    SECTION("reordered pending indices") {
        word(bytes, pending, 2);
        word(bytes, pending + 4, 0);
    }
    SECTION("trailing byte") {
        bytes.push_back(0);
    }
    CHECK_FALSE(PlacedEnemyCheckpoint::decode(bytes));
}

TEST_CASE("placed enemy wire checks every truncation and exact allocation limit",
          "[netplay][rollback][placed-enemy-checkpoint]") {
    auto state = sample();
    const auto encoded = PlacedEnemyCheckpoint::encode(state);
    REQUIRE(encoded);
    for (usize size = 0; size < encoded->size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(PlacedEnemyCheckpoint::decode(std::span{*encoded}.first(size)));
    }
    state.roster.resize(PlacedEnemyCheckpoint::kMaxPlacements, state.roster.front());
    state.pending.resize(state.roster.size());
    std::iota(state.pending.begin(), state.pending.end(), usize{0});
    const auto full = PlacedEnemyCheckpoint::encode(state);
    REQUIRE(full);
    CHECK(full->size() == PlacedEnemyCheckpoint::kMaxBytes);
    CHECK(PlacedEnemyCheckpoint::decode(*full) == state);
    state.roster.push_back(state.roster.front());
    REQUIRE(state.valid());
    CHECK_FALSE(PlacedEnemyCheckpoint::encode(state));
    auto invalid = *full;
    invalid.push_back(0);
    CHECK_FALSE(PlacedEnemyCheckpoint::decode(invalid));
}

} // namespace
