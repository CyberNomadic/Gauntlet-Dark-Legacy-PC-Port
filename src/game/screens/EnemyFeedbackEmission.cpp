#include "game/screens/EnemyFeedbackEmission.h"

#include <algorithm>

namespace gdl::game {

EnemyFeedbackEmission EnemyFeedbackEmission::resolve(const EnemyFeedback& feedback,
                                                     ItemArchive* enemyArchive,
                                                     ItemArchive& weapons, const LevelInfo* level) {
    EnemyFeedbackEmission result;
    result.position = feedback.position;
    if (level != nullptr) {
        result.sound = feedback.sound(level->enemies, level->bossType);
    }
    const auto tree = feedback.effect();
    if (tree.empty()) {
        return result;
    }
    auto* archive = enemyArchive;
    if (archive == nullptr || !archive->trees.find(tree)) {
        archive = &weapons;
    }
    if (!archive->trees.find(tree)) {
        return result;
    }
    result.archive = archive;
    result.tree = tree;
    result.setting.scale = feedback.effectScale();
    result.setting.yaw = feedback.yaw;
    // Native common impacts set node alpha to 96. The two tree-creature
    // effects retain the archive's own opacity instead.
    if (tree != "TREEHIT" && tree != "TREEDIE") {
        result.setting.tint.a = 96;
    }
    return result;
}

std::optional<EffectTrees::Prepared>
EnemyFeedbackEmission::prepare(std::span<const EffectTrees::PoolResource> resources,
                               std::span<TextureSet* const> lenders) const {
    if (archive == nullptr || tree.empty()) {
        return std::nullopt;
    }
    const auto index = archive->trees.find(tree);
    if (!index) {
        return std::nullopt;
    }
    const auto* nativeTree = &archive->trees.tree(*index);
    for (const auto& resource : resources) {
        const auto* prototype = resource.prototype;
        if (resource.archive != archive || resource.textures == nullptr || prototype == nullptr ||
            !std::ranges::equal(resource.lenders, lenders) || prototype->archive != archive ||
            prototype->tree != nativeTree || !prototype->emitParticles ||
            prototype->missingObjectsAreEmpty || prototype->trails.size() != 0) {
            continue;
        }
        EffectTrees::Prepared prepared{archive,           nativeTree,
                                       &prototype->model, &prototype->particles,
                                       resource.textures, resource.lenders};
        prepared.device = prototype->device;
        return prepared;
    }
    return std::nullopt;
}

} // namespace gdl::game
