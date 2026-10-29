#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/hud/CompanionArrows.h"
#include "game/party/PlayerRuntime.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kWidth = 512;
constexpr f32 kHeight = 384;
constexpr f32 kBottom = 304;

/** Two seats, each in its own colour, the first this machine's. Clip space is the world
 * here (the identity clip), so a body at x of two is well off the right of the view. */
struct Fixture {
    std::array<PlayerRuntime, 2> players;
    Mat4 clip{1.0f};
    Mat4 canvas = makeScreenProjection(static_cast<s32>(kWidth), static_cast<s32>(kHeight));
    WorldCamera camera;
    Fixture() {
        for (usize i = 0; i < players.size(); ++i) {
            CharacterSave save;
            save.color = static_cast<s32>(i);
            players[i].actor.spawn(static_cast<s32>(i), save, nullptr, Vec3{0.0f}, 0.0f);
        }
    }
    /** Stands seat `i` with its body's centre at `at`. */
    void put(usize i, const Vec3& at) {
        const Vec3 offset = players[i].actor.attentionPoint() - players[i].actor.position();
        players[i].actor.place(at - offset);
    }
    std::vector<CompanionArrows::Arrow> arrows(u8 own = 0b01) {
        return CompanionArrows::place(players, own, camera, clip, canvas, kWidth, kHeight,
                                      kBottom);
    }
};

TEST_CASE("a companion off the right of the view gets an arrow at that edge in their colour",
          "[game][hud][companion-arrows]") {
    Fixture f;
    f.put(1, Vec3{2.0f, 0.0f, 0.0f});
    const auto arrows = f.arrows();
    REQUIRE(arrows.size() == 1);
    CHECK(arrows[0].seat == 1);
    CHECK(arrows[0].direction.x == Approx(1.0f));
    CHECK(arrows[0].direction.y == Approx(0.0f).margin(1e-5f));
    CHECK(arrows[0].tip.x == Approx(kWidth - CompanionArrows::kInset));
    CHECK(arrows[0].tip.y == Approx(kHeight * 0.5f));
    const Color expected = playerColor(1);
    CHECK(arrows[0].color.r == expected.r);
    CHECK(arrows[0].color.g == expected.g);
    CHECK(arrows[0].color.b == expected.b);
    CHECK(arrows[0].color.a == CompanionArrows::kAlpha);
}

TEST_CASE("a companion on the view, one in the tower and one's own seat get no arrow",
          "[game][hud][companion-arrows]") {
    Fixture f;
    f.put(1, Vec3{0.5f, 0.0f, 0.0f}); // on the view
    CHECK(f.arrows().empty());
    f.put(1, Vec3{2.0f, 0.0f, 0.0f});
    CHECK(f.arrows(0b11).empty()); // both seats are this machine's
    f.players[1].life = PlayerLife::InTower;
    CHECK(f.arrows().empty());
    f.players[1].life = PlayerLife::Standing;
    f.players[1].departed = true;
    CHECK(f.arrows().empty());
}

TEST_CASE("a companion behind the eye is pointed to across the ground, above the status boxes",
          "[game][hud][companion-arrows]") {
    Fixture f;
    f.clip[3][3] = -1.0f; // every point lands behind the eye
    f.put(1, Vec3{0.0f, 0.0f, -50.0f}); // behind a camera looking along +z
    const auto arrows = f.arrows();
    REQUIRE(arrows.size() == 1);
    CHECK(arrows[0].direction.x == Approx(0.0f).margin(1e-5f));
    CHECK(arrows[0].direction.y == Approx(1.0f));
    CHECK(arrows[0].tip.x == Approx(kWidth * 0.5f));
    CHECK(arrows[0].tip.y == Approx(kBottom - CompanionArrows::kInset));
}

TEST_CASE("the edge point keeps a diagonal arrow on the nearer side", "[game][hud][companion-arrows]") {
    const Vec2 up = CompanionArrows::edgePoint(Vec2{0.0f, -1.0f}, kWidth, kHeight, kBottom);
    CHECK(up.x == Approx(kWidth * 0.5f));
    CHECK(up.y == Approx(CompanionArrows::kInset));
    const Vec2 corner = CompanionArrows::edgePoint(glm::normalize(Vec2{1.0f, -1.0f}), kWidth,
                                                   kHeight, kBottom);
    // The top is nearer than the right from the centre: the arrow meets the top edge.
    CHECK(corner.y == Approx(CompanionArrows::kInset));
    CHECK(corner.x == Approx(kWidth * 0.5f + (kHeight * 0.5f - CompanionArrows::kInset)));
}

TEST_CASE("drawing an arrow fills its rim and its body", "[game][hud][companion-arrows]") {
    Fixture f;
    f.put(1, Vec3{2.0f, 0.0f, 0.0f});
    test::FakeRenderDevice device;
    Canvas canvas;
    CompanionArrows arrows;
    canvas.begin(device, Mat4{1.0f});
    arrows.draw(canvas, f.players, 0b01, f.camera, f.clip, f.canvas, kWidth, kHeight, kBottom);
    canvas.end();
    CHECK(arrows.shown() == 1);
    usize vertices = 0;
    for (const auto& draw : device.draws) {
        vertices += draw.vertices.size();
    }
    CHECK(vertices == 6); // two triangles
    canvas.begin(device, Mat4{1.0f});
    arrows.draw(canvas, f.players, 0b11, f.camera, f.clip, f.canvas, kWidth, kHeight, kBottom);
    canvas.end();
    CHECK(arrows.shown() == 0);
}

} // namespace
