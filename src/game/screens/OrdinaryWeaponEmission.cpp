#include "game/screens/OrdinaryWeaponEmission.h"

#include <algorithm>
#include <cmath>
#include <vector>

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
    if (!arsenal.m_resources || actor.player() < 0 || actor.player() >= 4 || save.character < 0 ||
        save.character > 16 || !animator.released() || animator.strongReleased() ||
        !finite(direction) || glm::length(direction) < 1e-6f || (target && !finite(*target))) {
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
    auto prepared = arsenal.prepareWeapon(actor, figure, direction, 1, true, target);
    if (!prepared || prepared->pojo || prepared->missiles.empty() ||
        prepared->missiles.size() > 5 || prepared->weapon.multiplayer != MultiplayerMode::Normal) {
        return {};
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
            launch.release->character != save.character || launch.release->strong ||
            launch.release->ordinal != ordinal || launch.release->emission != 0 ||
            launch.release->generation != animator.player().generation() ||
            launch.release->generation == 0 || !finite(launch.position) ||
            !finite(launch.direction) || !finite(launch.speed) || launch.speed <= 0 ||
            !finite(launch.reach) || launch.reach <= 0 || !finite(launch.damage) ||
            (launch.velocity && !finite(*launch.velocity))) {
            return {};
        }
        const Vec3 clear = arsenal.m_resources->collision.resolveWalls(
            launch.position, launch.spec->radius, launch.position.y - launch.spec->radius * 0.5f,
            launch.position.y + launch.spec->radius * 0.5f);
        if (!finite(clear) || glm::distance(clear, launch.position) > 1e-4f) {
            // The live blocked-muzzle path immediately emits an untargeted
            // world impact. It must be refused before those external outputs.
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
    return {.launched = true, .cue = cue};
}

} // namespace gdl::game
