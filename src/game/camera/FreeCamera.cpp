#include "game/camera/FreeCamera.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {

namespace {
constexpr Vec3 kWorldUp{0.0f, 1.0f, 0.0f};
} // namespace

void FreeCamera::start(const WorldCamera& from) {
    m_active = true;
    m_camera = from;
    m_camera.roll = 0.0f;
    m_dragFrom.reset();
}

void FreeCamera::stop() {
    m_active = false;
    m_dragFrom.reset();
}

void FreeCamera::update(const Input& input, f32 seconds) {
    if (!m_active) {
        return;
    }
    const f32 pace = kSpeed * (input.isKeyDown(Key::LeftShift) ? kFastFactor : 1.0f) * seconds;
    Vec3 move{0.0f};
    if (input.isKeyDown(Key::W)) {
        move += m_camera.forward();
    }
    if (input.isKeyDown(Key::S)) {
        move -= m_camera.forward();
    }
    if (input.isKeyDown(Key::D)) {
        move += m_camera.right();
    }
    if (input.isKeyDown(Key::A)) {
        move -= m_camera.right();
    }
    if (input.isKeyDown(Key::Space)) {
        move += kWorldUp;
    }
    if (input.isKeyDown(Key::LeftControl)) {
        move -= kWorldUp;
    }
    m_camera.position += move * pace;
    // A larger yaw turns toward the camera's right; a larger pitch looks further down.
    const f32 turn = kTurnRate * seconds;
    if (input.isKeyDown(Key::Right)) {
        m_camera.yaw += turn;
    }
    if (input.isKeyDown(Key::Left)) {
        m_camera.yaw -= turn;
    }
    if (input.isKeyDown(Key::Up)) {
        m_camera.pitch -= turn;
    }
    if (input.isKeyDown(Key::Down)) {
        m_camera.pitch += turn;
    }
    const PointerSnapshot& pointer = input.pointer();
    if (pointer.down) {
        const Vec2 at{pointer.x, pointer.y};
        if (m_dragFrom.has_value()) {
            m_camera.yaw += (at.x - m_dragFrom->x) * kDragTurn;
            m_camera.pitch += (at.y - m_dragFrom->y) * kDragTurn;
        }
        m_dragFrom = at;
    } else {
        m_dragFrom.reset();
    }
    m_camera.pitch = std::clamp(m_camera.pitch, -kPitchLimit, kPitchLimit);
    m_camera.yaw = std::remainder(m_camera.yaw, kTwoPi);
}

} // namespace gdl::game
