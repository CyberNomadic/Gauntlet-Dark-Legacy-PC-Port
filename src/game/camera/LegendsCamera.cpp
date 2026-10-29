#include "game/camera/LegendsCamera.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "engine/core/Error.h"

namespace gdl::game {
namespace {
constexpr f32 kStep = 1.0f / 30.0f;

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           glm::all(glm::lessThanEqual(glm::abs(value), Vec3{100000}));
}

f32 angle(f32 value) {
    return std::remainder(value, 2 * kPi);
}

// Native e00363e0 accelerates to a per-update angular cap, then brakes in the
// last four cap-widths. It takes the short route across the signed angle seam.
f32 turn(f32 current, f32 desired, f32 cap, f32& velocity) {
    const f32 difference = angle(desired - current);
    const f32 distance = std::abs(difference);
    if (distance > cap * 4) {
        velocity = std::min(velocity + cap / 16, cap);
    } else if (distance > cap) {
        velocity = std::max(velocity - cap / 4, cap / 4);
    } else if (distance > cap / 4) {
        velocity = cap / 4;
    } else {
        return angle(desired);
    }
    return angle(current + std::copysign(velocity, difference));
}

s32 nearestMarker(const formats::LegendsN64CameraData& data, const Vec3& point, s32 previous) {
    // e0035018 compares squared distances in integer XYZ quarters; marker zero
    // is reserved for the opening view. Retain the old marker until another is
    // at least 1.5 times closer, preventing oscillation at Voronoi boundaries.
    const auto distance = [&](usize index) {
        const Vec3 raw = data.markers[index].position * 4.0f;
        const Vec3 delta{std::trunc(point.x * 4) - raw.x, std::trunc(point.y * 4) - raw.y,
                         std::trunc(point.z * 4) - raw.z};
        return glm::dot(delta, delta);
    };
    s32 best = 1;
    f32 bestDistance = distance(1);
    for (usize i = 2; i < data.markers.size(); ++i) {
        const f32 candidate = distance(i);
        if (candidate < bestDistance) {
            best = static_cast<s32>(i);
            bestDistance = candidate;
        }
    }
    if (previous > 0 && static_cast<usize>(previous) < data.markers.size() &&
        distance(static_cast<usize>(previous)) * 4 < bestDistance * 9) {
        return previous;
    }
    return best;
}
} // namespace

void LegendsCamera::reset(formats::LegendsN64CameraData data, const Vec3& feet) {
    if (!finite(feet) || data.markers.size() < 2 || data.markers.size() > 4096 ||
        (data.tower && data.towerExits.empty())) {
        throw FormatError("Legends camera: missing native camera data or invalid actor position");
    }
    for (const auto& marker : data.markers) {
        if (!finite(marker.position) || !std::isfinite(marker.pitch) ||
            !std::isfinite(marker.yaw)) {
            throw FormatError("Legends camera: invalid marker");
        }
    }
    for (const auto& exit : data.towerExits) {
        if (!finite(exit) || exit.x * exit.x + exit.z * exit.z < .0001f) {
            throw FormatError("Legends camera: invalid tower return direction");
        }
    }
    *this = {};
    m_data = std::move(data);
    m_mode = m_data.tower ? Mode::TowerInner : Mode::Markers;
    m_marker = nearestMarker(m_data, feet, -1);
    const auto& marker = m_data.markers[static_cast<usize>(m_marker)];
    m_camera.yaw = marker.yaw;
    m_camera.pitch = marker.pitch;
    m_smoothed = feet + m_camera.up() * 3.0f;
    f32 pitch = m_camera.pitch;
    f32 yaw = m_camera.yaw;
    m_attention = target(feet, pitch, yaw);
    m_camera.pitch = pitch;
    m_camera.yaw = yaw;
    m_previousTarget = m_attention;
    // Skip the opening cinematic but start at a settled native follow view.
    // e0869a6c: inner min55, middle/outer min40. e0034ac8's min50
    // override is for level ordinal8 (bosses), not realm8: tower2 is level1.
    m_distance = m_mode == Mode::TowerInner ? 55.0f : (m_data.tower ? 40.0f : 36.0f);
    m_transition = false;
    place();
    m_previousCamera = m_camera;
}

Vec3 LegendsCamera::target(const Vec3& feet, f32& pitch, f32& yaw) {
    Vec3 desired = feet + m_camera.up() * 3.0f;
    if (!m_data.tower) {
        // e00352d0's two-parts-history/one-part-target filter.
        m_smoothed = (m_smoothed * 2.0f + desired) / 3.0f;
        m_marker = nearestMarker(m_data, m_smoothed, m_marker);
        const auto& marker = m_data.markers[static_cast<usize>(m_marker)];
        pitch = marker.pitch;
        yaw = marker.yaw;
        return m_smoothed;
    }

    // e08697d0 uses complete 3D actor distance, not just XZ. In single-player
    // its party-separation gate is trivially satisfied. Keep both hysteresis
    // bands: exiting the outer zone at87 enters middle before inner at42.
    const f32 distance = glm::dot(feet, feet);
    const Mode previous = m_mode;
    if (distance >= 91 * 91) {
        m_mode = Mode::TowerOuter;
    } else if (m_mode == Mode::TowerOuter && distance <= 87 * 87) {
        m_mode = Mode::TowerMiddle;
    } else if (m_mode == Mode::TowerInner && distance >= 47 * 47) {
        m_mode = Mode::TowerMiddle;
    } else if (distance <= 42 * 42) {
        m_mode = Mode::TowerInner;
    }
    if (previous != m_mode) {
        m_transition = true;
    }
    desired.y = std::max(desired.y, 1.0f); // e0035380's tower target floor
    if (m_mode == Mode::TowerInner) {
        pitch = .05f;
        // e0869a6c normalizes onto a radius20 horizontal ring at height7.
        // The exact origin is singular in retail; retain the last valid yaw.
        const f32 radial = std::hypot(desired.x, desired.z);
        yaw = radial > .0001f ? std::atan2(desired.x, desired.z) : m_camera.yaw;
        return {std::sin(yaw) * 20, 7, std::cos(yaw) * 20};
    }
    pitch = m_mode == Mode::TowerMiddle ? .1f : .7f;
    const f32 targetYaw = std::atan2(desired.x, desired.z);
    const Vec3* nearest = nullptr;
    f32 nearestAngle = std::numeric_limits<f32>::max();
    for (const auto& exit : m_data.towerExits) {
        // Native compares raw atan2 outputs here, without shortest-angle wrap.
        const f32 delta = std::atan2(exit.x, exit.z) - targetYaw;
        if (delta * delta < nearestAngle) {
            nearestAngle = delta * delta;
            nearest = &exit;
        }
    }
    const Vec3 ray = glm::normalize(Vec3{nearest->x, 0, nearest->z});
    const f32 projection = desired.x * ray.x + desired.z * ray.z;
    desired.x = projection * ray.x;
    desired.z = projection * ray.z;
    yaw = std::atan2(desired.x, desired.z);
    return desired;
}

void LegendsCamera::tick(const Vec3& feet) {
    m_previousCamera = m_camera;
    f32 desiredPitch = m_camera.pitch;
    f32 desiredYaw = m_camera.yaw;
    Vec3 desired = target(feet, desiredPitch, desiredYaw);
    const f32 angularCap = m_transition ? (m_data.tower ? .01f : .001f) : .05f;
    m_camera.pitch = turn(m_camera.pitch, desiredPitch, .02f, m_pitchVelocity);
    m_camera.yaw =
        turn(m_camera.yaw, desiredYaw, angularCap * (m_data.tower ? 1.5f : 1), m_yawVelocity);
    if (m_mode == Mode::TowerInner) {
        desired.x = std::sin(m_camera.yaw) * 20;
        desired.z = std::cos(m_camera.yaw) * 20;
    }
    if (angle(m_camera.yaw - desiredYaw) == 0 &&
        (m_mode != Mode::TowerInner || desired == m_attention)) {
        m_transition = false;
    }

    // e0036a2c is an acceleration-limited target follower. The native inner
    // tower mode bypasses it once a mode transition has settled (e086a0d8).
    const Vec3 step = desired - m_attention;
    const Vec3 difference = step - m_velocity;
    const f32 length = glm::length(difference);
    const f32 acceleration =
        m_data.tower ? (m_transition ? .1f : .3f) : (m_transition ? .05f : .1f);
    if ((m_mode == Mode::TowerInner && !m_transition) || length <= acceleration ||
        glm::dot(difference, m_velocity) < 0) {
        m_attention = desired;
        m_velocity = desired - m_previousTarget;
    } else {
        m_velocity += difference * (acceleration / length);
        m_attention += m_velocity;
    }
    m_previousTarget = desired;

    // Native distance response without multiplayer framing expansion. This
    // importer has one actor; scripted targets and combat framing stay absent.
    const f32 distance = m_mode == Mode::TowerInner ? 55.0f : (m_data.tower ? 40.0f : 36.0f);
    if (m_distance > distance) {
        m_distanceVelocity = std::max(m_distanceVelocity - .1f, -.95f);
        m_distance = std::max(distance, m_distance + m_distanceVelocity);
    } else if (m_distance < distance) {
        m_distanceVelocity = std::min(m_distanceVelocity + .1f, 1.05f);
        m_distance = std::min(distance, m_distance + m_distanceVelocity);
    } else {
        m_distanceVelocity = 0;
    }
    place();
}

void LegendsCamera::place() {
    m_camera.position = m_attention - m_camera.forward() * m_distance;
}

void LegendsCamera::update(const Vec3& feet, f32 seconds) {
    if (m_data.markers.empty() || !finite(feet) || !std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    m_accumulator += std::min(seconds, .25f);
    while (m_accumulator >= kStep) {
        tick(feet);
        m_accumulator -= kStep;
    }
}

WorldCamera LegendsCamera::sample(f32 extraSeconds) const {
    if (!std::isfinite(extraSeconds)) {
        extraSeconds = 0;
    }
    const f32 fraction =
        std::clamp((m_accumulator + std::max(extraSeconds, 0.0f)) / kStep, 0.0f, 1.0f);
    return m_camera.interpolate(m_previousCamera, fraction);
}

} // namespace gdl::game
