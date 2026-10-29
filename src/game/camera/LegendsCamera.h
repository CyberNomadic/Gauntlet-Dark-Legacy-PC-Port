#pragma once

#include "engine/world/WorldCamera.h"

#include "formats/LegendsN64Camera.h"

namespace gdl::game {

/** Single-player native Legends follow camera. Consumes actor feet, not a
 * pre-offset follow point. Scripted ceremonies and multiplayer fitting are not
 * part of the imported-map exploration scene. */
class LegendsCamera {
public:
    enum class Mode { Markers, TowerInner, TowerMiddle, TowerOuter };

    void reset(formats::LegendsN64CameraData data, const Vec3& feet);
    void update(const Vec3& feet, f32 seconds);
    const WorldCamera& camera() const { return m_camera; }
    /** Rendering-only native 30 Hz interpolation; extraSeconds is the caller's
     * remaining fractional simulation step. Never feed this into movement. */
    WorldCamera sample(f32 extraSeconds = 0) const;
    const Vec3& attention() const { return m_attention; }
    f32 distance() const { return m_distance; }
    Mode mode() const { return m_mode; }
    s32 marker() const { return m_marker; }

private:
    void tick(const Vec3& feet);
    Vec3 target(const Vec3& feet, f32& pitch, f32& yaw);
    void place();

    formats::LegendsN64CameraData m_data;
    WorldCamera m_camera;
    WorldCamera m_previousCamera;
    Vec3 m_attention{0};
    Vec3 m_smoothed{0};
    Vec3 m_velocity{0};
    Vec3 m_previousTarget{0};
    f32 m_distance = 36;
    f32 m_distanceVelocity = 0;
    f32 m_yawVelocity = 0;
    f32 m_pitchVelocity = 0;
    f32 m_accumulator = 0;
    Mode m_mode = Mode::Markers;
    s32 m_marker = -1;
    bool m_transition = false;
};

} // namespace gdl::game
