#pragma once

#include "game/critters/CombatEvents.h"
#include "game/effects/EffectTrees.h"

namespace gdl::game {
/** Resolved native move artwork. The archive and ordered lenders are borrowed
 * from a preloaded level; preparing an emission never loads or uploads art. */
struct CritterCueEmission {
    ItemArchive* archive = nullptr;
    std::string_view tree;
    Vec3 position{0};
    EffectTrees::Setting setting;

    static CritterCueEmission resolve(const CombatCue& /*cue*/, ItemArchive* /*archive*/,
                                      ItemArchive& weapons);
    std::optional<EffectTrees::Prepared>
        prepare(std::span<const EffectTrees::PoolResource> /*resources*/,
                std::span<TextureSet* const> /*lenders*/) const;
};
} // namespace gdl::game
