#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/screens/FloorRiding.h"
#include "game/screens/OrdinaryFloorSupport.h"
#include "game/screens/PartyMotion.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Phase = OrdinaryFloorSupport::Phase;

constexpr s32 kDeck = 7;
constexpr s32 kOtherDeck = 8;
constexpr u32 kFloor = 4;

PartyMotion::Events floorEvents() {
    // Native PartyMotion always dispatches selection for standing input lanes,
    // even when these figure-free floor fixtures have no selector state.
    return {.perform = [](usize, PartyMotion::Action) { FAIL("unexpected player action"); },
            .select = [](usize, const SelectorInput&, s32) {},
            .advanceTurbo = [](usize, s32, f32) { FAIL("unexpected turbo timeline"); }};
}

void floor(std::vector<CollisionTriangle>& triangles, Vec2 low, Vec2 high, f32 height, s32 object,
           u32 flags) {
    triangles.push_back(
        {{0, 1, 0},
         {Vec3{low.x, height, low.y}, Vec3{high.x, height, high.y}, Vec3{high.x, height, low.y}},
         object,
         flags});
    triangles.push_back(
        {{0, 1, 0},
         {Vec3{low.x, height, low.y}, Vec3{low.x, height, high.y}, Vec3{high.x, height, high.y}},
         object,
         flags});
}

struct Fixture {
    WorldCollision collision;
    std::array<PlayerRuntime, 2> party;

    Fixture() {
        std::vector<CollisionTriangle> triangles;
        floor(triangles, {-5, -5}, {5, 5}, 0, kDeck, kFloor | FloorRiding::kMoving);
        floor(triangles, {10, -5}, {20, 5}, 0, kOtherDeck,
              kFloor | FloorRiding::kMoving | FloorRiding::kKeptApart);
        floor(triangles, {-20, -5}, {-5, 5}, 0, 3, kFloor);
        floor(triangles, {-30, -5}, {-20, 5}, 0, 4, kFloor);
        collision.build(triangles);
        collision.setMovingObjects(std::array<s32, 2>{kDeck, kOtherDeck});
        party[0].actor.spawn(0, {}, nullptr, {2, 0, 0}, 0);
        party[1].actor.spawn(1, {}, nullptr, {15, 0, 0}, 0);
        FloorRiding::land(party, 0, party[0].actor.position(), collision);
        FloorRiding::land(party, 1, party[1].actor.position(), collision);
    }

    bool accepted(usize player, Phase phase, bool moving = true) const {
        return OrdinaryFloorSupport::accepts(party[player], collision, phase, moving);
    }
};

TEST_CASE("ordinary floor admission observes native lift and turntable phases without mutation",
          "[game][screens][ordinary-floor-support][rollback]") {
    const s32 rate = GENERATE(30, 60);
    const bool rotate = GENERATE(false, true);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    Fixture fixture;
    std::span<PlayerRuntime> rider{fixture.party.data(), 1};
    const std::array<PlayInput, 1> input{};
    for (s32 tick = 0; tick < rate; ++tick) {
        const auto previous = fixture.party[0].floor.placement;
        REQUIRE(previous);
        const Vec3 oldPosition = fixture.party[0].actor.position();
        const f32 yaw = rotate ? static_cast<f32>(tick + 1) * seconds : 0;
        const Mat4 current =
            glm::rotate(glm::translate(Mat4{1}, Vec3{0, 1 + static_cast<f32>(tick) * seconds, 0}),
                        yaw, Vec3{0, 1, 0});
        fixture.collision.setObjectTransform(kDeck, current);
        const auto originalBody = fixture.party[0].actor.checkpoint();
        const auto originalFloor = fixture.party[0].floor;
        const auto save = fixture.party[0].actor.save().toJson();
        REQUIRE(fixture.accepted(0, Phase::BeforeCarry));
        CHECK_FALSE(fixture.accepted(0, Phase::AfterLanding));
        CHECK(fixture.party[0].actor.checkpoint() == originalBody);
        CHECK(fixture.party[0].floor.object == originalFloor.object);
        CHECK(fixture.party[0].floor.flags == originalFloor.flags);
        CHECK(fixture.party[0].floor.placement == originalFloor.placement);
        CHECK(fixture.party[0].actor.save().toJson() == save);
        CHECK(fixture.collision.objectTransform(kDeck) == std::optional<Mat4>{current});

        // This is the actual complete native player phase, not a duplicate
        // carry formula used by the admission implementation.
        PartyMotion::step(rider, input, false, 0, 60 / rate, seconds, fixture.collision,
                          floorEvents());
        const Vec3 expected = Vec3{current * glm::inverse(*previous) * Vec4{oldPosition, 1}};
        CHECK(glm::distance(fixture.party[0].actor.position(), expected) < 0.0001f);
        CHECK(fixture.party[0].actor.yaw() == Approx(yaw).margin(0.0001f));
        REQUIRE(fixture.accepted(0, Phase::AfterLanding));
        REQUIRE(fixture.accepted(0, Phase::BeforeCarry));
        CHECK_FALSE(fixture.accepted(0, Phase::BeforeCarry, false));

        // Native triggers/rotators run AFTER PartyMotion. A checkpoint here
        // must keep the old player support matrix, ready for next tick's carry.
        fixture.collision.setObjectTransform(kDeck,
                                             glm::translate(Mat4{1}, Vec3{0, 0.025f, 0}) * current);
        REQUIRE(fixture.accepted(0, Phase::BeforeCarry));
        CHECK_FALSE(fixture.accepted(0, Phase::AfterLanding));
    }
}

TEST_CASE("ordinary floor admission retains static initial landing and post-fixture floor lag",
          "[game][screens][ordinary-floor-support][rollback]") {
    Fixture fixture;
    auto& player = fixture.party[0];
    player.actor.place({-10, 0, 0});
    player.floor = {};
    CHECK(fixture.accepted(0, Phase::BeforeCarry, false));
    CHECK_FALSE(fixture.accepted(0, Phase::AfterLanding, false));
    FloorRiding::land(fixture.party, 0, player.actor.position(), fixture.collision);
    REQUIRE(player.floor.object == 3);
    CHECK(fixture.accepted(0, Phase::AfterLanding, false));
    CHECK(fixture.accepted(0, Phase::BeforeCarry, false));

    player.actor.place({-19, 0, 0});
    FloorRiding::land(fixture.party, 0, player.actor.position(), fixture.collision);
    REQUIRE(player.floor.object == 3);
    // Native fixture pushes occur after land(), without re-landing. A static
    // coplanar seam therefore legitimately retains the prior support identity.
    player.actor.slide({-2, 0, 0}, &fixture.collision);
    const auto newSupport = FloorRiding::support(player.actor, fixture.collision);
    REQUIRE(newSupport);
    REQUIRE(newSupport->object == 4);
    CHECK(player.floor.object == 3);
    CHECK(fixture.accepted(0, Phase::BeforeCarry));
    CHECK(fixture.accepted(0, Phase::AfterLanding));
    player.floor = {};
    player.actor.place({0, 0, 0});
    // A fresh static link cannot silently stand in for a moving-floor seed.
    CHECK_FALSE(fixture.accepted(0, Phase::BeforeCarry));
}

TEST_CASE("ordinary floor admission preserves native kept-apart boarding and exit locks",
          "[game][screens][ordinary-floor-support][rollback]") {
    Fixture fixture;
    auto& player = fixture.party[0];
    player.actor.place({-6, 0, 0});
    FloorRiding::land(fixture.party, 0, player.actor.position(), fixture.collision);
    REQUIRE(player.floor.object == 3);
    REQUIRE(fixture.accepted(1, Phase::BeforeCarry));
    REQUIRE(FloorRiding::keptApart(fixture.party, 0, kDeck));
    const Vec3 from = player.actor.position();
    player.actor.place({-4, 0, 0});
    FloorRiding::land(fixture.party, 0, from, fixture.collision);
    CHECK(player.actor.position() == from);
    CHECK(player.floor.object == 3);
    REQUIRE(fixture.accepted(0, Phase::AfterLanding));

    // Once the other member leaves its separate carrier, native landing
    // admits the boarder. The helper never runs a separate party policy.
    fixture.party[1].actor.place({-12, 0, 0});
    FloorRiding::land(fixture.party, 1, fixture.party[1].actor.position(), fixture.collision);
    player.actor.place({-4, 0, 0});
    FloorRiding::land(fixture.party, 0, from, fixture.collision);
    REQUIRE(player.floor.object == kDeck);
    REQUIRE(fixture.accepted(0, Phase::AfterLanding));
    fixture.collision.setFloorExitBlocked(kDeck, true);
    const Mat4 lifted = glm::translate(Mat4{1}, Vec3{0, 0.1f, 0});
    fixture.collision.setObjectTransform(kDeck, lifted);
    REQUIRE(fixture.accepted(0, Phase::BeforeCarry));
    FloorRiding::carry(player, fixture.collision);
    const Vec3 carried = player.actor.position();
    player.actor.place({-6, 0, 0});
    FloorRiding::land(fixture.party, 0, carried, fixture.collision);
    CHECK(player.actor.position() == carried);
    CHECK(player.floor.object == kDeck);
    REQUIRE(fixture.accepted(0, Phase::AfterLanding));
    fixture.collision.setFloorExitBlocked(kDeck, false);
    player.actor.place({-6, 0, 0});
    FloorRiding::land(fixture.party, 0, carried, fixture.collision);
    CHECK(player.floor.object == 3);
    REQUIRE(fixture.accepted(0, Phase::AfterLanding));
}

TEST_CASE("ordinary floor admission keeps all-party native shove and camera re-landing",
          "[game][screens][ordinary-floor-support][rollback]") {
    const s32 rate = GENERATE(30, 60);
    Fixture fixture;
    fixture.party[0].actor.place({0, 0, -0.75f});
    fixture.party[1].actor.place({0, 0, 0.75f});
    for (usize index = 0; index < fixture.party.size(); ++index) {
        fixture.party[index].floor = {};
        FloorRiding::land(fixture.party, index, fixture.party[index].actor.position(),
                          fixture.collision);
        REQUIRE(fixture.accepted(index, Phase::BeforeCarry));
    }
    fixture.collision.setObjectTransform(kDeck, glm::translate(Mat4{1}, Vec3{0, 1, 0}));
    std::array<PlayInput, 2> inputs{};
    inputs[0].move = {{0, 1}, 1};
    bool limited = false;
    auto events = floorEvents();
    events.limitMovement = [&](usize index, const Vec3& from, const Vec3& to) {
        if (index == 1) {
            limited |= from != to;
            return from;
        }
        return to;
    };
    PartyMotion::step(fixture.party, inputs, false, 0, 60 / rate, 1.0f / static_cast<f32>(rate),
                      fixture.collision, events);
    CHECK(fixture.party[1].knockback.pushed());
    CHECK(limited);
    for (usize index = 0; index < fixture.party.size(); ++index) {
        CHECK(fixture.party[index].actor.position().y == Approx(1));
        CHECK(fixture.party[index].floor.object == kDeck);
        CHECK(fixture.accepted(index, Phase::AfterLanding));
    }
}

TEST_CASE("ordinary floor admission rejects malformed historical links before native inverse",
          "[game][screens][ordinary-floor-support][rollback]") {
    Fixture fixture;
    auto& player = fixture.party[0];
    SECTION("missing placement") {
        player.floor.placement.reset();
    }
    SECTION("placement without native moving flag") {
        player.floor.flags = kFloor;
    }
    SECTION("unknown collision owner") {
        player.floor.object = 99;
    }
    SECTION("different valid collision owner") {
        player.floor.object = kOtherDeck;
    }
    SECTION("incorrect native support flags") {
        player.floor.flags |= FloorRiding::kKeptApart;
    }
    SECTION("singular historical transform") {
        player.floor.placement = glm::scale(Mat4{1}, Vec3{1, 0, 1});
    }
    SECTION("nonfinite historical transform") {
        (*player.floor.placement)[0][0] = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("projective historical transform") {
        (*player.floor.placement)[0][3] = 0.5f;
    }
    SECTION("inverse overflows") {
        (*player.floor.placement)[0][0] = std::numeric_limits<f32>::denorm_min();
    }
    SECTION("collapsed current support") {
        fixture.collision.setObjectTransform(kDeck, glm::scale(Mat4{1}, Vec3{0}));
    }
    SECTION("hidden current support") {
        fixture.collision.setSolid(kDeck, false);
    }
    SECTION("departed member") {
        player.departed = true;
    }
    SECTION("dying member") {
        player.life = PlayerLife::Dying;
    }
    SECTION("invalid actor") {
        player.actor.place({0, std::numeric_limits<f32>::infinity(), 0});
    }
    const auto before = player.actor.checkpoint();
    CHECK_FALSE(fixture.accepted(0, Phase::BeforeCarry));
    CHECK_FALSE(fixture.accepted(0, Phase::AfterLanding));
    CHECK(player.actor.checkpoint() == before);
}

TEST_CASE("ordinary floor admission refuses unsupported falling and native rescue discontinuities",
          "[game][screens][ordinary-floor-support][rollback]") {
    Fixture fixture;
    SECTION("feet above support") {
        fixture.party[0].actor.place({2, 0.2f, 0});
        CHECK_FALSE(fixture.accepted(0, Phase::BeforeCarry));
        CHECK_FALSE(fixture.accepted(0, Phase::AfterLanding));
    }
    SECTION("feet outside support") {
        fixture.party[0].actor.place({2, 0, 10});
        CHECK_FALSE(fixture.accepted(0, Phase::BeforeCarry));
        CHECK_FALSE(fixture.accepted(0, Phase::AfterLanding));
    }
    SECTION("carrier goes under native rescue depth") {
        fixture.collision.setObjectTransform(kDeck, glm::translate(Mat4{1}, Vec3{0, -5, 0}));
        CHECK_FALSE(fixture.accepted(0, Phase::BeforeCarry));
        const std::array<PlayInput, 2> inputs{};
        PartyMotion::step(fixture.party, inputs, false, 0, 1, 1.0f / 60, fixture.collision,
                          floorEvents());
        // Native rescue found a safe spot by the other member and cleared the
        // floor link. A grounded endpoint must not hide that discontinuity.
        CHECK(fixture.party[0].actor.position().y == Approx(0));
        CHECK(fixture.party[0].floor.object == -1);
        CHECK_FALSE(fixture.accepted(0, Phase::AfterLanding));
    }
}

TEST_CASE("ordinary floor admission preserves footprint ownership and current solidity",
          "[game][screens][ordinary-floor-support][rollback]") {
    std::vector<CollisionTriangle> triangles;
    floor(triangles, {-5, -5}, {0, 5}, 2, kDeck, kFloor | FloorRiding::kMoving);
    floor(triangles, {-5, -5}, {5, 5}, 0, 3, kFloor);
    WorldCollision collision;
    collision.build(triangles);
    collision.setMovingObjects(std::array<s32, 1>{kDeck});
    std::array<PlayerRuntime, 1> party;
    party[0].actor.spawn(0, {}, nullptr, {0.1f, 2, 0}, 0);
    FloorRiding::land(party, 0, party[0].actor.position(), collision);
    REQUIRE(party[0].floor.object == kDeck);
    collision.setObjectTransform(kDeck, glm::translate(Mat4{1}, Vec3{0, -0.1f, 0}));
    CHECK(OrdinaryFloorSupport::accepts(party[0], collision, Phase::BeforeCarry, true));
    FloorRiding::carry(party[0], collision);
    FloorRiding::land(party, 0, party[0].actor.position(), collision);
    CHECK(OrdinaryFloorSupport::accepts(party[0], collision, Phase::AfterLanding, true));
    collision.setContactOnly(kDeck, true);
    CHECK(OrdinaryFloorSupport::accepts(party[0], collision, Phase::BeforeCarry, true));
    collision.setSolid(kDeck, false);
    CHECK_FALSE(OrdinaryFloorSupport::accepts(party[0], collision, Phase::BeforeCarry, true));
    collision.setSolid(kDeck, true);
    CHECK(OrdinaryFloorSupport::accepts(party[0], collision, Phase::BeforeCarry, true));
}

TEST_CASE("ordinary floor admission rejects an unknown phase and restores no live state",
          "[game][screens][ordinary-floor-support][rollback]") {
    Fixture fixture;
    const auto before = fixture.party[0].actor.checkpoint();
    CHECK_FALSE(OrdinaryFloorSupport::accepts(fixture.party[0], fixture.collision,
                                              static_cast<Phase>(99), true));
    CHECK(fixture.party[0].actor.checkpoint() == before);
    CHECK_FALSE(fixture.party[0].figure);
    CHECK_FALSE(fixture.party[0].mikeyFigure);
}
} // namespace
