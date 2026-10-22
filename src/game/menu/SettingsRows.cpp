#include "game/menu/SettingsRows.h"

#include <algorithm>
#include <utility>

namespace gdl::game {
SettingsRows::SettingsRows(MenuDefinition& definition, const TextPainter& painter, Text text)
    : m_definition(definition), m_painter(painter), m_text(std::move(text)) {}

MenuItem& SettingsRows::choice(std::string label, s32 code) {
    m_definition.items.push_back({std::move(label), code});
    return m_definition.items.back();
}

MenuItem& SettingsRows::marked(std::string label, s32 code, bool marked) {
    MenuItem& item = choice(std::move(label), code);
    item.markedPart = marked ? 1 : 0;
    return item;
}

MenuItem& SettingsRows::value(std::string label, s32 code, std::string value, bool enabled) {
    MenuItem& item = choice(std::move(label), code);
    item.enabled = enabled;
    item.value = std::move(value);
    return item;
}

MenuItem& SettingsRows::toggle(std::string label, s32 code, bool on) {
    return value(std::move(label), code, m_text(on ? "settings.on" : "settings.off"));
}

void SettingsRows::column(s32 x, s32 y, f32 scale) {
    m_definition.x = x;
    m_definition.y = y;
    m_definition.scale = scale;
    m_definition.cursorScale = scale;
}

void SettingsRows::switches(s32 x) {
    m_definition.valueX = x;
    m_definition.valueWidth =
        std::max(m_painter.measure(m_text("settings.on"), m_definition.scale),
                 m_painter.measure(m_text("settings.off"), m_definition.scale));
}

void SettingsRows::fit(f32 most, s32 width, std::string_view tail) {
    const s32 left = m_definition.x < 0 ? kMargin : m_definition.x;
    s32 longest = 1;
    for (const MenuItem& item : m_definition.items) {
        longest = std::max(longest, m_painter.measure(item.text + std::string(tail), 1));
    }
    const auto available = static_cast<f32>(std::max(1, width - left - kMargin));
    m_definition.scale = std::min(most, available / static_cast<f32>(longest));
    m_definition.cursorScale = m_definition.scale;
}
} // namespace gdl::game
