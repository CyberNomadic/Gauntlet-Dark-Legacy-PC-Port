#include "game/screens/OrdinaryEnemyMissilePhase.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "game/screens/LevelOpponents.h"

namespace gdl::game {
namespace {
bool ordinaryHit(const EnemyMissileHit& hit) {
    constexpr u32 kFlags = 0xF | EnemyMissileKind::kKnockBack | EnemyMissileKind::kKnockDown |
                           EnemyMissileKind::kArrowHit | EnemyMissileKind::kBoltHit;
    const auto bounded = [](f32 value) {
        return std::isfinite(value) && std::abs(value) <= 1'000'000;
    };
    const auto point = [&](const Vec3& value) {
        return bounded(value.x) && bounded(value.y) && bounded(value.z);
    };
    return hit.player >= -1 && hit.player < 4 && hit.shooter >= -1 &&
           hit.shooter < Enemies::kMost && hit.target == -1 && hit.worldObject >= -1 &&
           hit.worldObject <= 1'000'000 && bounded(hit.damage) && hit.damage >= 0 &&
           point(hit.position) && point(hit.direction) && hit.burstRadius == 0 && !hit.fromBurst &&
           !hit.ricochet && (hit.flags & ~kFlags) == 0 && (hit.flags & 0xF) <= 4;
}
} // namespace

EnemyMissileEmission EnemyMissileEmission::resolve(const EnemyMissileHit& hit,
                                                   ItemArchive& weapons) {
    EnemyMissileEmission result;
    result.position = hit.position;
    if (hit.ricochet) {
        result.sound = "S_RICOCHET";
        result.level = 127;
        return result;
    }
    const auto tree = hit.effect();
    if (!tree.empty() && weapons.loaded() && weapons.trees.find(tree)) {
        result.archive = &weapons;
        result.tree = tree;
    }
    result.setting.unlit = true;
    result.setting.depthWrite = false;
    if (hit.burstRadius <= 0) {
        result.setting.tint.a = 96;
    }
    result.sound = hit.sound();
    result.level = hit.liquid ? LevelSoundscape::kSplashLevel : 224;
    return result;
}

std::optional<EffectTrees::Prepared>
EnemyMissileEmission::prepare(std::span<const EffectTrees::PoolResource> resources,
                              std::span<TextureSet* const> lenders,
                              const RenderDevice& device) const {
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
        if (resource.id == 0 || resource.archive != archive || resource.textures == nullptr ||
            prototype == nullptr || !std::ranges::equal(resource.lenders, lenders) ||
            !std::ranges::equal(prototype->lenders, lenders) || prototype->archive != archive ||
            prototype->tree != nativeTree || prototype->device != &device ||
            !prototype->emitParticles || prototype->missingObjectsAreEmpty ||
            prototype->trails.size() != 0) {
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

bool OrdinaryEnemyMissilePhase::supported(const EnemyMissiles::Checkpoint& state) {
    return state.bursts.empty() && state.rockHits.empty() && state.pickupBlasts.empty() &&
           state.hits.size() <= kMaxImpacts && std::ranges::all_of(state.hits, ordinaryHit);
}

OrdinaryEnemyMissilePhase::Result
OrdinaryEnemyMissilePhase::advance(LevelOpponents& owner, std::span<PlayerRuntime> party,
                                   PlayerHealth& health, const ClassDataSet& classes,
                                   const PartyDamage::Context& context,
                                   std::span<const EffectTrees::PoolResource> resources) {
    if (!owner.m_resources || party.size() > 4 || !context.valid() ||
        owner.m_enemyMissiles.burstCount() != 0) {
        return {Refusal::Binding};
    }
    auto& binding = *owner.m_resources;
    auto& effects = binding.effects;
    // Taking this queue belongs to the caller's complete undo. Check the entire
    // batch before the first native dispatch, not just the next visible hit.
    const auto hits = owner.m_enemyMissiles.takeHits();
    if (hits.size() > kMaxImpacts || !std::ranges::all_of(hits, ordinaryHit)) {
        return {Refusal::Hit};
    }
    for (const auto& hit : hits) {
        if (hit.worldObject >= 0 && binding.world.wouldExplodeObject(hit.worldObject)) {
            return {Refusal::WorldExplosion};
        }
        const auto emission = EnemyMissileEmission::resolve(hit, binding.weapons);
        if (emission.archive &&
            !emission.prepare(resources, effects.textureLenders(), binding.device)) {
            return {Refusal::Resources};
        }
    }
    Result result;
    bool refused = false;
    const LevelOpponents::Events events{.hurt = [&](usize index, f32 amount, HurtKind kind,
                                                    bool directed, const PlayerImpact& impact) {
        auto damage = PartyDamage::stageHit(party[index], party, health, classes, amount, kind,
                                            directed, context, impact);
        refused |= damage.status != PartyDamage::Status::Applied;
        result.damage.insert(result.damage.end(), std::make_move_iterator(damage.outputs.begin()),
                             std::make_move_iterator(damage.outputs.end()));
    }};
    for (const auto& hit : hits) {
        if (!owner.dispatchEnemyMissileHit(
                hit, party, events,
                [&](const EnemyMissileEmission& emission) -> std::optional<u32> {
                    u32 effect = 0;
                    if (emission.archive) {
                        const auto prepared =
                            emission.prepare(resources, effects.textureLenders(), binding.device);
                        if (!prepared) {
                            return std::nullopt;
                        }
                        effect =
                            effects.startPrepared(*prepared, emission.position, emission.setting);
                        if (effect == 0) {
                            return std::nullopt;
                        }
                    }
                    const f32 volume =
                        emission.level / 255 *
                        LevelOpponents::attenuation(emission.position, owner.m_hearers);
                    if (!emission.sound.empty() && volume > 0) {
                        result.sounds.push_back(
                            {std::string{emission.sound}, emission.position, volume});
                    }
                    return effect;
                })) {
            return {Refusal::Resources};
        }
        if (refused) {
            return {Refusal::Damage};
        }
        ++result.impacts;
    }
    return result;
}
} // namespace gdl::game
