#pragma once

#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {
/** CRITTER descriptor families, distinct from level enemy/boss encounter ids. */
// NOLINTNEXTLINE(performance-enum-size): Keep imported signed IDs intact.
enum class CombatantKind : s32 { Unknown = 0, Golem = 3, Boss = 4, Gargoyle = 7, General = 8 };

/** The kind's word, as the developer console names it. */
constexpr std::string_view combatantKindName(CombatantKind kind) {
    switch (kind) {
    case CombatantKind::Golem: return "golem";
    case CombatantKind::Boss: return "boss";
    case CombatantKind::Gargoyle: return "gargoyle";
    case CombatantKind::General: return "general";
    case CombatantKind::Unknown: break;
    }
    return "critter";
}
} // namespace gdl::game
