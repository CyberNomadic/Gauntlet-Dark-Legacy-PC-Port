#include "game/effects/CritterCueEmission.h"

#include <algorithm>

namespace gdl::game {
CritterCueEmission CritterCueEmission::resolve(const CombatCue& cue, ItemArchive* archive,
                                               ItemArchive& weapons) {
    CritterCueEmission result;
    result.position = cue.position;
    if (cue.tree.empty()) {
        return result;
    }
    if (archive == nullptr || !archive->trees.find(cue.tree)) {
        archive = weapons.loaded() && weapons.trees.find(cue.tree) ? &weapons : nullptr;
    }
    result.archive = archive;
    result.tree = cue.tree;
    result.setting.scale = cue.scale;
    result.setting.yaw = cue.yaw;
    result.setting.seconds = cue.life;
    result.setting.loop = cue.loop;
    return result;
}

std::optional<EffectTrees::Prepared>
CritterCueEmission::prepare(std::span<const EffectTrees::PoolResource> resources,
                            std::span<TextureSet* const> lenders) const {
    if (archive == nullptr) {
        return std::nullopt;
    }
    const auto index = archive->trees.find(tree);
    if (!index) {
        return std::nullopt;
    }
    const auto* nativeTree = &archive->trees.tree(*index);
    for (const auto& resource : resources) {
        const auto* prototype = resource.prototype;
        if (resource.id == 0 || resource.archive != archive || resource.textures == nullptr ||
            prototype == nullptr || prototype->archive != archive ||
            prototype->tree != nativeTree || !std::ranges::equal(resource.lenders, lenders) ||
            !std::ranges::equal(prototype->lenders, lenders) || !prototype->emitParticles ||
            prototype->missingObjectsAreEmpty || prototype->trails.size() != 0) {
            continue;
        }
        EffectTrees::Prepared prepared{
            archive,           nativeTree,      prototype->model.get(), &prototype->particles,
            resource.textures, resource.lenders};
        prepared.device = prototype->device;
        return prepared;
    }
    return std::nullopt;
}
} // namespace gdl::game
