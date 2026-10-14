#include "game/screens/OrdinaryWeaponEmission.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "game/screens/OrdinaryProjectilePhase.h"

namespace gdl::game {
namespace {
bool finite(f32 value) {
    return std::isfinite(value) && std::abs(value) <= 1e6f;
}
bool finite(const Vec3& value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

std::optional<EffectTrees::Prepared>
bodyResource(const MissileLaunch& launch, std::span<const EffectTrees::PoolResource> resources,
             std::span<TextureSet* const> fallback) {
    if (launch.archive == nullptr || launch.tree.empty()) {
        return std::nullopt;
    }
    const auto index = launch.archive->trees.find(launch.tree);
    if (!index) {
        return std::nullopt;
    }
    const auto* tree = &launch.archive->trees.tree(*index);
    std::vector<TextureSet*> lenders;
    if (launch.textureLender != nullptr) {
        lenders.push_back(launch.textureLender);
    }
    lenders.insert(lenders.end(), fallback.begin(), fallback.end());
    for (const auto& resource : resources) {
        const auto* prototype = resource.prototype;
        if (resource.id == 0 || resource.archive != launch.archive ||
            resource.textures == nullptr || prototype == nullptr ||
            !std::ranges::equal(resource.lenders, lenders) ||
            !std::ranges::equal(prototype->lenders, lenders) ||
            prototype->archive != launch.archive || prototype->tree != tree ||
            !prototype->emitParticles || prototype->missingObjectsAreEmpty ||
            prototype->trails.size() != 0 ||
            (!prototype->model.bound() && prototype->particles.field().size() == 0)) {
            continue;
        }
        EffectTrees::Prepared prepared{launch.archive,    tree,
                                       &prototype->model, &prototype->particles,
                                       resource.textures, resource.lenders};
        prepared.device = prototype->device;
        return prepared;
    }
    return std::nullopt;
}
} // namespace

OrdinaryWeaponEmission::Result OrdinaryWeaponEmission::advance(
    PlayerArsenal& arsenal, const PlayerActor& actor, PlayerFigure& figure, const Vec3& direction,
    std::optional<Vec3> target, const CombatRollbackCheckpoint::Resources& resources) {
    const auto& animator = figure.animator();
    const auto& save = actor.save();
    const bool strong = animator.strongReleased();
    if (!arsenal.m_resources || actor.player() < 0 || actor.player() >= 4 || save.character < 0 ||
        save.character > 16 || animator.released() == strong || !finite(direction) ||
        glm::length(direction) < 1e-6f || (target && !finite(*target))) {
        return {};
    }
    const auto worn = PowerupEffects::of(save.progress().inventory);
    if ((worn.weapon & ~PlayerMissiles::kPlainModifiers) != 0 ||
        (worn.special & powerup::kPojo) != 0 ||
        resources.missiles.size() > PlayerMissiles::Checkpoint::kMaxResources ||
        resources.missileEffects.size() >
            EffectTrees::PoolCheckpoint::kMaxEffects + EffectTrees::PoolCheckpoint::kMaxBanks) {
        return {};
    }
    // PlayScene::perform uses twice the size and harm for the native strong latch.
    auto prepared =
        arsenal.prepareWeapon(actor, figure, direction, strong ? 2.0f : 1.0f, true, target);
    if (!prepared || prepared->pojo || prepared->missiles.empty() ||
        prepared->missiles.size() > 5 || prepared->weapon.multiplayer != MultiplayerMode::Normal) {
        return {};
    }
    const auto& first = prepared->missiles.front();
    if (first.spec != &MissileSpec::of(save.character) || !finite(first.position)) {
        return {};
    }
    const Vec3 clear = arsenal.weaponMuzzle(first);
    if (!finite(clear)) {
        return {};
    }
    if (glm::distance(clear, first.position) > 1e-4f) {
        // Native launchWeapon supplies only these two fields: SPARKS and the
        // level wall cue, never class/fan impact art or per-projectile provenance.
        const auto impact = arsenal.impactPresentation({.position = clear, .owner = first.owner});
        if (impact.archive != nullptr && impact.archive->trees.find(impact.tree)) {
            const auto effect = OrdinaryProjectilePhase::impactResource(
                impact, resources.sharedEffects, arsenal.m_resources->effects.textureLenders(),
                arsenal.m_resources->device);
            if (!effect || arsenal.m_resources->effects.startPrepared(*effect, impact.position,
                                                                      impact.setting) == 0) {
                return {};
            }
        }
        return {.accepted = true, .impact = impact.cue};
    }
    const PlayerMissiles::Resource* physical = nullptr;
    for (const auto& resource : resources.missiles) {
        if (resource.spec == &MissileSpec::of(save.character) &&
            resource.model == &figure.missile()) {
            // Multiple bindings for this model with different streaks are not
            // an invitation to choose arbitrary artwork by vector order.
            if (physical != nullptr || resource.id == 0) {
                return {};
            }
            physical = &resource;
        }
    }
    if (physical == nullptr ||
        (physical->streak != nullptr) !=
            arsenal.m_resources->weapons.textures.find("WEP_STREAK").has_value()) {
        return {};
    }
    arsenal.bindWeaponAppearance(actor, figure, *prepared,
                                 arsenal.weaponStreak(actor, physical->streak, false));
    std::vector<EffectTrees::Prepared> bodies;
    bodies.reserve(prepared->missiles.size());
    for (usize ordinal = 0; ordinal < prepared->missiles.size(); ++ordinal) {
        const auto& launch = prepared->missiles[ordinal];
        if (launch.spec != physical->spec || launch.model != physical->model ||
            launch.streak.texture != physical->streak || launch.owner != actor.player() ||
            launch.riderArchive != nullptr || !launch.riderTree.empty() || launch.potion != 0 ||
            launch.potionRelease || !launch.release ||
            launch.release->character != save.character || launch.release->strong != strong ||
            launch.release->ordinal != ordinal || launch.release->emission != 0 ||
            launch.release->generation != animator.player().generation() ||
            launch.release->generation == 0 || !finite(launch.position) ||
            !finite(launch.direction) || !finite(launch.speed) || launch.speed <= 0 ||
            !finite(launch.reach) || launch.reach <= 0 || !finite(launch.damage) ||
            (launch.velocity && !finite(*launch.velocity))) {
            return {};
        }
        auto body = bodyResource(launch, resources.missileEffects,
                                 arsenal.missiles().visuals().textureLenders());
        if (!body) {
            return {};
        }
        bodies.push_back(*body);
    }
    const Cue cue{actor.player(), *prepared->missiles.front().release,
                  PlayerArsenal::weaponCue(prepared->soundFlags, figure)};
    for (usize index = 0; index < prepared->missiles.size(); ++index) {
        if (!arsenal.missiles().launchPrepared(prepared->missiles[index], bodies[index])) {
            return {};
        }
    }
    return {.accepted = true, .launched = true, .cue = cue};
}

OrdinaryWeaponEmission::Result
OrdinaryWeaponEmission::familiar(PlayerArsenal& arsenal, const PlayerActor& actor,
                                 PlayerFigure& figure, std::optional<Vec3> target,
                                 const CombatRollbackCheckpoint::Resources& resources) {
    if (!arsenal.m_resources || !figure.familiarReleased() || actor.player() < 0 ||
        actor.player() >= 4 || figure.pojoActive() || (target && !finite(*target)) ||
        resources.missiles.size() > PlayerMissiles::Checkpoint::kMaxResources ||
        resources.missileEffects.size() >
            EffectTrees::PoolCheckpoint::kMaxEffects + EffectTrees::PoolCheckpoint::kMaxBanks) {
        return {};
    }
    const auto launches = arsenal.prepareFamiliar(actor, figure, target);
    if (!launches || launches->size() > 2) {
        return {};
    }
    std::vector<EffectTrees::Prepared> bodies;
    bodies.reserve(launches->size());
    for (const auto& launch : *launches) {
        const auto matches = std::ranges::count_if(resources.missiles, [&](const auto& resource) {
            return resource.id != 0 && resource.spec == launch.spec &&
                   resource.model == launch.model && resource.streak == nullptr;
        });
        if (matches != 1 || launch.model == nullptr || !launch.model->bound() ||
            launch.spec != &PlayerArsenal::familiarSpec(arsenal.m_resources->bossEncounter) ||
            launch.owner != actor.player() || launch.release || launch.potionRelease ||
            launch.potion != 0 || launch.riderArchive != nullptr || !launch.riderTree.empty() ||
            launch.streak.texture != nullptr || launch.multiplayer != MultiplayerMode::Normal ||
            launch.breaksPotions || launch.wallSound != MissileWallSound::Silent ||
            !finite(launch.position) || !finite(launch.direction) || !launch.velocity ||
            !finite(*launch.velocity) || !finite(launch.damage) || launch.damage < 0) {
            return {};
        }
        const auto body = bodyResource(launch, resources.missileEffects,
                                       arsenal.missiles().visuals().textureLenders());
        if (!body || body->device != &arsenal.m_resources->device) {
            return {};
        }
        bodies.push_back(*body);
    }
    for (usize index = 0; index < launches->size(); ++index) {
        if (!arsenal.missiles().launchPrepared((*launches)[index], bodies[index])) {
            return {};
        }
    }
    return {.accepted = true, .launched = !launches->empty()};
}

} // namespace gdl::game
