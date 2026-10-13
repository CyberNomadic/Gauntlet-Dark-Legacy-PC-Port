#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/WorldCollision.h"

namespace {
using namespace gdl;

TEST_CASE("animated surfaces stop colliding while collapsed and recover their original normals",
          "[world][world-collapsed-collision]") {
    const bool wall = GENERATE(false, true);
    const CollisionTriangle triangle =
        wall ? CollisionTriangle{{0, 0, 1}, {Vec3{-2, -2, 0}, Vec3{2, -2, 0}, Vec3{0, 2, 0}}, 0}
             : CollisionTriangle{{0, 1, 0}, {Vec3{-2, 0, -2}, Vec3{2, 0, -2}, Vec3{0, 0, 2}}, 0};
    WorldCollision collision;
    collision.build({triangle});
    collision.setMovingObjects(std::array<s32, 1>{0});
    const Vec3 from = wall ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const Vec3 to = -from;
    const auto checkExpanded = [&] {
        REQUIRE(collision.pickSurface(from, to));
        CHECK(*collision.pickSurface(from, to) == Vec3{0});
        CHECK_FALSE(collision.surfaceContacts({0, 0, 0}, 0.5f, -0.5f, 0.5f).empty());
        if (wall) {
            CHECK(collision.wallBetween(from, to));
            CHECK(collision.wallBetween(from, to, 0.25f));
            CHECK(collision.resolveWalls({0, 0, 0.1f}, 0.5f, -0.5f, 0.5f).z > 0.1f);
            CHECK(collision.sweepWalls(from, to, 0.5f, -0.5f, 0.5f).z > to.z);
        } else {
            const auto floor = collision.floorAt({0, 0, 0}, 1, 1);
            REQUIRE(floor);
            CHECK(floor->normal == triangle.normal);
            CHECK(floor->y == 0);
        }
    };
    checkExpanded();
    collision.setObjectTransform(0, glm::scale(Mat4{1}, Vec3{0}));
    CHECK(collision.loaded());
    CHECK(collision.triangleCount() == 1);
    CHECK(collision.movingObjectCount() == 1);
    CHECK_FALSE(collision.pickSurface(from, to));
    CHECK_FALSE(collision.floorAt({0, 0, 0}, 1, 1));
    CHECK_FALSE(collision.wallBetween({0, 0, 1}, {0, 0, -1}));
    // The swept sphere and cylinder edge tests would otherwise hit the collapsed point.
    CHECK_FALSE(collision.wallBetween({0, 0, 1}, {0, 0, -1}, 0.25f));
    CHECK(collision.surfaceContacts({0, 0, 0}, 0.5f, -0.5f, 0.5f).empty());
    CHECK(collision.resolveWalls({0, 0, 0}, 0.5f, -0.5f, 0.5f) == Vec3{0});
    CHECK(collision.sweepWalls({0, 0, 1}, {0, 0, -1}, 0.5f, -0.5f, 0.5f) == Vec3(0, 0, -1));
    collision.setObjectTransform(0, Mat4{1});
    checkExpanded();
}

TEST_CASE("nonzero animated normal normalization matches the native expression exactly",
          "[world][world-collapsed-collision]") {
    const f32 scale = GENERATE(0.01f, 0.25f, 1.0f, 3.0f);
    const CollisionTriangle triangle{
        {0, 1, 0}, {Vec3{-20, 0, -20}, Vec3{20, 0, -20}, Vec3{0, 0, 20}}, 0};
    WorldCollision collision;
    collision.build({triangle});
    collision.setMovingObjects(std::array<s32, 1>{0});
    const Mat4 transform = glm::rotate(Mat4{1}, 0.25f, Vec3{0, 0, 1}) *
                           glm::scale(Mat4{1}, Vec3{scale, scale * 2, scale * 3});
    collision.setObjectTransform(0, transform);
    const auto floor = collision.floorAt({0, 0, 0}, 1, 1);
    REQUIRE(floor);
    CHECK(floor->normal == glm::normalize(Mat3{transform} * triangle.normal));
    CHECK(std::isfinite(floor->y));
    CHECK(std::abs(floor->y) < 0.00001f);
}
} // namespace
