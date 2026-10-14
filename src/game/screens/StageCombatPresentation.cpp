#include "game/screens/StageCombatPresentation.h"

#include <algorithm>
#include <cmath>

#include "game/screens/StageParticlePresentation.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {
namespace {
bool external(ProjectileSource source) {
    switch (source) {
    case ProjectileSource::Arrival:
    case ProjectileSource::Sumner:
    case ProjectileSource::Promotion:
    case ProjectileSource::TowerRelic:
    case ProjectileSource::TowerWizard: return true;
    default: return false;
    }
}
bool portal(FixtureSource source) {
    return source == FixtureSource::Portal || source == FixtureSource::SecretPortal;
}
bool unrepresentedParticles(const StageCombatPresentation::Sources& sources) {
    const auto missing = [](const EffectTrees& pool, const PlayerMissiles* flights) {
        for (usize i = 0; i < pool.count(); ++i) {
            const auto& effect = pool.effect(i);
            if (effect.trails.particleCount() != 0 ||
                (effect.particles.field().particleCount() != 0 &&
                 (flights == nullptr || flights->visualHistory(effect.id) == nullptr))) {
                return true;
            }
        }
        return false;
    };
    if (missing(sources.effects, nullptr) ||
        missing(sources.playerMissiles.visuals(), &sources.playerMissiles)) {
        return true;
    }
    const auto& items = sources.world.placedItems();
    if (items.burstParticleCount() != 0) {
        return true;
    }
    for (usize i = 0; i < items.size(); ++i) {
        const auto& item = items.item(i);
        if (item.visible && item.particles.field().particleCount() != 0) {
            return true;
        }
    }
    return false;
}
bool sameRoster(const CombatSnapshot& authority, const MotionSnapshot& motion) {
    if (authority.motion.epoch != motion.epoch ||
        authority.motion.cameraContinuity != motion.cameraContinuity) {
        return false;
    }
    for (usize seat = 0; seat < motion.players.size(); ++seat) {
        const auto& before = authority.motion.players[seat];
        const auto& after = motion.players[seat];
        if (before.has_value() != after.has_value() ||
            (before && (before->grant != after->grant || before->continuity != after->continuity ||
                        authority.players[seat]->portalPhase))) {
            return false;
        }
    }
    return true;
}
void drawingOnly(CombatSnapshot& snapshot) {
    snapshot.worldMotion.reset();
    snapshot.worldMotionTicks = 0;
    snapshot.opponents.reset();
    snapshot.opponentFrame.reset();
    snapshot.combatState.reset();
    snapshot.combatFrame.reset();
    for (auto& player : snapshot.players) {
        if (!player) {
            continue;
        }
        player->rollbackInput.reset();
        player->movement.enabled = false;
        player->movement.current = false;
        player->movement.ranged = false;
        player->movement.platform = false;
        player->knockbackTicks = 0;
        player->locomotion.reset();
        player->potion.reset();
        player->melee.reset();
        player->turbo.reset();
        player->turboGeometry.reset();
        player->turboEffects.reset();
        player->shove = false;
        player->weapon.reset();
        player->modifiers.reset();
    }
}
} // namespace

std::unique_ptr<CombatSnapshot>
StageCombatPresentation::capture(const CombatSnapshot& authority, const MotionSnapshot& motion,
                                 const Sources& sources, const Resources& resources,
                                 const StageParticlePresentation* particles) {
    if (!authority.valid() || !motion.valid() || motion.tick < authority.motion.tick ||
        !sameRoster(authority, motion) ||
        (particles ? !particles->matches(motion.epoch, motion.tick)
                   : unrepresentedParticles(sources)) ||
        std::ranges::any_of(authority.projectiles,
                            [](const auto& shot) { return external(shot.source); }) ||
        (sources.fixtures.portals == nullptr &&
         std::ranges::any_of(authority.fixtures,
                             [](const auto& fixture) { return portal(fixture.source); }))) {
        return nullptr;
    }
    auto captured =
        CombatCapture::capture(motion, sources.party, sources.enemies, nullptr, &sources.world);
    if (!captured) {
        return nullptr;
    }
    auto result = std::make_unique<CombatSnapshot>(std::move(*captured));
    result->geometry = sources.world.scene().geometry();
    if (!result->geometry || (authority.geometry &&
                              (authority.geometry->layout != result->geometry->layout ||
                               authority.geometry->objectCount != result->geometry->objectCount))) {
        return nullptr;
    }
    const auto& clock = sources.world.textureAnimator();
    result->worldTextureFrame =
        static_cast<f32>(clock.frame()) + clock.presentationOffset(1).value_or(0);
    if (!ProjectileCapture::append(*result, resources.projectiles, sources.playerMissiles,
                                   sources.enemyMissiles, &sources.effects) ||
        !PickupCapture::append(*result, sources.world.placedItems(), resources.pickups) ||
        !FixtureCapture::append(*result, resources.fixtures, sources.fixtures) ||
        !FighterCapture::append(*result, resources.fighters, sources.critters, sources.bosses)) {
        return nullptr;
    }
    // A nonnull but unbound portal owner is not coverage. Ordinary replay does
    // not consume exits, so every authoritative portal must still be represented
    // by its same trusted artwork identity (its pose may have advanced).
    for (const auto& fixture : authority.fixtures) {
        if (!portal(fixture.source)) {
            continue;
        }
        const auto found =
            std::ranges::lower_bound(result->fixtures, fixture.key(), {}, &FixtureState::key);
        if (found == result->fixtures.end() || found->key() != fixture.key() ||
            found->resource != fixture.resource || found->continuity != fixture.continuity) {
            return nullptr;
        }
    }
    drawingOnly(*result);
    return result->valid() ? std::move(result) : nullptr;
}

bool StageCombatPresentation::present(CombatSnapshot& shown, EnemyPresentation& presentation,
                                      const CombatSnapshot& previous, const CombatSnapshot& current,
                                      f32 blend, const StageSnapshotSampling::Swarm* swarm,
                                      const StageSnapshotSampling::Party* party) {
    if (!shown.valid() || !previous.valid() || !current.valid() || !presentation.matches(shown) ||
        !std::isfinite(blend) || blend < 0 || blend > 1 ||
        previous.motion.tick >= current.motion.tick ||
        current.motion.tick - previous.motion.tick != 1 ||
        shown.motion.tick > previous.motion.tick || !sameRoster(shown, previous.motion) ||
        !sameRoster(shown, current.motion) || !shown.geometry || !previous.geometry ||
        !current.geometry || shown.geometry->layout != previous.geometry->layout ||
        shown.geometry->layout != current.geometry->layout ||
        shown.geometry->objectCount != previous.geometry->objectCount ||
        shown.geometry->objectCount != current.geometry->objectCount) {
        return false;
    }
    for (usize seat = 0; seat < shown.players.size(); ++seat) {
        if (shown.players[seat] && (shown.players[seat]->life != previous.players[seat]->life ||
                                    shown.players[seat]->life != current.players[seat]->life)) {
            return false;
        }
    }
    EnemyPresentation nextPresentation;
    auto sampled =
        StageSnapshotSampling::sample(previous, current, blend, &nextPresentation, swarm, party);
    if (!sampled) {
        return false;
    }
    // The render submission retains its host display stamp, not a newly claimed
    // authority tick. Every visual field below nevertheless shares one private
    // interval, including remote seats, floor geometry, and attached artwork.
    sampled->motion.tick = shown.motion.tick;
    sampled->hud = shown.hud;
    sampled->musicArea = shown.musicArea;
    sampled->musicLevel = shown.musicLevel;
    for (usize seat = 0; seat < sampled->players.size(); ++seat) {
        if (sampled->players[seat]) {
            sampled->players[seat]->health = shown.players[seat]->health;
            sampled->players[seat]->damageable = shown.players[seat]->damageable;
        }
    }
    drawingOnly(*sampled);
    nextPresentation.epoch = sampled->motion.epoch;
    nextPresentation.tick = sampled->motion.tick;
    if (!sampled->valid() || !nextPresentation.matches(*sampled)) {
        return false;
    }
    shown = std::move(*sampled);
    presentation = std::move(nextPresentation);
    return true;
}
} // namespace gdl::game
