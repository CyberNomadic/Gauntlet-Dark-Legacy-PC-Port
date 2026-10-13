#include "game/players/LevelWatch.h"

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl::game {

bool LevelWatch::Checkpoint::valid() const {
    if (marks.size() > 4) {
        return false;
    }
    u32 seen = 0;
    for (const auto& mark : marks) {
        if (mark.player < 0 || mark.player >= 4 || mark.level < 1 || mark.level > 99 ||
            (seen & (1U << mark.player)) != 0) {
            return false;
        }
        seen |= 1U << mark.player;
    }
    return true;
}

bool LevelWatch::restore(const Checkpoint& state) {
    if (!state.valid()) {
        return false;
    }
    auto marks = state.marks;
    m_marks.swap(marks);
    return true;
}

std::optional<LevelChange> LevelWatch::observe(s32 player, s32 level) {
    const auto mark = std::ranges::find(m_marks, player, &Mark::player);
    if (mark == m_marks.end()) {
        m_marks.push_back(Mark{player, level});
        return std::nullopt;
    }
    if (mark->level == level) {
        return std::nullopt;
    }
    LevelChange change;
    change.player = player;
    change.from = mark->level;
    change.to = level;
    mark->level = level;
    return change;
}

void LevelWatch::forget(s32 player) {
    std::erase_if(m_marks, [player](const Mark& mark) { return mark.player == player; });
}

std::optional<s32> LevelWatch::markOf(s32 player) const {
    const auto mark = std::ranges::find(m_marks, player, &Mark::player);
    return mark == m_marks.end() ? std::nullopt : std::optional<s32>(mark->level);
}

} // namespace gdl::game
