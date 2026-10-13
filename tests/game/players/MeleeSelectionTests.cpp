#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/players/MeleeSelection.h"

namespace {
using namespace gdl;
using namespace gdl::game;

MeleeSelection::Target targetAt(const Vec3& base, f32 distance, f32 score, bool low = false) {
    return {base, base, distance, score, low};
}

TEST_CASE("melee selection preserves acquisition weighting and original tie order",
          "[player-attacks][melee-selection]") {
    MeleeSelection::Snapshot snapshot;
    snapshot.reach = 2;
    snapshot.targets = {targetAt({0, 0, 4}, 1, 3), targetAt({1, 0, 5}, 2, 2),
                        targetAt({-1, 0, 5}, 2, 2)};
    const auto selected = MeleeSelection::select(snapshot, {0, 0, 1}, 5);
    REQUIRE(selected);
    CHECK_FALSE(selected->contact);
    CHECK(selected->index == 1);
    CHECK(selected->distance == 2);
    // The near target's physical radius wins reach checks, not ranking.
    snapshot.targets[1].distance = 5;
    snapshot.targets[2].distance = 5;
    REQUIRE(MeleeSelection::select(snapshot, {0, 0, 1}, 5));
    CHECK(MeleeSelection::select(snapshot, {0, 0, 1}, 5)->index == 0);
}

TEST_CASE("melee acquisition uses the full encounter range for its narrowing cone",
          "[player-attacks][melee-selection]") {
    MeleeSelection::Snapshot snapshot;
    snapshot.reach = 2;
    snapshot.targets = {targetAt({0.6f, 0, 0.8f}, 1, 20)};
    CHECK_FALSE(MeleeSelection::select(snapshot, {0, 0, 1}, 5));
    snapshot.range = 200;
    CHECK(MeleeSelection::select(snapshot, {0, 0, 1}, 5));
    CHECK_FALSE(MeleeSelection::select(snapshot, {0, 0, -1}, 5));
    CHECK_FALSE(MeleeSelection::select(snapshot, {0, 0, 0}, 5));
    CHECK_FALSE(MeleeSelection::select(snapshot, {0, 0, 1}, 0));
    snapshot.targets[0].score = snapshot.range;
    CHECK_FALSE(MeleeSelection::select(snapshot, {0, 0, 1}, 5));
}

TEST_CASE("an accepted swarm contact owns melee acquisition even outside swing reach",
          "[player-attacks][melee-selection]") {
    MeleeSelection::Snapshot snapshot;
    snapshot.reach = 2;
    snapshot.targets = {targetAt({0, 0, 1}, 0.5f, 0.5f, true)};
    snapshot.contact = MeleeSelection::Contact{targetAt({0, 0, 10}, 8, 8), false};
    const auto selected = MeleeSelection::select(snapshot, {0, 0, 1}, 5);
    REQUIRE(selected);
    CHECK(selected->contact);
    CHECK(selected->distance == 8);
    CHECK(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).range == MeleeRange::Beyond);
    snapshot.contact->target.distance = 2;
    const auto close = MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0);
    CHECK(close.range == MeleeRange::Swing);
    CHECK_FALSE(close.low);
    snapshot.contact->target.distance = snapshot.range;
    CHECK(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).low);
}

TEST_CASE("overlapping swarm contacts bypass facing except Death and use a 3D cone",
          "[player-attacks][melee-selection]") {
    MeleeSelection::Snapshot snapshot;
    snapshot.reach = 2;
    snapshot.targets = {targetAt({0, 0, 1}, 0.5f, 0.5f, true)};
    snapshot.contact = MeleeSelection::Contact{targetAt({0, 0, -1}, -0.5f, 0), false};
    CHECK_FALSE(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).low);
    snapshot.contact->requireFacing = true;
    CHECK(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).low);
    snapshot.contact->requireFacing = false;
    snapshot.contact->target.distance = 0.5f;
    CHECK(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).low);
    snapshot.contact->target.offset = {0, 10, 1};
    CHECK(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).low);
    snapshot.contact->target.offset = {0, 0, 1};
    CHECK_FALSE(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).low);
}

TEST_CASE("melee held bias changes range but never extends the low-strike threshold",
          "[player-attacks][melee-selection]") {
    MeleeSelection::Snapshot snapshot;
    snapshot.reach = 2;
    snapshot.targets = {targetAt({0, 0, 4}, 3, 3, true)};
    CHECK(MeleeSelection::sense(snapshot, false, {0, 0, 1}, 0).range == MeleeRange::Step);
    CHECK(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).range == MeleeRange::Swing);
    CHECK(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).low);
    snapshot.targets[0].distance = 4;
    CHECK(MeleeSelection::sense(snapshot, false, {0, 0, 1}, 0).range == MeleeRange::Beyond);
    CHECK(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).range == MeleeRange::Step);
    CHECK_FALSE(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).low);
    snapshot.targets[0].distance = 5;
    CHECK(MeleeSelection::sense(snapshot, true, {0, 0, 1}, 0).range == MeleeRange::Beyond);
}

TEST_CASE("melee sensing separates acquisition heading from the body's assisted rotation",
          "[player-attacks][melee-selection]") {
    MeleeSelection::Snapshot snapshot;
    snapshot.reach = 2;
    snapshot.feet = {10, 2, 10};
    auto target = targetAt({11, 2, 11}, 1, 1);
    target.offset = {1, 0, 1};
    snapshot.targets = {target};
    const Vec3 attackFacing{1, 0, 1};
    const auto before = MeleeSelection::sense(snapshot, true, attackFacing, 0);
    CHECK(before.range == MeleeRange::Swing);
    CHECK(before.yaw == Catch::Approx(std::numbers::pi_v<f32> / 4));
    const auto after =
        MeleeSelection::sense(snapshot, true, attackFacing, std::numbers::pi_v<f32> / 4);
    CHECK(after.range == MeleeRange::Swing);
    CHECK(after.yaw == Catch::Approx(0).margin(1e-5f));
    const auto backward = MeleeSelection::sense(snapshot, true, {-1, 0, -1}, 0);
    CHECK(backward.range == MeleeRange::Beyond);
}

TEST_CASE("ordinary melee deeds retain stationary stepping low and strong-chain rules",
          "[player-attacks][melee-selection]") {
    for (const bool strong : {false, true}) {
        for (const bool moved : {false, true}) {
            for (const bool low : {false, true}) {
                for (const s32 chain : {0, 1, 2, 3}) {
                    CAPTURE(strong, moved, low, chain);
                    const auto ranged = strong ? PlayerDeed::StrongAttack : PlayerDeed::Attack;
                    const auto plain = low ? PlayerDeed::MeleeLow : PlayerDeed::Melee;
                    const auto heavy = low ? PlayerDeed::MeleeSlowLow : PlayerDeed::MeleeSlow;
                    const auto melee = strong ? heavy : plain;
                    CHECK(MeleeSelection::deed({MeleeRange::Beyond, low, 0}, strong, moved,
                                               chain) == ranged);
                    CHECK(MeleeSelection::deed({MeleeRange::Swing, low, 0}, strong, moved, chain) ==
                          melee);
                    const bool stepping = moved && (!low || (strong && chain != 0));
                    CHECK(MeleeSelection::deed({MeleeRange::Step, low, 0}, strong, moved, chain) ==
                          (stepping ? melee : ranged));
                }
            }
        }
    }
}
} // namespace
