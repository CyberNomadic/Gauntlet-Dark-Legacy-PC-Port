#pragma once
#include <optional>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {

/**
 * A camera let off the party for looking a level over, for the developer console: it starts
 * where the level's camera stood and is flown with W, A, S and D along its own axes, Space
 * and Ctrl straight up and down, Shift hurrying it, and turned with the arrow keys or a
 * dragged pointer. The level's camera is not moved; what it framed goes on.
 */
class FreeCamera {
public:
    static constexpr f32 kSpeed = 40.0f;                   ///< units a second
    static constexpr f32 kFastFactor = 4.0f;               ///< with Shift held
    static constexpr f32 kTurnRate = glm::radians(120.0f); ///< a second, by the arrow keys
    static constexpr f32 kDragTurn = 0.004f;               ///< radians a pointer pixel
    static constexpr f32 kPitchLimit = glm::radians(89.0f);
    static constexpr std::string_view kHelp =
        "W A S D fly, Space and Ctrl rise and sink, arrows or a dragged pointer turn, Shift "
        "hurries";

    /** Takes off from `from`. */
    void start(const WorldCamera& from);
    void stop();
    bool active() const { return m_active; }
    const WorldCamera& camera() const { return m_camera; }

    /** Flies by the keys held and the pointer dragged since the last poll; nothing when off. */
    void update(const Input& input, f32 seconds);

private:
    bool m_active = false;
    WorldCamera m_camera;
    std::optional<Vec2> m_dragFrom; ///< where the held pointer was last seen
};

} // namespace gdl::game
