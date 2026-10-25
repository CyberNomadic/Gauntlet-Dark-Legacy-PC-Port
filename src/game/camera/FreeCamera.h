#pragma once
#include <optional>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/world/WorldCamera.h"

#include "game/camera/FlightVelocity.h"

namespace gdl::game {

/**
 * A camera let off the party for looking a level over, for the developer console: it starts
 * where the level's camera stood and is flown with W, A, S and D along its own axes, Q and E
 * straight up and down, Shift slowing it, its velocity easing in and out the way a Source
 * engine noclip flier's does, and turned by the pointer while the right mouse button is
 * held. The level's camera is not moved; what it framed goes on.
 */
class FreeCamera {
public:
    static constexpr f32 kSpeed = 40.0f;      ///< units a second
    static constexpr f32 kSlowFactor = 0.25f; ///< with Shift held
    static constexpr f32 kDragTurn = kPi;     ///< radians a drag across the whole view turns
    static constexpr f32 kPitchLimit = glm::radians(89.0f);
    static constexpr std::string_view kHelp =
        "W A S D fly, Q and E rise and sink, the right mouse button held turns, Shift slows";

    /** Takes off from `from`, at rest. */
    void start(const WorldCamera& from);
    void stop();
    bool active() const { return m_active; }
    const WorldCamera& camera() const { return m_camera; }
    const Vec3& velocity() const { return m_velocity.velocity; }

    /** Flies by the keys held and the pointer dragged since the last poll; nothing when off. */
    void update(const Input& input, f32 seconds);

private:
    bool m_active = false;
    WorldCamera m_camera;
    FlightVelocity m_velocity;
    std::optional<Vec2> m_dragFrom; ///< where the pointer was last seen, the button held
};

} // namespace gdl::game
