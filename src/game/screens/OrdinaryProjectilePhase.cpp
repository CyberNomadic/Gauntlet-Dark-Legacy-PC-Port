#include "game/screens/OrdinaryProjectilePhase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace gdl::game {
namespace {

bool ordinarySpec(const MissileSpec* spec) {
    for (s32 character = 0; character <= 16; ++character) {
        if (spec == &MissileSpec::of(character)) {
            return true;
        }
    }
    return false;
}

template <class Impact> bool ordinaryImpact(const Impact& impact) {
    return impact.owner >= 0 && impact.owner < 4 && impact.potion == 0 && impact.potency == 0 &&
           impact.player == -1 && impact.worldObject == -1 && !impact.liquid && !impact.stun &&
           (impact.flags & ~PlayerMissiles::kPlainModifiers) == 0;
}

bool knownOwner(s32 owner, std::span<const PlayerRuntime> players) {
    return std::ranges::any_of(players, [owner](const auto& player) {
        return !player.departed && player.actor.player() == owner;
    });
}

} // namespace

bool OrdinaryProjectilePhase::supported(const PlayerArsenalCheckpoint::State& state,
                                        std::span<const PlayerMissiles::Resource> resources,
                                        f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 1.0f / 30 || !state.valid() ||
        !state.bound || !state.casts.empty() || state.missiles.ricochetIn != 0 ||
        resources.size() > PlayerMissiles::Checkpoint::kMaxResources ||
        state.missiles.impacts.size() > kMaxImpacts ||
        state.missiles.missiles.size() > kMaxImpacts - state.missiles.impacts.size()) {
        return false;
    }
    std::unordered_map<u32, const MissileSpec*> specs;
    specs.reserve(resources.size());
    for (const auto& resource : resources) {
        if (resource.id == 0 || resource.spec == nullptr ||
            !specs.emplace(resource.id, resource.spec).second) {
            return false;
        }
    }
    usize total = 0;
    for (const auto& flight : state.missiles.missiles) {
        const auto resource = specs.find(flight.resource);
        if (resource == specs.end() || !ordinarySpec(resource->second) || flight.potion != 0 ||
            flight.potency != 0 || flight.potionRelease ||
            flight.multiplayer != MultiplayerMode::Normal || flight.rider != 0 ||
            (flight.flags & ~PlayerMissiles::kPlainModifiers) != 0 || !flight.pierced.empty() ||
            !flight.playerContacts.empty() || (flight.release && flight.release->emission != 0)) {
            return false;
        }
        // PlayerMissiles uses this exact pre-gravity f32 expression to choose
        // its spatial substeps. Check it before its float-to-s32 conversion;
        // finite captured velocity alone does not bound native update work.
        const f32 steps = std::max(1.0f, std::ceil(glm::length(flight.velocity) * seconds /
                                                   (resource->second->radius * 0.5f)));
        if (!std::isfinite(steps) || steps > static_cast<f32>(kMaxFlightSteps)) {
            return false;
        }
        total += static_cast<usize>(steps);
        if (total > kMaxTotalSteps) {
            return false;
        }
    }
    for (const auto& impact : state.missiles.impacts) {
        const auto resource = specs.find(impact.effectResource);
        if (!ordinaryImpact(impact) || impact.target < PlayerAttacks::kEnemyTargetBase ||
            impact.target >= PlayerAttacks::kGeneratorTargetBase ||
            (impact.effectResource != 0 &&
             (resource == specs.end() || !ordinarySpec(resource->second)))) {
            return false;
        }
    }
    return std::ranges::none_of(state.missiles.history, [](const auto& history) {
        return history.potion != 0 || history.potionRelease ||
               (history.release && history.release->emission != 0);
    });
}

OrdinaryProjectilePhase::Result
OrdinaryProjectilePhase::advance(PlayerAttacks& attacks, std::span<PlayerRuntime> players,
                                 const PlayerAttacks::Targets& targets, f32 seconds,
                                 const CombatRollbackCheckpoint::Resources& resources) {
    if (!attacks.m_resources || targets.multiplayer != MultiplayerMode::Normal || players.empty() ||
        players.size() > 4 || attacks.pendingGameplay() || !attacks.m_moveAttachments.empty() ||
        !attacks.m_particleEffects.empty() || !attacks.m_strikeEffects.empty() ||
        !attacks.m_strikeSources.empty()) {
        return {};
    }
    auto& bound = *attacks.m_resources;
    // Native surviving missiles call shootScenery before impact dispatch. That
    // owner is not in this transaction, even when no current obstacle overlaps.
    if (bound.world.fallingScenery().size() != 0 ||
        (bound.multiplayer != nullptr && *bound.multiplayer != MultiplayerMode::Normal)) {
        return {};
    }
    std::array<bool, 4> occupied{};
    for (const auto& player : players) {
        const s32 slot = player.actor.player();
        const auto worn = PowerupEffects::of(player.actor.save().progress().inventory);
        if (slot < 0 || slot >= 4 || occupied[static_cast<usize>(slot)] ||
            (worn.weapon & ~PlayerMissiles::kPlainModifiers) != 0) {
            return {};
        }
        occupied[static_cast<usize>(slot)] = true;
    }
    const auto before = PlayerArsenalCheckpoint::capture(bound.arsenal, resources.missiles,
                                                         resources.missileEffects);
    if (!before || !supported(*before, resources.missiles, seconds) ||
        std::ranges::any_of(
            before->missiles.missiles,
            [&](const auto& flight) { return !knownOwner(flight.owner, players); }) ||
        std::ranges::any_of(before->missiles.impacts, [&](const auto& impact) {
            return !knownOwner(impact.owner, players);
        })) {
        return {};
    }
    const auto impacts = attacks.advanceProjectileFlights(seconds, players, targets);
    if (impacts.size() > kMaxImpacts) {
        return {};
    }
    const auto& enemies = targets.opponents.enemies();
    for (const auto& impact : impacts) {
        if (!ordinaryImpact(impact) || impact.target < PlayerAttacks::kEnemyTargetBase ||
            impact.target >= PlayerAttacks::kGeneratorTargetBase ||
            !knownOwner(impact.owner, players)) {
            return {};
        }
        const s32 id = impact.target - PlayerAttacks::kEnemyTargetBase;
        const s32 kind = enemies.kindOf(id);
        if (!enemies.observe(id) || kind < 0 || kind == kDeathKind || kind == kItKind ||
            kind == kGarmBroodKind || enemies.algorithmOf(id) == kSuicideWay) {
            return {};
        }
    }
    // A later unsupported hit cannot leave an earlier enemy damaged. The whole
    // batch has passed, and ordinary target impacts produce only owned queues.
    for (const auto& impact : impacts) {
        attacks.dispatchProjectileImpact(impact, players, targets);
    }
    return {true, impacts.size()};
}

} // namespace gdl::game
