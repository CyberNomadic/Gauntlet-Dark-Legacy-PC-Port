#pragma once
#include <functional>

#include "game/menu/OptionMenu.h"

namespace gdl::game {
/** Per-player editor. Captures only the selected device, after the opening press is released. */
class ControlSettings {
public:
    /** What an update did: the event to be heard (a move, a choice, a step back) and whether
     * the list of players closed. */
    struct Result {
        MenuEvent event;
        bool closed = false;
    };
    void begin(const GameConfig& config);
    void define(MenuDefinition& definition, const TextPainter& painter,
                const std::function<std::string(std::string_view)>& text) const;
    /** Edits are committed transactionally. */
    Result update(const MenuInput& input, s32 ticks, OptionMenu& menu, GameConfig& config,
                  const std::function<bool(const GameConfig&)>& persist,
                  const std::function<void(s32)>& rebuild);
    /** Whether a player's controls are open, a level below the list of players. */
    bool editing() const { return m_player >= 0; }

private:
    std::vector<PlayerControlConfig> choices(const GameConfig& config) const;
    Input m_devices;
    GameConfig m_draft;
    GameConfig m_navigation;
    s32 m_player = -1;
    s32 m_page = 0;
    s32 m_capture = -1;
    s32 m_timeout = 0;
    bool m_released = false;
    bool m_failed = false;
};
} // namespace gdl::game
