#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "game/enemies/EnemyFeedback.h"
#include "game/world/EffectTrees.h"

namespace gdl::game {

/** Native swarm impact presentation, resolved before its actor/slot can be
 * recycled. Resolution never starts an effect, plays sound, or awards damage.
 * Archives remain borrowed from the loaded stage, not selected by packet text. */
struct EnemyFeedbackEmission {
    std::string sound;
    ItemArchive* archive = nullptr; ///< null when native artwork is unavailable
    std::string_view tree;
    Vec3 position{0};
    EffectTrees::Setting setting;

    static EnemyFeedbackEmission resolve(const EnemyFeedback& feedback, ItemArchive* enemyArchive,
                                         ItemArchive& weapons, const LevelInfo* level);

    /** Finds the same tree and ordered native lender context in a trusted
     * preloaded catalog. Missing bindings refuse replay; they never substitute
     * another archive, suppress an available effect, or load artwork mid-tick.
     * An emission with archive == nullptr needs no visual preparation. */
    std::optional<EffectTrees::Prepared>
    prepare(std::span<const EffectTrees::PoolResource> resources,
            std::span<TextureSet* const> lenders) const;
};

} // namespace gdl::game
