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
    m_velocity.reset();
    m_dragFrom.reset();
}

void FreeCamera::stop() {
    m_active = false;
    m_velocity.reset();
    m_dragFrom.reset();
}

void FreeCamera::update(const Input& input, f32 seconds) {
    if (!m_active) {
        return;
    }
    Vec3 wish{0.0f};
    if (input.isKeyDown(Key::W)) {
        wish += m_camera.forward();
    }
    if (input.isKeyDown(Key::S)) {
        wish -= m_camera.forward();
    }
    if (input.isKeyDown(Key::D)) {
        wish += m_camera.right();
    }
    if (input.isKeyDown(Key::A)) {
        wish -= m_camera.right();
    }
    if (input.isKeyDown(Key::Q)) {
        wish += kWorldUp;
    }
    if (input.isKeyDown(Key::E)) {
        wish -= kWorldUp;
    }
    if (const f32 length = glm::length(wish); length > 1.0f) {
        wish /= length;
    }
    wish *= kSpeed * (input.isKeyDown(Key::LeftShift) ? kSlowFactor : 1.0f);
    m_camera.position += m_velocity.advance(wish, seconds);
    // The pointer dragged with the right button held turns the view: across for yaw, a
    // larger one turning toward the camera's right, and down for pitch, looking further down.
    const PointerSnapshot& pointer = input.pointer();
    if (input.isKeyDown(Key::MouseRight)) {
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
