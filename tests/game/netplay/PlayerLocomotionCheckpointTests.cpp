#include <bit>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/netplay/PlayerLocomotionCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

PlayerLocomotionState contacts() {
    PlayerLocomotionState state;
    state.origin = {2, 3, 4};
    state.radius = 3;
    state.ticks = 12;
    state.bodies = {{{4, 3, 5}, 1.5f, 6}, {{5, 3, 7}, 2, 4}};
    state.party = {{{3, 3, 3}, 1, 5}};
    state.generators = {{{6, 3, 6}, 0.5f, 1, 2, 4, true, 0, Obstacle::ItemQuery{false, 2}}};
    state.fixtures = {{{2, 3, 8}, -0.5f, 2, 3, 5, true, 2, Obstacle::ItemQuery{true, 3}}};
    return state;
}
void word(std::vector<u8>& bytes, usize offset, u32 value) {
    REQUIRE(offset + 4 <= bytes.size());
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes[offset + shift / 8] = static_cast<u8>(value >> shift);
    }
}

TEST_CASE("locomotion contacts preserve native ordering and independent collision phases",
          "[netplay][locomotion-checkpoint]") {
    const bool automatic = GENERATE(false, true);
    auto state = contacts();
    state.automatic = automatic;
    if (automatic) {
        auto& melee = state.automaticGeometry.emplace();
        melee.selection.feet = state.origin;
        melee.selection.reach = 3;
        melee.captureYaw = 0.5f;
        melee.moving = PlayerMeleeState::Moving{state.origin, state.radius, state.ticks, {}, {}};
    }
    const auto bytes = PlayerLocomotionCheckpoint::encode(state);
    REQUIRE(bytes);
    const auto decoded = PlayerLocomotionCheckpoint::decode(*bytes);
    REQUIRE(decoded);
    CHECK(decoded->origin == state.origin);
    CHECK(decoded->radius == state.radius);
    CHECK(decoded->ticks == state.ticks);
    CHECK(decoded->bodies == state.bodies);
    CHECK(decoded->party == state.party);
    REQUIRE(decoded->generators.size() == 1);
    REQUIRE(decoded->fixtures.size() == 1);
    CHECK(decoded->generators[0].centre == state.generators[0].centre);
    CHECK(decoded->fixtures[0].centre == state.fixtures[0].centre);
    CHECK(PlayerLocomotionCheckpoint::encode(*decoded) == bytes);
    auto reordered = state;
    std::swap(reordered.bodies[0], reordered.bodies[1]);
    CHECK(PlayerLocomotionCheckpoint::encode(reordered) != bytes);
}

TEST_CASE("locomotion contacts reject malformed lengths counts flags and numerical state",
          "[netplay][locomotion-checkpoint]") {
    const auto original = PlayerLocomotionCheckpoint::encode(contacts());
    REQUIRE(original);
    auto bytes = *original;
    SECTION("every truncated packet") {
        for (usize length = 0; length < bytes.size(); ++length) {
            CAPTURE(length);
            CHECK_FALSE(PlayerLocomotionCheckpoint::decode(std::span(bytes).first(length)));
        }
        return;
    }
    SECTION("trailing byte") {
        bytes.push_back(0);
    }
    SECTION("unknown header flag") {
        word(bytes, 36, 2);
    }
    SECTION("overflowing bounded counts") {
        const usize offset = GENERATE(usize{20}, usize{24}, usize{28}, usize{32}, usize{40});
        word(bytes, offset, std::numeric_limits<u32>::max());
    }
    SECTION("zero or excessive prefix") {
        word(bytes, 16, GENERATE(0U, PlayerLocomotionState::kMaxTicks + 1));
    }
    SECTION("nonfinite geometry") {
        const usize offset = GENERATE(usize{0}, usize{12}, usize{44}, usize{56});
        word(bytes, offset, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()));
    }
    SECTION("unknown obstacle flags") {
        word(bytes,
             PlayerLocomotionCheckpoint::kHeaderBytes + 2 * PlayerLocomotionCheckpoint::kBodyBytes +
                 32,
             8);
    }
    SECTION("absent obstacle metadata must be canonical") {
        word(bytes,
             PlayerLocomotionCheckpoint::kHeaderBytes + 2 * PlayerLocomotionCheckpoint::kBodyBytes +
                 32,
             1);
    }
    CHECK_FALSE(PlayerLocomotionCheckpoint::decode(bytes));
    CHECK(PlayerLocomotionCheckpoint::decode(*original));
}

TEST_CASE("locomotion contact capacity is complete or rejected never truncated",
          "[netplay][locomotion-checkpoint]") {
    auto state = contacts();
    state.bodies.resize(PlayerLocomotionState::kMaxBodies, state.bodies[0]);
    state.generators.resize(PlayerLocomotionState::kMaxGenerators, state.generators[0]);
    state.fixtures.resize(PlayerLocomotionState::kMaxFixtures, state.fixtures[0]);
    state.party.resize(PlayerLocomotionState::kMaxParty, state.party[0]);
    const auto bytes = PlayerLocomotionCheckpoint::encode(state);
    REQUIRE(bytes);
    CHECK(PlayerLocomotionCheckpoint::decode(*bytes));
    SECTION("body overflow") {
        state.bodies.push_back(state.bodies[0]);
    }
    SECTION("generator overflow") {
        state.generators.push_back(state.generators[0]);
    }
    SECTION("fixture overflow") {
        state.fixtures.push_back(state.fixtures[0]);
    }
    SECTION("party overflow") {
        state.party.push_back(state.party[0]);
    }
    CHECK_FALSE(PlayerLocomotionCheckpoint::encode(state));
}

TEST_CASE("automatic melee unknown context differs from certified empty acquisition",
          "[netplay][locomotion-checkpoint]") {
    auto state = contacts();
    state.automatic = true;
    const auto unknown = PlayerLocomotionCheckpoint::encode(state);
    REQUIRE(unknown);
    REQUIRE(PlayerLocomotionCheckpoint::decode(*unknown));
    CHECK_FALSE(PlayerLocomotionCheckpoint::decode(*unknown)->automaticGeometry);
    auto& melee = state.automaticGeometry.emplace();
    melee.selection.feet = state.origin;
    melee.selection.reach = 3;
    melee.moving = PlayerMeleeState::Moving{state.origin, state.radius, state.ticks, {}, {}};
    REQUIRE(state.valid());
    const auto empty = PlayerLocomotionCheckpoint::encode(state);
    REQUIRE(empty);
    CHECK(empty != unknown);
    SECTION("disabled automation cannot carry live acquisition") {
        state.automatic = false;
    }
    SECTION("mismatched feet") {
        melee.selection.feet.x += 1;
    }
    SECTION("shorter geometry prefix") {
        melee.moving->ticks -= 1;
    }
    SECTION("smaller geometry radius") {
        melee.moving->radius -= 0.1f;
    }
    SECTION("stationary acquisition is insufficient") {
        melee.moving.reset();
    }
    CHECK_FALSE(state.valid());
    CHECK_FALSE(PlayerLocomotionCheckpoint::encode(state));
}
} // namespace
