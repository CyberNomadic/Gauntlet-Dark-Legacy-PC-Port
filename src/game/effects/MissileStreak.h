#pragma once

#include "engine/core/Types.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {

/** SfxSetStreak / UpdateFXStreak: a velocity-aligned, camera-facing textured quad.
 * The borrowed texture outlives the missile. No texture means no streak. */
struct MissileStreak {
    const Texture* texture = nullptr;
    Color color = Color::white();
    f32 forward = 0.0f;
    template <class D> friend void visitNativeState(D& digest, const MissileStreak& value) {
        const auto& [texture, color, forward] = value;
        digest.resource(texture);
        digest.values(color, forward);
    }

    ImmediateBatch geometry(const Vec3& position, const Vec3& velocity, f32 age, f32 scale,
                            const CameraFrame& camera) const;
};

} // namespace gdl::game
