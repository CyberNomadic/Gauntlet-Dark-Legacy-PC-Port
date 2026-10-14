#pragma once

#include "game/enemies/CombatEvents.h"
#include "game/world/EffectTrees.h"

namespace gdl::game {
/** Resolved native move artwork. The archive and ordered lenders are borrowed
 * from a preloaded level; preparing an emission never loads or uploads art. */
struct CritterCueEmission {
    ItemArchive* archive = nullptr;
    std::string_view tree;
    Vec3 position{0};
    EffectTrees::Setting setting;

    static CritterCueEmission resolve(const CombatCue&, ItemArchive*, ItemArchive& weapons);
    std::optional<EffectTrees::Prepared> prepare(std::span<const EffectTrees::PoolResource>,
                                                 std::span<TextureSet* const>) const;
};
} // namespace gdl::game
