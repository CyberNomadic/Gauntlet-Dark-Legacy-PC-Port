#include "game/screens/OrdinaryProjectilePhase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

#include "game/screens/EnemyFeedbackEmission.h"
#include "game/screens/OrdinaryBarrelEmission.h"
#include "game/screens/OrdinaryStrongPhase.h"

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

template <class Impact> bool ordinaryImpact(const Impact& impact, bool phoenix = false) {
    return impact.owner >= 0 && impact.owner < 4 && impact.potion == 0 && impact.potency == 0 &&
           impact.player == -1 && !impact.stun &&
           (impact.target == -1 || (impact.worldObject == -1 && !impact.liquid)) &&
           ((impact.flags & ~PlayerMissiles::kPlainModifiers) == 0 ||
            (phoenix && impact.flags == 0x11 && impact.damage == 10 &&
             impact.wallSound == MissileWallSound::Silent && impact.playerHitGap == 0.25f));
}

bool knownOwner(s32 owner, std::span<const PlayerRuntime> players) {
    return std::ranges::any_of(players, [owner](const auto& player) {
        return !player.departed && player.actor.player() == owner;
    });
}

OrdinaryProjectilePhase::Result refuse(OrdinaryProjectilePhase::Refusal reason,
                                       std::optional<s32> target = std::nullopt) {
    return {.refusal = reason, .refusedTarget = target};
}

} // namespace

std::optional<EffectTrees::Prepared>
OrdinaryProjectilePhase::impactResource(const PlayerArsenal::ImpactPresentation& emission,
                                        std::span<const EffectTrees::PoolResource> resources,
                                        std::span<TextureSet* const> lenders,
                                        const RenderDevice& device) {
    const auto index = emission.archive->trees.find(emission.tree);
    if (!index) {
        return std::nullopt;
    }
    const auto* tree = &emission.archive->trees.tree(*index);
    for (const auto& resource : resources) {
        const auto* prototype = resource.prototype;
        if (resource.id == 0 || resource.archive != emission.archive ||
            resource.textures == nullptr || prototype == nullptr ||
            !std::ranges::equal(resource.lenders, lenders) ||
            !std::ranges::equal(prototype->lenders, lenders) ||
            prototype->archive != emission.archive || prototype->tree != tree ||
            prototype->device != &device || !prototype->emitParticles ||
            prototype->missingObjectsAreEmpty || prototype->trails.size() != 0) {
            continue;
        }
        EffectTrees::Prepared prepared{emission.archive,  tree,
                                       &prototype->model, &prototype->particles,
                                       resource.textures, resource.lenders};
        prepared.device = prototype->device;
        return prepared;
    }
    return std::nullopt;
}

bool OrdinaryProjectilePhase::ordinaryTarget(s32 target) {
    return target == -1 || (target >= 0 && target < PlayerAttacks::kCritterTargetBase) ||
           (target >= PlayerAttacks::kItemStopBase && target < PlayerAttacks::kPotionTargetBase);
}

bool OrdinaryProjectilePhase::ordinaryGenerator(const Generators& generators, s32 slot) {
    if (slot < 0 || static_cast<usize>(slot) >= generators.count() ||
        generators.count() > static_cast<usize>(PlayerAttacks::kCritterTargetBase -
                                                PlayerAttacks::kGeneratorTargetBase)) {
        return false;
    }
    const s32 kind = generators.kindOf(slot);
    return kind >= 0 && kind < kSwarmKindCount && kind != kDeathKind && kind != kItKind &&
           kind != kGarmBroodKind;
}

std::optional<u32> OrdinaryProjectilePhase::generatorPresentation(
    const GeneratorFeedbackEmission& emission, EffectTrees& effects, const RenderDevice& device,
    std::span<const EffectTrees::PoolResource> resources, std::vector<Cue>& cues) {
    u32 effect = 0;
    if (emission.archive != nullptr) {
        const auto prepared = emission.prepare(resources, effects.textureLenders(), device);
        if (!prepared) {
            return std::nullopt;
        }
        effect = effects.startPrepared(*prepared, emission.position, {});
        if (effect == 0) {
            return std::nullopt;
        }
        effects.placeAt(effect, emission.placement);
    }
    if (!emission.sound.empty()) {
        cues.push_back({emission.sound, emission.position, std::nullopt, 1});
    }
    return effect;
}

bool OrdinaryProjectilePhase::supported(const PlayerArsenalCheckpoint::State& state,
                                        std::span<const PlayerMissiles::Resource> resources,
                                        f32 seconds, const PlayerArsenal* arsenal,
                                        std::span<const PlayerRuntime> players) {
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 1.0f / 30 || !state.valid() ||
        !state.bound || !state.casts.empty() || state.missiles.ricochetIn != 0 ||
        resources.size() > PlayerMissiles::Checkpoint::kMaxResources ||
        state.missiles.impacts.size() > kMaxImpacts ||
        state.missiles.missiles.size() > kMaxImpacts - state.missiles.impacts.size()) {
        return false;
    }
    std::unordered_map<u32, const PlayerMissiles::Resource*> specs;
    specs.reserve(resources.size());
    for (const auto& resource : resources) {
        if (resource.id == 0 || resource.spec == nullptr ||
            !specs.emplace(resource.id, &resource).second) {
            return false;
        }
    }
    usize total = 0;
    for (const auto& flight : state.missiles.missiles) {
        const auto resource = specs.find(flight.resource);
        if (resource == specs.end()) {
            return false;
        }
        const auto& binding = *resource->second;
        bool companion = false;
        if (arsenal && arsenal->m_resources &&
            binding.spec == &PlayerArsenal::familiarSpec(arsenal->m_resources->bossEncounter)) {
            const auto player = std::ranges::find_if(players, [&](const auto& value) {
                return !value.departed && value.actor.player() == flight.owner && value.figure;
            });
            const bool phoenix = binding.model == &arsenal->phoenixMissile();
            const bool earned =
                player != players.end() && binding.model == &player->figure->familiarMissile();
            companion = player != players.end() && binding.model && binding.model->bound() &&
                        binding.streak == nullptr && (phoenix || earned) &&
                        flight.flags == (phoenix ? 0x11U : 0U) &&
                        (!phoenix || flight.damage == 10) && flight.scale == 1 &&
                        flight.wallSound == MissileWallSound::Silent && !flight.breaksPotions &&
                        flight.playerHitGap == 0.25f && !flight.release;
        }
        if ((!ordinarySpec(binding.spec) && !companion) || flight.potion != 0 ||
            flight.potency != 0 || flight.potionRelease ||
            flight.multiplayer != MultiplayerMode::Normal || flight.rider != 0 ||
            (!companion && (flight.flags & ~PlayerMissiles::kPlainModifiers) != 0) ||
            !flight.pierced.empty() || !flight.playerContacts.empty() ||
            (flight.release && flight.release->emission != 0)) {
            return false;
        }
        // PlayerMissiles uses this exact pre-gravity f32 expression to choose
        // its spatial substeps. Check it before its float-to-s32 conversion;
        // finite captured velocity alone does not bound native update work.
        const f32 steps = std::max(1.0f, std::ceil(glm::length(flight.velocity) * seconds /
                                                   (binding.spec->radius * 0.5f)));
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
        if (!ordinaryImpact(impact) || !ordinaryTarget(impact.target) ||
            (impact.effectResource != 0 &&
             (resource == specs.end() || !ordinarySpec(resource->second->spec)))) {
            return false;
        }
    }
    return std::ranges::none_of(state.missiles.history, [](const auto& history) {
        return history.potion != 0 || history.potionRelease ||
               (history.release && history.release->emission != 0);
    });
}

bool OrdinaryProjectilePhase::supportsEffect(const PlayerArsenal& arsenal,
                                             const EffectTrees::PoolResource& resource) {
    if (!arsenal.m_resources || resource.id == 0 ||
        resource.archive != &arsenal.m_resources->weapons || resource.textures == nullptr ||
        resource.prototype == nullptr || resource.prototype->archive != resource.archive ||
        resource.prototype->tree == nullptr ||
        resource.prototype->device != &arsenal.m_resources->device ||
        !resource.prototype->emitParticles || resource.prototype->missingObjectsAreEmpty ||
        resource.prototype->trails.size() != 0 ||
        !std::ranges::equal(resource.prototype->lenders, resource.lenders) ||
        !std::ranges::equal(resource.lenders, arsenal.m_resources->effects.textureLenders())) {
        return false;
    }
    for (s32 character = 0; character <= 16; ++character) {
        const auto tree = resource.archive->trees.find(MissileSpec::of(character).impactTree);
        if (tree && resource.prototype->tree == &resource.archive->trees.tree(*tree)) {
            return true;
        }
    }
    return false;
}

OrdinaryProjectilePhase::Result
OrdinaryProjectilePhase::advance(PlayerAttacks& attacks, std::span<PlayerRuntime> players,
                                 const PlayerAttacks::Targets& targets, f32 seconds,
                                 const CombatRollbackCheckpoint::Resources& resources,
                                 LevelWorldCheckpoint* world) {
    if (!attacks.m_resources || players.empty() || players.size() > 4) {
        return refuse(Refusal::Binding);
    }
    auto& bound = *attacks.m_resources;
    if (targets.multiplayer != MultiplayerMode::Normal ||
        (bound.multiplayer != nullptr && *bound.multiplayer != MultiplayerMode::Normal)) {
        return refuse(Refusal::Multiplayer);
    }
    if (!OrdinaryStrongPhase::supported(attacks, players)) {
        return refuse(Refusal::StrongState);
    }
    if (targets.fixtures.barrels().size() > static_cast<usize>(PlayerAttacks::kEnemyTargetBase) ||
        targets.fixtures.inertStops().size() >
            static_cast<usize>(PlayerAttacks::kPotionTargetBase - PlayerAttacks::kItemStopBase) ||
        targets.opponents.generators().count() >
            static_cast<usize>(PlayerAttacks::kCritterTargetBase -
                               PlayerAttacks::kGeneratorTargetBase)) {
        return refuse(Refusal::TargetCapacity); // Native IDs must not alias another family.
    }
    std::array<bool, 4> occupied{};
    for (const auto& player : players) {
        const s32 slot = player.actor.player();
        const auto worn = PowerupEffects::of(player.actor.save().progress().inventory);
        if (slot < 0 || slot >= 4 || occupied[static_cast<usize>(slot)] ||
            (worn.weapon & ~PlayerMissiles::kPlainModifiers) != 0) {
            return refuse(Refusal::Owner);
        }
        occupied[static_cast<usize>(slot)] = true;
    }
    const auto before = PlayerArsenalCheckpoint::capture(bound.arsenal, resources.missiles,
                                                         resources.missileEffects);
    if (!before) {
        return refuse(Refusal::Capture);
    }
    if (!supported(*before, resources.missiles, seconds, &bound.arsenal, players)) {
        return refuse(Refusal::State);
    }
    if (std::ranges::any_of(
            before->missiles.missiles,
            [&](const auto& flight) { return !knownOwner(flight.owner, players); }) ||
        std::ranges::any_of(before->missiles.impacts, [&](const auto& impact) {
            return !knownOwner(impact.owner, players);
        })) {
        return refuse(Refusal::Owner);
    }
    attacks.stepProjectileFlights(seconds, players, targets);
    // Native scenery contact happens only at surviving endpoints after all
    // spatial flight substeps, never at a swept/intermediate position or the
    // endpoint of a retired missile. Preflight the complete batch before its
    // first external cue. A refused tick is restored by the enclosing owner.
    auto& flying = bound.arsenal.missiles();
    for (usize index = 0; index < flying.count(); ++index) {
        const auto& missile = flying.missile(index);
        if (bound.world.fallingScenery().wouldShoot(
                missile.position, missile.spec != nullptr ? missile.spec->radius : 1.0f)) {
            return refuse(Refusal::Scenery);
        }
    }
    const auto impacts = flying.takeImpacts();
    if (impacts.size() > kMaxImpacts) {
        return refuse(Refusal::State);
    }
    const auto& enemies = targets.opponents.enemies();
    std::vector<std::optional<EffectTrees::Prepared>> presentation;
    presentation.reserve(impacts.size());
    for (const auto& impact : impacts) {
        // The complete preflight above rejected every preexisting special
        // impact and admitted 0x11 only on the exact native Phoenix model/spec.
        // Thus a new 0x11 impact here is from a validated native flight, never
        // a blanket admission of an unproven queued elemental hit.
        if (!ordinaryImpact(impact, true) || (impact.flags == 0x11 && !impact.effect.empty())) {
            return refuse(Refusal::Impact, impact.target);
        }
        if (!ordinaryTarget(impact.target)) {
            return refuse(Refusal::Target, impact.target);
        }
        if (!knownOwner(impact.owner, players)) {
            return refuse(Refusal::Owner, impact.target);
        }
        if (bound.world.wouldExplodeObject(impact.worldObject)) {
            return refuse(Refusal::WorldExplosion, impact.target);
        }
        if (impact.target >= 0 && impact.target < PlayerAttacks::kEnemyTargetBase) {
            if (static_cast<usize>(impact.target) >= targets.fixtures.barrels().size()) {
                return refuse(Refusal::Target, impact.target);
            }
            // Native barrel dispatch settles the entire blast queue, even if
            // this particular strike is nonbreaking or a no-op on spent wood.
            if (!targets.fixtures.m_blasts.empty()) {
                return refuse(Refusal::BarrelBlasts, impact.target);
            }
        } else if (impact.target >= PlayerAttacks::kEnemyTargetBase &&
                   impact.target < PlayerAttacks::kGeneratorTargetBase) {
            const s32 id = impact.target - PlayerAttacks::kEnemyTargetBase;
            const s32 kind = enemies.kindOf(id);
            if (!enemies.observe(id) || kind < 0 || kind == kDeathKind || kind == kItKind ||
                kind == kGarmBroodKind || enemies.algorithmOf(id) == kSuicideWay) {
                return refuse(Refusal::Enemy, impact.target);
            }
        } else if (impact.target >= PlayerAttacks::kGeneratorTargetBase &&
                   impact.target < PlayerAttacks::kCritterTargetBase &&
                   !ordinaryGenerator(targets.opponents.generators(),
                                      impact.target - PlayerAttacks::kGeneratorTargetBase)) {
            return refuse(Refusal::Generator, impact.target);
        }
        // Native missing artwork remains missing; an available native tree must
        // have exactly its prebound archive/lender prototype before any damage.
        const auto emission = bound.arsenal.impactPresentation(impact);
        std::optional<EffectTrees::Prepared> prepared;
        if (emission.archive != nullptr && emission.archive->trees.find(emission.tree)) {
            prepared = impactResource(emission, resources.sharedEffects,
                                      bound.effects.textureLenders(), bound.device);
            if (!prepared) {
                return refuse(Refusal::Resource, impact.target);
            }
        }
        presentation.push_back(prepared);
    }
    // Target admission covers the whole batch. Generator state-dependent art is
    // prepared at its native damage point; failure there requires the caller's
    // whole-tick undo, including earlier impacts and the consumed flight queue.
    Result result{.advanced = true, .impacts = impacts.size()};
    for (usize index = 0; index < impacts.size(); ++index) {
        Refusal failure = Refusal::None;
        const bool dispatched = attacks.dispatchProjectileImpact(
            impacts[index], players, targets,
            [&](const MissileImpact& impact, f32 distance) {
                const auto emission = bound.arsenal.impactPresentation(impact, distance);
                if (presentation[index] &&
                    bound.effects.startPrepared(*presentation[index], emission.position,
                                                emission.setting) == 0) {
                    result.advanced = false;
                    failure = Refusal::Effect;
                }
                if (emission.cue) {
                    result.cues.push_back(*emission.cue);
                }
            },
            [&](const GeneratorFeedbackEmission& emission) {
                const auto effect = generatorPresentation(emission, bound.effects, bound.device,
                                                          resources.sharedEffects, result.cues);
                if (!effect) {
                    failure = Refusal::GeneratorEmission;
                }
                return effect;
            },
            [&](const BreakableStrike& strike, std::string_view sound) {
                // Preserve native mutation/order; the caller's complete undo
                // owns rounded damage, authored break animation and choice RNG.
                // No rejected drop/help, hazard, rubble or sound has run yet.
                if (strike.broken) {
                    if (strike.contents >= 0 && world == nullptr) {
                        failure = Refusal::BarrelContents;
                    } else if (strike.kind == BreakableStrike::Kind::Exploding) {
                        failure = Refusal::BarrelExplosion;
                    } else if (strike.kind == BreakableStrike::Kind::Poison) {
                        failure = Refusal::BarrelGas;
                    } else if (strike.kind != BreakableStrike::Kind::Plain &&
                               strike.kind != BreakableStrike::Kind::Holding) {
                        failure = Refusal::BarrelEmission;
                    }
                }
                if (failure != Refusal::None) {
                    return false;
                }
                if (world != nullptr) {
                    auto emission = OrdinaryBarrelEmission::advance(
                        strike, sound, impacts[index].owner, players, targets.fixtures, *world);
                    if (!emission.accepted) {
                        using Rejected = OrdinaryBarrelEmission::Refusal;
                        failure = emission.refusal == Rejected::Record ||
                                          emission.refusal == Rejected::Birth ||
                                          emission.refusal == Rejected::Lesson
                                      ? Refusal::BarrelContents
                                      : Refusal::BarrelEmission;
                        return false;
                    }
                    if (emission.cue) {
                        result.cues.push_back(std::move(*emission.cue));
                    }
                    return true;
                }
                result.cues.push_back({std::string{sound}, strike.position, std::nullopt, 1});
                return true;
            });
        if (!dispatched || !result.advanced) {
            // No partial outbox escapes. The enclosing transaction owns all
            // earlier damage and the consumed flight queue, not just this impact.
            return refuse(failure == Refusal::None ? Refusal::BarrelEmission : failure,
                          impacts[index].target);
        }
    }
    return result;
}

} // namespace gdl::game
