#include "engine/world/WorldCamera.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

namespace gdl {

namespace {

/**
 * The camera's axes in world space. The original's world is left-handed: x right, y up and
 * z into the screen, so the axes form a left-handed frame and the eye looks along +z. A
 * positive pitch looks down.
 */
struct Axes {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
};

Axes axesOf(f32 pitch, f32 yaw, f32 roll) {
    const f32 cp = portable::cos(pitch);
    const f32 sp = portable::sin(pitch);
    const f32 cy = portable::cos(yaw);
    const f32 sy = portable::sin(yaw);
    const f32 cr = portable::cos(roll);
    const f32 sr = portable::sin(roll);
    Axes axes{};
    axes.forward = Vec3{sy * cp, -sp, cy * cp};
    const Vec3 up{sy * sp, cp, cy * sp};
    const Vec3 right{cy, 0.0f, -sy};
    // Roll turns right and up about the forward axis.
    axes.right = right * cr + up * sr;
    axes.up = up * cr - right * sr;
    return axes;
}

} // namespace

WorldCamera WorldCamera::interpolate(const WorldCamera& previous, f32 fraction) const {
    const f32 t = std::clamp(fraction, 0.0f, 1.0f);
    const auto angle = [t](f32 from, f32 to) {
        return from + std::remainder(to - from, kTwoPi) * t;
    };
    return {glm::mix(previous.position, position, t), angle(previous.pitch, pitch),
            angle(previous.yaw, yaw), angle(previous.roll, roll)};
}

Vec3 WorldCamera::right() const {
    return axesOf(pitch, yaw, roll).right;
}

Vec3 WorldCamera::up() const {
    return axesOf(pitch, yaw, roll).up;
}

Vec3 WorldCamera::forward() const {
    return axesOf(pitch, yaw, roll).forward;
}

Mat4 WorldCamera::view() const {
    const Axes axes = axesOf(pitch, yaw, roll);
    // Rows are the camera axes, so eye space keeps the world's handedness: z is depth.
    Mat4 view{1.0f};
    view[0] = Vec4{axes.right.x, axes.up.x, axes.forward.x, 0.0f};
    view[1] = Vec4{axes.right.y, axes.up.y, axes.forward.y, 0.0f};
    view[2] = Vec4{axes.right.z, axes.up.z, axes.forward.z, 0.0f};
    view[3] = Vec4{-glm::dot(axes.right, position), -glm::dot(axes.up, position),
                   -glm::dot(axes.forward, position), 1.0f};
    return view;
}

Mat4 WorldCamera::projection(f32 horizontalFov, f32 aspect) {
    const f32 verticalFov = 2.0f * portable::atan(portable::tan(horizontalFov / 2.0f) / aspect);
    // Swapping the planes reverses depth so that nearer is larger; then it is squeezed under
    // the 2D layers.
    const f32 reversedNear = kFar;
    const f32 reversedFar = kNear;
    const Mat4 reversed = glm::perspectiveLH_ZO(verticalFov, aspect, reversedNear, reversedFar);
    return glm::scale(Mat4{1.0f}, Vec3{1.0f, 1.0f, kDepthRange}) * reversed;
}

ViewVolume ViewVolume::of(const WorldCamera& camera, f32 horizontalFov, f32 aspect) {
    ViewVolume volume;
    volume.position = camera.position;
    volume.right = camera.right();
    volume.up = camera.up();
    volume.forward = camera.forward();
    const f32 halfX = horizontalFov / 2.0f;
    const f32 halfY = portable::atan(portable::tan(halfX) / aspect);
    volume.sinX = portable::sin(halfX);
    volume.cosX = portable::cos(halfX);
    volume.sinY = portable::sin(halfY);
    volume.cosY = portable::cos(halfY);
    return volume;
}

bool ViewVolume::sees(const Vec3& centre, f32 radius) const {
    const Vec3 offset = centre - position;
    const f32 depth = glm::dot(offset, forward);
    if (depth < WorldCamera::kNear - radius) {
        return false;
    }
    // Each side plane passes through the eye; the distance past it must be under the radius.
    const f32 across = glm::dot(offset, right);
    const f32 upward = glm::dot(offset, up);
    const f32 sideBound = (depth * sinX) + radius;
    const f32 heightBound = (depth * sinY) + radius;
    return std::abs(across * cosX) <= sideBound && std::abs(upward * cosY) <= heightBound;
}

Mat4 WorldCamera::frameMapping(f32 frameWidth, f32 frameHeight) {
    const Mat4 mapping =
        glm::translate(Mat4{1.0f}, Vec3{frameWidth / 2.0f, frameHeight / 2.0f, 0.0f});
    return glm::scale(mapping, Vec3{frameWidth / 2.0f, -frameHeight / 2.0f, 1.0f});
}

Mat4 WorldCamera::clipTransform(f32 horizontalFov, f32 frameWidth, f32 frameHeight,
                                const Mat4& frameProjection) const {
    return frameProjection * frameMapping(frameWidth, frameHeight) *
           projection(horizontalFov, frameWidth / frameHeight) * view();
}

CameraFrame CameraFrame::of(const WorldCamera& camera) {
    CameraFrame frame;
    frame.position = camera.position;
    frame.right = camera.right();
    frame.up = camera.up();
    frame.forward = camera.forward();
    return frame;
}

CameraFrame CameraFrame::at(const Vec3& eye) {
    CameraFrame frame;
    frame.position = eye;
    return frame;
}

Mat4 CameraFrame::along(const Mat4& placement, const Vec3& direction) const {
    const f32 length = glm::length(direction);
    if (length < 0.0001f) {
        return placement;
    }
    const Vec3 z = direction / length;
    Vec3 y = glm::cross(z, forward);
    if (glm::length(y) < 0.0001f) {
        // Collinear travel/view has no unique roll. Retain a stable frame
        // instead of normalizing a zero vector.
        y = glm::cross(z, up);
        if (glm::length(y) < 0.0001f) {
            y = glm::cross(z, right);
        }
    }
    y = glm::normalize(y);
    Mat4 result = placement;
    result[0] = Vec4{glm::cross(y, z) * glm::length(Vec3{placement[0]}), 0};
    result[1] = Vec4{y * glm::length(Vec3{placement[1]}), 0};
    result[2] = Vec4{z * glm::length(Vec3{placement[2]}), 0};
    return result;
}

Mat4 CameraFrame::face(const Mat4& placement, u32 mode) const {
    if (mode == 0) {
        return placement;
    }
    Mat4 faced = placement;
    const Vec3 at{placement[3]};
    if (mode == kFacingProjectedYaw) {
        // Legends N64 e002754c/e001e640 use the centre in view space, not its
        // world-XZ bearing. e001e488 adds pi to camera-forward yaw; e001e640
        // adds another pi, cancelling modulo a turn. e0013a70 then rotates
        // only the X/Z basis columns, preserving the Y axis and placement.
        const Vec3 offset = at - position;
        const f32 eyeX = glm::dot(right, offset);
        const f32 eyeZ = glm::dot(forward, offset);
        if (!std::isfinite(eyeX) || !std::isfinite(eyeZ) || !std::isfinite(forward.x) ||
            !std::isfinite(forward.z) || !std::isfinite(placement[0].z) ||
            !std::isfinite(placement[2].z)) {
            return placement;
        }
        const auto angle = [](f32 x, f32 z) {
            return x == 0.0f && z == 0.0f ? 0.0f : std::atan2(x, z);
        };
        const f32 turn =
            std::remainder(angle(-eyeX, -eyeZ) + angle(placement[0].z, placement[2].z) +
                               angle(forward.x, forward.z),
                           kTwoPi);
        if (std::abs(turn) < 0.0001f) {
            return placement;
        }
        const f32 sine = std::sin(turn);
        const f32 cosine = std::cos(turn);
        for (int row = 0; row < 3; ++row) {
            faced[0][row] = placement[0][row] * cosine - placement[2][row] * sine;
            faced[2][row] = placement[2][row] * cosine + placement[0][row] * sine;
        }
        return faced;
    }
    if (mode == kFacingTop) {
        // An XZ ribbon keeps its length and direction. Only its width rolls to
        // put the top surface toward the eye; facing its z at the eye collapses
        // it edge-on and destroys the directions of a radial lightning burst.
        const Vec3 along{placement[2]};
        const Vec3 side = glm::cross(position - at, along);
        constexpr f32 kTopFaceThreshold = 0.01f;
        const f32 length = glm::length(side);
        if (length >= kTopFaceThreshold) {
            faced[0] = Vec4{side / length, 0.0f};
            faced[1] = Vec4{glm::cross(along, Vec3{faced[0]}), 0.0f};
        } else {
            // The original's collinear-eye fallback, also safe for a collapsed
            // animation key: no normalization of a zero-length axis.
            faced[1] = Vec4{0, 1, 0, 0};
            faced[0] = Vec4{glm::cross(Vec3{0, 1, 0}, along), 0.0f};
        }
        return faced;
    }
    // Facing replaces orientation, not the authored growth of a billboard effect.
    const Vec3 scale{glm::length(Vec3{placement[0]}), glm::length(Vec3{placement[1]}),
                     glm::length(Vec3{placement[2]})};
    if (mode == kFacingFull) {
        // A proper rotation whose z points back at the camera.
        faced[0] = Vec4{-right * scale.x, 0.0f};
        faced[1] = Vec4{up * scale.y, 0.0f};
        faced[2] = Vec4{-forward * scale.z, 0.0f};
        return faced;
    }
    const Vec3 toCamera = position - at;
    if (toCamera.x * toCamera.x + toCamera.z * toCamera.z < 1e-8f) {
        return placement;
    }
    const f32 yaw = portable::atan2(toCamera.x, toCamera.z);
    const f32 c = portable::cos(yaw);
    const f32 s = portable::sin(yaw);
    faced[0] = Vec4{c * scale.x, 0.0f, -s * scale.x, 0.0f};
    faced[1] = Vec4{0.0f, scale.y, 0.0f, 0.0f};
    faced[2] = Vec4{s * scale.z, 0.0f, c * scale.z, 0.0f};
    return faced;
}


ViewSet ViewSet::of(const ViewVolume& eye, const Vec3& attention) {
    ViewSet views;
    views.add(eye, attention);
    return views;
}

void ViewSet::add(const ViewVolume& eye, const Vec3& attention) {
    if (count >= kMost) {
        return;
    }
    eyes[count] = eye;
    attentions[count] = attention;
    ++count;
}

bool ViewSet::sees(const Vec3& centre, f32 radius, f32 reach) const {
    for (u32 i = 0; i < std::min<u32>(count, kMost); ++i) {
        if (!eyes[i].sees(centre, radius)) {
            continue;
        }
        if (reach <= 0.0f) {
            return true;
        }
        const Vec3 away = centre - attentions[i];
        if (glm::dot(away, away) <= reach * reach) {
            return true;
        }
    }
    return false;
}

bool ViewSet::bounded(f32 magnitude) const {
    const auto finite = [&](const Vec3& v) {
        for (s32 axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(v[axis]) || std::abs(v[axis]) > magnitude) {
                return false;
            }
        }
        return true;
    };
    const auto unit = [](f32 v) { return std::isfinite(v) && v >= -1.0f && v <= 1.0f; };
    if (count > kMost) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        const ViewVolume& eye = eyes[i];
        if (!finite(eye.position) || !finite(eye.right) || !finite(eye.up) ||
            !finite(eye.forward) || !unit(eye.sinX) || !unit(eye.cosX) || !unit(eye.sinY) ||
            !unit(eye.cosY) || !finite(attentions[i])) {
            return false;
        }
    }
    return true;
}

} // namespace gdl
