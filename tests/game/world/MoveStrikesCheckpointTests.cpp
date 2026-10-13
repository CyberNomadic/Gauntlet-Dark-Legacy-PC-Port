#include <limits>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/world/MoveStrikes.h"

namespace {
using namespace gdl;
using namespace gdl::game;
static_assert(!std::is_copy_assignable_v<MoveStrikes>);

TEST_CASE("strike checkpoint resumes area windows delayed launch reflection and exact outputs",
          "[move-strikes-checkpoint]") {
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30, 0.25f);
    MoveStrikes native;
    MoveStrikes replay;
    MoveStrike burst;
    burst.type = MoveStrike::kBursts;
    burst.delay = 0.25f;
    burst.radius = 12;
    burst.amount = 50;
    const auto area = native.start(burst, 0, {2, 3, 4}, {0, 0, 1}, 10, 2);
    MoveStrike flight;
    flight.type = MoveStrike::kFlies;
    flight.flags = 0x840;
    flight.loopEffect = 1;
    flight.hitRadius = 1;
    flight.speed = 20;
    flight.maxTime = 6;
    flight.amount = 70;
    const auto shot = native.start(flight, 3, {0, 5, 0}, {0, 0, 1}, 10, 1);
    native.update(0.17f, nullptr);
    const auto saved = native.checkpoint();
    REQUIRE(saved);
    REQUIRE(saved->strikes.size() == 2);
    REQUIRE(saved->strikes[1].launchIn > 0);
    REQUIRE(replay.restore(*saved));
    for (s32 tick = 0; tick < 110; ++tick) {
        CAPTURE(tick, seconds);
        if (tick == 3) {
            const auto placement = glm::translate(Mat4{1}, Vec3{7, 8, 9});
            native.placeArea(area, placement);
            replay.placeArea(area, placement);
        }
        if (tick == 8) {
            native.hitPlayer(shot, true, {1, 2, 3});
            replay.hitPlayer(shot, true, {1, 2, 3});
        }
        const auto expected = native.update(seconds, nullptr);
        const auto actual = replay.update(seconds, nullptr);
        CHECK(actual == expected);
        const auto a = native.checkpoint();
        const auto b = replay.checkpoint();
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a == b);
        if (tick == 20) {
            REQUIRE(native.restore(*saved));
            REQUIRE(replay.restore(*saved));
        }
    }
    CHECK(native.start(burst, 2, {}, {1, 0, 0}, 10, 1) ==
          replay.start(burst, 2, {}, {1, 0, 0}, 10, 1));
}

TEST_CASE("strike restoration validates before mutation and consumes only its owner's preparation",
          "[move-strikes-checkpoint]") {
    MoveStrikes native;
    MoveStrikes other;
    MoveStrike row;
    row.type = MoveStrike::kFlies;
    row.maxTime = 2;
    row.hitRadius = 1;
    row.amount = 10;
    native.start(row, 0, {}, {0, 0, 1}, 1);
    const auto saved = native.checkpoint();
    REQUIRE(saved);
    for (s32 mutation = 0; mutation < 8; ++mutation) {
        auto bad = *saved;
        switch (mutation) {
        case 0: bad.strikes.push_back(bad.strikes[0]); break;
        case 1: bad.strikes[0].owner = 4; break;
        case 2: bad.strikes[0].launchIn = -1; break;
        case 3: bad.strikes[0].radius = std::numeric_limits<f32>::quiet_NaN(); break;
        case 4: bad.strikes[0].facing.y = 0.5f; break;
        case 5: bad.next = bad.strikes[0].id; break;
        case 6: bad.strikes[0].expanding = true; break;
        case 7: bad.strikes.resize(MoveStrikes::Checkpoint::kMaxStrikes + 1); break;
        }
        CHECK_FALSE(native.restore(bad));
        CHECK(native.checkpoint() == saved);
    }
    auto prepared = native.prepareRestore(*saved);
    auto sibling = native.prepareRestore(*saved);
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK_FALSE(other.commitRestore(std::move(*prepared)));
    REQUIRE(native.commitRestore(std::move(*prepared)));
    CHECK_FALSE(native.canCommit(*prepared));
    CHECK_FALSE(native.commitRestore(std::move(*sibling)));
    prepared = native.prepareRestore(*saved);
    REQUIRE(prepared);
    native.clear();
    CHECK_FALSE(native.commitRestore(std::move(*prepared)));
    REQUIRE(native.restore(*saved));
}
} // namespace
