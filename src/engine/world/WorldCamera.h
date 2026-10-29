#pragma once

#include <array>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/**
 * A camera placed in a level with the original's pitch, yaw and roll convention, in its
 * left-handed world (x right, y up, z away). Its projection keeps depth within the range
 * under the 2D layers, so screens can draw sprites over the world without clearing depth.
 */
struct WorldCamera {
    bool operator==(const WorldCamera&) const = default;
    template <class Visitor>
    friend void visitNativeState(Visitor& visitor, const WorldCamera& value) {
        visitor.template fields<4>(value);
    }

    static constexpr f32 kNear = 0.25f;
    static constexpr f32 kFar = 2000.0f;
    static constexpr f32 kDepthRange = 0.45f; ///< world depths stay below the canvas' 0.5

    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 pitch = 0.0f; ///< radians, positive looks down
    f32 yaw = 0.0f;
    f32 roll = 0.0f;

    /** Camera axes in world space: right, up and the direction looked along. */
    Vec3 right() const;
    Vec3 up() const;
    Vec3 forward() const;

    /** World to eye space; eye z is the distance ahead. */
    Mat4 view() const;
    /** Drawing-only sample between two completed states; caller bypasses this for cuts. */
    WorldCamera interpolate(const WorldCamera& previous, f32 fraction) const;

    /** Perspective with reversed depth: nearer is larger, within [0, kDepthRange]. */
    static Mat4 projection(f32 horizontalFov, f32 aspect);

    /** Maps clip space onto a frame's pixel space (y down), for the letterbox projection. */
    static Mat4 frameMapping(f32 frameWidth, f32 frameHeight);

    /** Everything from world space to the frame's clip space. */
    Mat4 clipTransform(f32 horizontalFov, f32 frameWidth, f32 frameHeight,
                       const Mat4& frameProjection) const;
};

/**
 * What a camera takes in, for telling whether something is on screen: in front of the eye and
 * inside each side of the view by a margin (the original's world sphere visibility test).
 */
struct ViewVolume {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 forward{0.0f, 0.0f, 1.0f};
    f32 sinX = 0.5f; ///< the sine and cosine of half the view across
    f32 cosX = 0.8660254f;
    f32 sinY = 0.5f; ///< and of half the view up and down
    f32 cosY = 0.8660254f;
    bool operator==(const ViewVolume&) const = default;
    template <class Visitor>
    friend void visitNativeState(Visitor& visitor, const ViewVolume& value) {
        visitor.template fields<8>(value);
    }

    static ViewVolume of(const WorldCamera& camera, f32 horizontalFov, f32 aspect);
    /** Whether a sphere of `radius` about `centre` is at least partly in view. */
    bool sees(const Vec3& centre, f32 radius) const;
};

/**
 * What a party's cameras take in together: up to four views, each with the point its camera
 * looks at, for telling whether something is on anyone's screen. The party's shared camera
 * is one view; under solo cameras each player's own camera is one.
 */
struct ViewSet {
    static constexpr usize kMost = 4;
    std::array<ViewVolume, kMost> eyes{};
    std::array<Vec3, kMost> attentions{};
    u32 count = 0;
    bool operator==(const ViewSet&) const = default;
    template <class Visitor> friend void visitNativeState(Visitor& visitor, const ViewSet& value) {
        visitor.template fields<3>(value);
    }

    static ViewSet of(const ViewVolume& eye, const Vec3& attention);
    /** Adds a view; past `kMost` it is dropped. */
    void add(const ViewVolume& eye, const Vec3& attention);
    /** Whether any view sees the sphere; with `reach` above zero, only a view whose camera
     * looks within `reach` of the centre counts (the original's attention gate). */
    bool sees(const Vec3& centre, f32 radius, f32 reach = 0.0f) const;
    /** Whether every value is finite and within `magnitude`, the sines and cosines within
     * one: what a checkpoint of the views accepts. */
    bool bounded(f32 magnitude) const;
};

/** Where a camera stands and which way its axes point, for what must face it. */
struct CameraFrame {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 forward{0.0f, 0.0f, 1.0f};

    static CameraFrame of(const WorldCamera& camera);
    /** A frame standing at `eye` with the world's axes. */
    static CameraFrame at(const Vec3& eye);

    /** The facing modes an object's flags ask for, in their top nibble. */
    static constexpr u32 kFacingShift = 24U;
    static constexpr u32 kFacingMask = 0xFU;
    static constexpr u32 kFacingFull = 4; ///< the whole rotation follows the camera
    static constexpr u32 kFacingTop = 8;  ///< roll an XZ ribbon toward the eye, preserving its z
    // Adapter-only semantic mode; 9 is not a recovered GameCube/N64 flag value.
    static constexpr u32 kFacingProjectedYaw = 9; ///< yaw using the centre's projected view depth
    static u32 facingOf(u32 objectFlags) { return (objectFlags >> kFacingShift) & kFacingMask; }

    /** Face the camera fully, about a ribbon's authored z (kFacingTop), or about the
     * world vertical for ordinary upright sprites. */
    Mat4 face(const Mat4& placement, u32 mode) const;
    /** Align a travelling effect's z to its velocity, with roll determined by
     * the view direction (CreateDirMatrix). Translation and scale are kept. */
    Mat4 along(const Mat4& placement, const Vec3& direction) const;
};

} // namespace gdl
