#include "game/menu/MainMenuFactory.h"

#include <algorithm>

namespace gdl::game {

std::pair<s32, f32> MainMenuFactory::place(s32 rows, s32 lineHeight, s32 height) {
    const s32 column = std::max(0, rows) * std::max(0, lineHeight);
    const s32 room = std::max(0, height - kBottomMargin - kHighestTop);
    f32 scale = 1.0f;
    if (column > room && column > 0) {
        scale = static_cast<f32>(room) / static_cast<f32>(column);
    }
    const auto scaled = static_cast<s32>(static_cast<f32>(column) * scale);
    const s32 top = std::max(kHighestTop, std::min(kRowTop, height - kBottomMargin - scaled));
    return {top, scale};
}

MenuDefinition MainMenuFactory::build(std::span<const MainMenuEntry> entries,
                                      const TextPainter& painter, const MenuScreen& screen) {
    MenuDefinition menu;
    const auto [top, scale] =
        place(static_cast<s32>(entries.size()), painter.lineHeight(1.0f), screen.height);
    menu.x = -(screen.width / 2);
    menu.y = top;
    menu.scale = scale;
    menu.cursorScale = scale;
    menu.fitWidth = screen.width; // and no row runs off the screen's sides
    menu.startSelects = true;
    for (const MainMenuEntry& entry : entries) {
        menu.items.push_back({entry.text, entry.code, 0, entry.enabled});
    }
    return menu;
}

} // namespace gdl::game
