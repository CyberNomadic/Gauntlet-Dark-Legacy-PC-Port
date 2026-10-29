#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "engine/ui/TextPainter.h"

#include "game/config/GameConfig.h"

namespace gdl::game {
using ControlLabels = std::function<std::string(s32, std::string_view)>;

/** The assigned action's primary binding, including derived potion gestures. */
std::string boundControlLabel(const GameConfig& config, s32 player, bool keyboard,
                              std::string_view action);
std::string controlLabel(const ControlLabels& labels, s32 player, std::string_view action);
/** Expand explicit action tokens only; ordinary letters and asset names are never rewritten. */
std::string controlText(std::string_view text, const ControlLabels& labels, s32 player);
void drawControlLabel(Canvas& canvas, const TextPainter& painter, const Rect& area,
                      std::string_view label, Color color = Color::white());

/** Remembers keyboard/pad activity separately for each player's automatic device profile. */
class PromptDevices {
public:
    void update(const Input& input, const GameConfig& config);
    std::string label(const Input& input, const GameConfig& config, s32 player,
                      std::string_view action) const;
    s32 lastPlayer() const { return m_lastPlayer; }
    /** Whether the pointer is out of the way: the last input was a pad's and the keyboard
     * and mouse belong to an automatic profile, so they return it when they are used. */
    bool cursorHidden() const { return m_cursorHidden; }

private:
    std::array<bool, 4> m_keyboard{true, false, false, false};
    std::optional<Vec2> m_pointer;
    s32 m_lastPlayer = 0;
    bool m_cursorHidden = false;
};
} // namespace gdl::game
