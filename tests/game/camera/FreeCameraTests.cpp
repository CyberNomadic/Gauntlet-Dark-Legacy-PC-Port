#include <cmath>
#include <initializer_list>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/world/WorldCamera.h"

#include "game/camera/FreeCamera.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

bool near(const Vec3& a, const Vec3& b) {
    return glm::distance(a, b) < 1e-3f;
}

void hold(Input& input, std::initializer_list<Key> keys) {
    input.beginPoll();
    for (const Key key : keys) {
        input.setKey(key, true);
    }
}

void release(Input& input) {
    for (const Key key : {Key::W, Key::A, Key::S, Key::D, Key::Space, Key::LeftControl,
                          Key::LeftShift, Key::Up, Key::Down, Key::Left, Key::Right}) {
        input.setKey(key, false);
    }
    input.beginPoll();
}

TEST_CASE("the free camera takes off where the level's camera stood and flies by the keys",
          "[camera][free-camera][dev-console]") {
    FreeCamera camera;
    CHECK_FALSE(camera.active());
    const WorldCamera from{.position = Vec3{10.0f, 5.0f, -3.0f}, .pitch = 0.3f, .yaw = 1.0f};
    camera.start(from);
    CHECK(camera.active());
    CHECK(camera.camera() == from);
    Input input;
    hold(input, {Key::W});
    camera.update(input, 0.5f);
    CHECK(
        near(camera.camera().position, from.position + from.forward() * FreeCamera::kSpeed * 0.5f));
    CHECK(camera.camera().pitch == Approx(from.pitch));
    CHECK(camera.camera().yaw == Approx(from.yaw));
    Vec3 before = camera.camera().position;
    release(input);
    hold(input, {Key::S, Key::LeftShift}); // back, hurried
    camera.update(input, 0.5f);
    CHECK(near(camera.camera().position,
               before - from.forward() * FreeCamera::kSpeed * FreeCamera::kFastFactor * 0.5f));
    before = camera.camera().position;
    release(input);
    hold(input, {Key::D});
    camera.update(input, 1.0f);
    CHECK(near(camera.camera().position, before + from.right() * FreeCamera::kSpeed));
    before = camera.camera().position;
    release(input);
    hold(input, {Key::A});
    camera.update(input, 1.0f);
    CHECK(near(camera.camera().position, before - from.right() * FreeCamera::kSpeed));
    before = camera.camera().position;
    release(input);
    hold(input, {Key::Space}); // straight up, whatever the pitch
    camera.update(input, 1.0f);
    CHECK(near(camera.camera().position, before + Vec3{0.0f, FreeCamera::kSpeed, 0.0f}));
    before = camera.camera().position;
    release(input);
    hold(input, {Key::LeftControl});
    camera.update(input, 0.25f);
    CHECK(near(camera.camera().position, before - Vec3{0.0f, FreeCamera::kSpeed * 0.25f, 0.0f}));
    before = camera.camera().position;
    camera.stop();
    CHECK_FALSE(camera.active());
    release(input);
    hold(input, {Key::W});
    camera.update(input, 1.0f);
    CHECK(near(camera.camera().position, before)); // off: nothing moves
}

TEST_CASE("the arrow keys and a dragged pointer turn the free camera, the pitch within limits",
          "[camera][free-camera][dev-console]") {
    FreeCamera camera;
    const WorldCamera level{.position = Vec3{0.0f, 10.0f, 0.0f}, .pitch = 0.0f, .yaw = 0.0f};
    camera.start(level);
    Input input;
    hold(input, {Key::Right});
    camera.update(input, 0.25f);
    CHECK(camera.camera().yaw == Approx(FreeCamera::kTurnRate * 0.25f));
    CHECK(glm::dot(camera.camera().forward(), level.right()) > 0.0f); // toward its right
    release(input);
    hold(input, {Key::Left});
    camera.update(input, 0.25f);
    CHECK(camera.camera().yaw == Approx(0.0f).margin(1e-5f));
    release(input);
    hold(input, {Key::Up});
    camera.update(input, 0.25f);
    CHECK(camera.camera().pitch == Approx(-FreeCamera::kTurnRate * 0.25f)); // looks up
    CHECK(camera.camera().forward().y > 0.0f);
    release(input);
    hold(input, {Key::Down});
    camera.update(input, 10.0f);
    CHECK(camera.camera().pitch == Approx(FreeCamera::kPitchLimit)); // no further than down
    release(input);
    hold(input, {Key::Up});
    camera.update(input, 10.0f);
    CHECK(camera.camera().pitch == Approx(-FreeCamera::kPitchLimit));
    // Turning right for a whole turn and more leaves the yaw bounded.
    release(input);
    hold(input, {Key::Right});
    camera.update(input, 4.0f);
    CHECK(std::abs(camera.camera().yaw) <= kPi);
    // A pointer pressed turns nothing until it moves; then every pixel is a step.
    camera.start(level);
    release(input);
    input.setPointer({.x = 100.0f, .y = 100.0f, .inside = true, .down = true});
    camera.update(input, 1.0f / 60.0f);
    CHECK(camera.camera().yaw == Approx(0.0f));
    CHECK(camera.camera().pitch == Approx(0.0f));
    input.beginPoll();
    input.setPointer({.x = 110.0f, .y = 95.0f, .inside = true, .down = true});
    camera.update(input, 1.0f / 60.0f);
    CHECK(camera.camera().yaw == Approx(10.0f * FreeCamera::kDragTurn));
    CHECK(camera.camera().pitch == Approx(-5.0f * FreeCamera::kDragTurn));
    // Let go and pressed again elsewhere, the camera does not jump.
    input.beginPoll();
    input.setPointer({.x = 110.0f, .y = 95.0f, .inside = true, .down = false});
    camera.update(input, 1.0f / 60.0f);
    input.beginPoll();
    input.setPointer({.x = 300.0f, .y = 300.0f, .inside = true, .down = true});
    camera.update(input, 1.0f / 60.0f);
    CHECK(camera.camera().yaw == Approx(10.0f * FreeCamera::kDragTurn));
    CHECK(camera.camera().pitch == Approx(-5.0f * FreeCamera::kDragTurn));
}

} // namespace
