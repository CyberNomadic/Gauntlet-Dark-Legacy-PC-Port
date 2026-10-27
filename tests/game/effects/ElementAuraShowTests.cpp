#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"
#include "game/effects/ElementAura.h"
#include "game/effects/ElementAuraShow.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Body = ElementAuraShow::Body;
using Kind = ElementAuraShow::Kind;

constexpr f32 kRadius = 2.25f; // a grunt grown an elemental one's half again
constexpr f32 kHeight = 9.0f;

/** Fire, lightning, holy and acid bodies, each in its own place. */
std::array<Body, 4> fourBodies() {
    return {{{1, 1, Vec3{0.0f, 0.0f, 0.0f}, kRadius, kHeight},
             {2, 2, Vec3{40.0f, 0.0f, 0.0f}, kRadius, kHeight},
             {3, 3, Vec3{0.0f, 0.0f, 40.0f}, kRadius, kHeight},
             {4, 4, Vec3{40.0f, 0.0f, 40.0f}, kRadius, kHeight}}};
}

/** A place's distance from the body's centre, in shells: one is the shell itself. */
f32 shells(const Body& body, const Vec3& place) {
    const Vec3 local = place - body.feet - Vec3{0.0f, 0.5f * body.height, 0.0f};
    const Vec3 axes{1.1f * body.radius, 0.52f * body.height, 1.1f * body.radius};
    return glm::length(local / axes);
}

TEST_CASE("each element's show keeps its own kinds of piece about its body",
          "[game][effects][aggression][elemental]") {
    ElementAuraShow show;
    const auto bodies = fourBodies();
    for (s32 frame = 0; frame < 90; ++frame) {
        show.step(bodies, 1.0f / 60.0f);
    }
    REQUIRE(show.auras() == 4);
    const Body& fire = bodies[0];
    const Body& lightning = bodies[1];
    const Body& holy = bodies[2];
    const Body& acid = bodies[3];
    // Fire: tongues lick up off the skin, embers fly, smoke rises off the top.
    CHECK(show.pieces(Kind::Tongue) > 0);
    CHECK(show.pieces(Kind::Ember) > 0);
    CHECK(show.pieces(Kind::Smoke) > 0);
    for (const Vec3& at : show.placesOf(fire.instance, Kind::Tongue)) {
        CHECK(shells(fire, at) > 0.4f);
        CHECK(shells(fire, at) < 2.5f);
        CHECK(at.y >= fire.feet.y - 0.5f);
    }
    for (const Vec3& at : show.placesOf(fire.instance, Kind::Smoke)) {
        CHECK(at.y > fire.feet.y + 0.5f * fire.height);
    }
    CHECK(show.placesOf(lightning.instance, Kind::Tongue).empty());
    // Lightning: arcs jag over the shell between two of its points; static crackles on it.
    CHECK(show.pieces(Kind::Arc) > 0);
    CHECK(show.pieces(Kind::Static) > 0);
    const auto arcPoints = show.placesOf(lightning.instance, Kind::Arc);
    REQUIRE(arcPoints.size() >= 6);
    for (const Vec3& at : arcPoints) {
        CHECK(shells(lightning, at) > 0.8f);
        CHECK(shells(lightning, at) < 1.6f);
    }
    for (const Vec3& at : show.placesOf(lightning.instance, Kind::Static)) {
        CHECK(shells(lightning, at) > 0.9f);
        CHECK(shells(lightning, at) < 1.15f);
    }
    CHECK(show.placesOf(fire.instance, Kind::Arc).empty());
    // Holy: rays break out from within; glints on the skin; motes drift up.
    CHECK(show.pieces(Kind::Ray) > 0);
    CHECK(show.pieces(Kind::Glint) > 0);
    CHECK(show.pieces(Kind::Mote) > 0);
    for (const Vec3& at : show.placesOf(holy.instance, Kind::Ray)) {
        CHECK(shells(holy, at) < 0.9f); // its start, inside the body
    }
    for (const Vec3& at : show.placesOf(holy.instance, Kind::Glint)) {
        CHECK(shells(holy, at) > 0.9f);
        CHECK(shells(holy, at) < 1.2f);
    }
    // Acid: rings roll out over the ground at the feet; wisps rise; drips fall and splash.
    CHECK(show.pieces(Kind::Ring) > 0);
    CHECK(show.pieces(Kind::Wisp) > 0);
    CHECK(show.pieces(Kind::Drip) + show.pieces(Kind::Ring) > 1);
    for (const Vec3& at : show.placesOf(acid.instance, Kind::Ring)) {
        CHECK(at.y == acid.feet.y + ElementAuraShow::kRingLift);
    }
    for (const Vec3& at : show.placesOf(acid.instance, Kind::Drip)) {
        CHECK(at.y > acid.feet.y);
        CHECK(at.y < acid.feet.y + acid.height);
    }
    CHECK(show.placesOf(holy.instance, Kind::Ring).empty());
    // Every piece is one of its body's, and no body carries more than the most.
    CHECK(show.pieces() <= 4 * ElementAuraShow::kMostPieces);
    CHECK(show.pieces() > 0);
}

TEST_CASE("an aura follows its body and goes when the body does, within the most auras",
          "[game][effects][aggression][elemental]") {
    ElementAuraShow show;
    auto bodies = fourBodies();
    show.step(bodies, 1.0f / 30.0f);
    show.step(bodies, 1.0f / 30.0f);
    REQUIRE(show.auras() == 4);
    bodies[0].feet = Vec3{100.0f, 0.0f, 0.0f}; // fire walks off; its flames come along
    show.step(bodies, 1.0f / 30.0f);
    for (const Vec3& at : show.placesOf(1, Kind::Tongue)) {
        CHECK(at.x > 90.0f);
    }
    const std::array<Body, 2> fewer{bodies[1], bodies[3]};
    show.step(fewer, 1.0f / 30.0f);
    CHECK(show.auras() == 2);
    CHECK(show.placesOf(1, Kind::Tongue).empty());
    show.step({}, 1.0f / 30.0f);
    CHECK(show.auras() == 0);
    // No aura for a plain body, nor for a slot holding nothing, nor beyond the most.
    std::vector<Body> crowd;
    crowd.push_back({0, 1, Vec3{0.0f}, kRadius, kHeight});  // no lifetime
    crowd.push_back({99, 0, Vec3{0.0f}, kRadius, kHeight}); // no element
    for (u64 i = 1; i <= ElementAuraShow::kMostAuras + 5; ++i) {
        crowd.push_back({100 + i, 1 + static_cast<s32>(i % 4), Vec3{0.0f}, kRadius, kHeight});
    }
    show.step(crowd, 1.0f / 30.0f);
    CHECK(show.auras() == ElementAuraShow::kMostAuras);
    // One body held long never carries more than the most pieces.
    show.clear();
    const std::array<Body, 1> one{bodies[1]};
    for (s32 frame = 0; frame < 600; ++frame) {
        show.step(one, 1.0f / 60.0f);
    }
    CHECK(show.pieces() <= ElementAuraShow::kMostPieces);
    CHECK(show.pieces() > 0);
}

TEST_CASE("the show draws every element over its atlas, additive but for the smoke, no depth written",
          "[game][effects][aggression][elemental]") {
    test::FakeRenderDevice device;
    ElementAuraShow show;
    const auto bodies = fourBodies();
    const CameraFrame camera = CameraFrame::at(Vec3{20.0f, 30.0f, -40.0f});
    show.draw(device, Mat4{1.0f}, camera); // nothing yet: no body, no atlas made
    CHECK(device.draws.empty());
    CHECK(device.texturesCreated == 0);
    for (s32 frame = 0; frame < 60; ++frame) {
        show.step(bodies, 1.0f / 60.0f);
    }
    show.draw(device, Mat4{1.0f}, camera);
    REQUIRE_FALSE(device.draws.empty());
    CHECK(device.texturesCreated == 1);
    bool additive = false;
    bool soft = false;
    u8 brightest = 0;
    u8 mostOpaque = 0;
    usize vertices = 0;
    for (const auto& draw : device.draws) {
        CHECK(draw.texture == device.draws.front().texture);
        CHECK_FALSE(draw.state.depthWrite);
        CHECK(draw.state.depthTest);
        CHECK(draw.state.alphaTest > 0.0f);
        additive = additive || draw.blend() == BlendMode::Additive;
        soft = soft || draw.blend() == BlendMode::Alpha;
        for (const ImmediateVertex& vertex : draw.vertices) {
            ++vertices;
            brightest = std::max({brightest, vertex.color.r, vertex.color.g, vertex.color.b});
            mostOpaque = std::max(mostOpaque, vertex.color.a);
            // Every vertex is about one of the bodies: wide of it at most by its rings, rays
            // and embers.
            const bool near = std::ranges::any_of(bodies, [&](const Body& body) {
                const Vec3 flat{vertex.position.x - body.feet.x, 0.0f,
                                vertex.position.z - body.feet.z};
                return glm::length(flat) <= 5.0f * body.radius + 4.0f &&
                       vertex.position.y >= body.feet.y - 4.0f && // the halos' rims
                       vertex.position.y <= body.feet.y + body.height + 12.0f;
            });
            CAPTURE(vertex.position.x, vertex.position.y, vertex.position.z);
            CHECK(near);
        }
    }
    CHECK(additive);
    CHECK(soft);
    CHECK(vertices % 6 == 0); // quads and strips, two triangles each
    CHECK(vertices >= 120); // twenty quads at the least
    CHECK(brightest >= 200); // colours 0 to 255, bright enough to show
    CHECK(mostOpaque >= 200);
    // Cleared, it draws nothing; released, it makes its atlas again.
    show.clear();
    device.draws.clear();
    show.draw(device, Mat4{1.0f}, camera);
    CHECK(device.draws.empty());
    show.release();
    show.step(bodies, 1.0f / 30.0f);
    show.draw(device, Mat4{1.0f}, camera);
    CHECK(device.texturesCreated == 2);
}

} // namespace
