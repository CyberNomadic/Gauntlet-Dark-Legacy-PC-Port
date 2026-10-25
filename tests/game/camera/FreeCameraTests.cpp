#include <cmath>
#include <initializer_list>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/world/WorldCamera.h"

#include "game/camera/FlightVelocity.h"
#include "game/camera/FreeCamera.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 60.0f;

bool near(const Vec3& a, const Vec3& b) {
    return glm::distance(a, b) < 1e-3f;
}

bool alongside(const Vec3& a, const Vec3& b) {
    return glm::length(a) > 1e-4f && glm::length(b) > 1e-4f &&
           glm::dot(glm::normalize(a), glm::normalize(b)) > 0.999f;
}

void hold(Input& input, std::initializer_list<Key> keys) {
    input.beginPoll();
    for (const Key key : keys) {
        input.setKey(key, true);
    }
}

void release(Input& input) {
    for (const Key key : {Key::W, Key::A, Key::S, Key::D, Key::Q, Key::E, Key::LeftShift, Key::Up,
                          Key::Down, Key::Left, Key::Right, Key::MouseLeft, Key::MouseRight}) {
        input.setKey(key, false);
    }
    input.beginPoll();
}

/** Flies `steps` of a sixtieth with `keys` alone held; the last step's distance. */
f32 fly(FreeCamera& camera, Input& input, std::initializer_list<Key> keys, s32 steps) {
    release(input);
    f32 last = 0.0f;
    for (s32 i = 0; i < steps; ++i) {
        hold(input, keys);
        const Vec3 before = camera.camera().position;
        camera.update(input, kStep);
        last = glm::distance(camera.camera().position, before);
    }
    return last;
}

TEST_CASE("the free camera takes off where the level's camera stood and flies by the keys, "
          "easing in and out",
          "[camera][free-camera][dev-console]") {
    FreeCamera camera;
    CHECK_FALSE(camera.active());
    const WorldCamera from{.position = Vec3{10.0f, 5.0f, -3.0f}, .pitch = 0.3f, .yaw = 1.0f};
    camera.start(from);
    CHECK(camera.active());
    CHECK(camera.camera() == from);
    CHECK(camera.velocity() == Vec3{0.0f});
    Input input;
    // Held forward, it gathers speed step by step and settles at its pace along its own
    // forward, pitched as it is.
    f32 previous = 0.0f;
    for (s32 i = 0; i < 30; ++i) {
        const f32 step = fly(camera, input, {Key::W}, 1);
        CHECK(step > previous);
        previous = step;
    }
    CHECK(previous < FreeCamera::kSpeed * kStep);
    CHECK(fly(camera, input, {Key::W}, 240) == Approx(FreeCamera::kSpeed * kStep).epsilon(0.01));
    CHECK(alongside(camera.velocity(), from.forward()));
    CHECK(camera.camera().pitch == Approx(from.pitch));
    CHECK(camera.camera().yaw == Approx(from.yaw));
    // Let go, it coasts to a stop instead of halting.
    release(input);
    Vec3 before = camera.camera().position;
    camera.update(input, kStep);
    const f32 coast = glm::distance(camera.camera().position, before);
    CHECK(coast > 0.0f);
    CHECK(coast < FreeCamera::kSpeed * kStep);
    for (s32 i = 0; i < 240; ++i) {
        camera.update(input, kStep);
    }
    CHECK(camera.velocity() == Vec3{0.0f});
    // Shift slows it to a quarter of its pace; S flies back.
    CHECK(fly(camera, input, {Key::S, Key::LeftShift}, 300) ==
          Approx(FreeCamera::kSpeed * FreeCamera::kSlowFactor * kStep).epsilon(0.01));
    CHECK(alongside(camera.velocity(), -from.forward()));
    release(input);
    // D and A along its right, Q and E straight up and down whatever the pitch.
    fly(camera, input, {Key::D}, 300);
    CHECK(alongside(camera.velocity(), from.right()));
    fly(camera, input, {Key::A}, 300);
    CHECK(alongside(camera.velocity(), -from.right()));
    fly(camera, input, {Key::Q}, 300);
    CHECK(alongside(camera.velocity(), Vec3{0.0f, 1.0f, 0.0f}));
    fly(camera, input, {Key::E}, 300);
    CHECK(alongside(camera.velocity(), Vec3{0.0f, -1.0f, 0.0f}));
    // Two keys together go no faster than one.
    CHECK(fly(camera, input, {Key::W, Key::D}, 300) ==
          Approx(FreeCamera::kSpeed * kStep).epsilon(0.01));
    // Off, nothing moves, and it takes off again at rest.
    camera.stop();
    CHECK_FALSE(camera.active());
    before = camera.camera().position;
    fly(camera, input, {Key::W}, 10);
    CHECK(near(camera.camera().position, before));
    camera.start(from);
    CHECK(camera.velocity() == Vec3{0.0f});
}

TEST_CASE("the pointer dragged with the right button held turns the free camera, the pitch "
          "within limits",
          "[camera][free-camera][dev-console]") {
    FreeCamera camera;
    const WorldCamera level{.position = Vec3{0.0f, 10.0f, 0.0f}, .pitch = 0.0f, .yaw = 0.0f};
    camera.start(level);
    Input input;
    // The arrow keys no longer turn it.
    hold(input, {Key::Right, Key::Up});
    camera.update(input, 1.0f);
    CHECK(camera.camera().yaw == Approx(0.0f));
    CHECK(camera.camera().pitch == Approx(0.0f));
    release(input);
    // The right button pressed turns nothing until the pointer moves; then a drag across the
    // whole view is kDragTurn, across for yaw toward the camera's right, down for pitch down.
    const auto drag = [&](f32 x, f32 y, bool right) {
        input.beginPoll();
        input.setKey(Key::MouseRight, right);
        input.setPointer({.x = x, .y = y, .inside = true, .down = false});
        camera.update(input, kStep);
    };
    drag(0.5f, 0.5f, true);
    CHECK(camera.camera().yaw == Approx(0.0f));
    CHECK(camera.camera().pitch == Approx(0.0f));
    drag(0.6f, 0.45f, true);
    CHECK(camera.camera().yaw == Approx(0.1f * FreeCamera::kDragTurn));
    CHECK(glm::dot(camera.camera().forward(), level.right()) > 0.0f);
    CHECK(camera.camera().pitch == Approx(-0.05f * FreeCamera::kDragTurn)); // looks up
    CHECK(camera.camera().forward().y > 0.0f);
    // Let go and pressed again elsewhere, the camera does not jump.
    drag(0.6f, 0.45f, false);
    drag(0.9f, 0.9f, true);
    CHECK(camera.camera().yaw == Approx(0.1f * FreeCamera::kDragTurn));
    CHECK(camera.camera().pitch == Approx(-0.05f * FreeCamera::kDragTurn));
    // The left button alone turns nothing.
    input.beginPoll();
    input.setKey(Key::MouseRight, false);
    input.setKey(Key::MouseLeft, true);
    input.setPointer({.x = 0.1f, .y = 0.1f, .inside = true, .down = true});
    camera.update(input, kStep);
    input.beginPoll();
    input.setPointer({.x = 0.9f, .y = 0.9f, .inside = true, .down = true});
    camera.update(input, kStep);
    CHECK(camera.camera().yaw == Approx(0.1f * FreeCamera::kDragTurn));
    CHECK(camera.camera().pitch == Approx(-0.05f * FreeCamera::kDragTurn));
    input.setKey(Key::MouseLeft, false);
    // Dragged down and down again, the pitch stops at the limit; up, at the other.
    camera.start(level);
    drag(0.5f, 0.0f, true);
    drag(0.5f, 1.0f, true);
    drag(0.5f, 0.0f, false);
    drag(0.5f, 0.0f, true);
    drag(0.5f, 1.0f, true);
    CHECK(camera.camera().pitch == Approx(FreeCamera::kPitchLimit));
    drag(0.5f, 1.0f, false);
    drag(0.5f, 1.0f, true);
    drag(0.5f, 0.0f, true);
    drag(0.5f, 0.0f, false);
    drag(0.5f, 1.0f, true);
    drag(0.5f, 0.0f, true);
    CHECK(camera.camera().pitch == Approx(-FreeCamera::kPitchLimit));
    // Turned right for more than a whole turn, the yaw stays bounded.
    for (s32 i = 0; i < 4; ++i) {
        drag(0.0f, 0.5f, false);
        drag(0.0f, 0.5f, true);
        drag(1.0f, 0.5f, true);
    }
    CHECK(std::abs(camera.camera().yaw) <= kPi);
}

TEST_CASE("a flight velocity closes on what is asked by a fixed share a second and rests",
          "[camera][free-camera]") {
    FlightVelocity velocity;
    const Vec3 wish{12.0f, 0.0f, 0.0f};
    // After one second it has closed all but e^-kResponse of the gap.
    const Vec3 way = velocity.advance(wish, 1.0f);
    CHECK(velocity.velocity.x == Approx(12.0f * (1.0f - std::exp(-FlightVelocity::kResponse))));
    CHECK(way == velocity.velocity);
    // Asked nothing, it dwindles and is cut off under the rest.
    for (s32 i = 0; i < 600; ++i) {
        velocity.advance(Vec3{0.0f}, kStep);
    }
    CHECK(velocity.velocity == Vec3{0.0f});
    velocity.advance(wish, 1.0f);
    velocity.reset();
    CHECK(velocity.velocity == Vec3{0.0f});
}

} // namespace
