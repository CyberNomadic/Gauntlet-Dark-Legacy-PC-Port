#include "game/screens/OrdinaryStrongPhase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>

namespace gdl::game {
namespace {
std::optional<EffectTrees::Prepared> treeOf(ItemArchive& archive, std::string_view name,
                                            std::span<TextureSet* const> lenders,
                                            std::span<const EffectTrees::PoolResource> resources) {
    const auto index = archive.trees.find(name);
    if (!index) {
        return std::nullopt;
    }
    const auto* tree = &archive.trees.tree(*index);
    for (const auto& resource : resources) {
        const auto* prototype = resource.prototype;
        if (resource.id == 0 || resource.archive != &archive || resource.textures == nullptr ||
            !std::ranges::equal(resource.lenders, lenders) || prototype == nullptr ||
            prototype->archive != &archive || prototype->tree != tree ||
            !prototype->emitParticles || prototype->missingObjectsAreEmpty ||
            prototype->trails.size() != 0 ||
            (!prototype->model.bound() && prototype->particles.field().size() == 0)) {
            continue;
        }
        EffectTrees::Prepared prepared{&archive,          tree,
                                       &prototype->model, &prototype->particles,
                                       resource.textures, resource.lenders};
        prepared.device = prototype->device;
        return prepared;
    }
    return std::nullopt;
}

std::optional<EffectTrees::Prepared>
particleOf(ItemArchive& archive, const ParticleDescriptor& descriptor, u32 texture,
           std::span<const EffectTrees::PoolResource> resources) {
    for (const auto& resource : resources) {
        const auto* prototype = resource.prototype;
        if (resource.id == 0 || resource.archive != &archive || resource.textures == nullptr ||
            !resource.lenders.empty() || prototype == nullptr || prototype->archive != &archive ||
            prototype->tree != nullptr || prototype->particleTextureSlot != texture ||
            prototype->trails.size() != 1 ||
            prototype->trails.emitter(0).descriptor() != descriptor ||
            prototype->trails.textureOf(0) == nullptr) {
            continue;
        }
        EffectTrees::Prepared prepared;
        prepared.archive = &archive;
        prepared.textures = resource.textures;
        prepared.descriptor = &prototype->trails.emitter(0).descriptor();
        prepared.texture = prototype->trails.textureOf(0);
        prepared.textureSlot = texture;
        prepared.device = prototype->device;
        return prepared;
    }
    return std::nullopt;
}

struct Binding {
    ItemArchive* archive = nullptr;
    std::string_view tree;
    std::optional<EffectTrees::Prepared> prepared;
};
} // namespace

bool OrdinaryStrongPhase::supported(const PlayerAttacks& attacks,
                                    std::span<const PlayerRuntime> party) {
    constexpr usize kMaxStrikes = 256;
    if (!attacks.m_resources || attacks.m_strikes.count() > kMaxStrikes ||
        attacks.m_strikeSources.size() > kMaxStrikes || !attacks.m_items.empty() ||
        !attacks.m_potions.empty() || !attacks.m_shields.empty()) {
        return false;
    }
    const auto& bound = *attacks.m_resources;
    if (bound.multiplayer != nullptr && *bound.multiplayer != MultiplayerMode::Normal) {
        return false;
    }
    for (const auto& source : attacks.m_strikeSources) {
        if (source.actor >= party.size() || source.multiplayer != MultiplayerMode::Normal ||
            !party[source.actor].figure || !party[source.actor].figure->loadedEffects()) {
            return false;
        }
        const auto* stats = bound.classes.stats(party[source.actor].actor.save().character);
        if (stats == nullptr || source.row < 0 ||
            static_cast<usize>(source.row) >= stats->moveStrikes.size()) {
            return false;
        }
        const auto rows = stats->strikesOf(stats->moves.turboAThrow);
        if (std::ranges::find(rows, source.row) == rows.end()) {
            return false;
        }
        const auto& row = stats->moveStrikes[static_cast<usize>(source.row)];
        if (row.hitEffect >= 0 || row.damageType != source.damageType || row.dimming() != 0) {
            return false;
        }
    }
    return true;
}

bool OrdinaryStrongPhase::supportsEffect(const PlayerAttacks& attacks,
                                         const EffectTrees::PoolResource& resource,
                                         std::span<const PlayerRuntime> party) {
    if (!attacks.m_resources || resource.id == 0 || resource.prototype == nullptr ||
        resource.archive == nullptr) {
        return false;
    }
    const auto& bound = *attacks.m_resources;
    const auto& prototype = *resource.prototype;
    for (const auto& player : party) {
        const auto* stats = bound.classes.stats(player.actor.save().character);
        if (stats == nullptr || !player.figure) {
            continue;
        }
        const auto* classArchive = player.figure->loadedEffects();
        if (resource.archive != classArchive && resource.archive != &bound.weapons) {
            continue;
        }
        for (const s32 row : stats->strikesOf(stats->moves.turboAThrow)) {
            const auto& strike = stats->moveStrikes[static_cast<usize>(row)];
            s32 index = strike.effect;
            for (usize visited = 0;
                 index >= 0 && static_cast<usize>(index) < stats->moveEffects.size() &&
                 visited < stats->moveEffects.size();
                 ++visited) {
                const auto& effect = stats->moveEffects[static_cast<usize>(index)];
                if (effect.particle()) {
                    if (prototype.tree == nullptr && prototype.trails.size() == 1 &&
                        prototype.trails.emitter(0).descriptor() ==
                            TurboEmission::particle(effect).descriptor) {
                        return true;
                    }
                } else if (prototype.tree != nullptr && prototype.tree->name == effect.tree) {
                    return true;
                }
                index = effect.next;
            }
        }
    }
    return false;
}

OrdinaryStrongPhase::Result
OrdinaryStrongPhase::emit(PlayerAttacks& attacks, usize index, const TurboMove::Strike& occurrence,
                          std::span<PlayerRuntime> party,
                          const CombatRollbackCheckpoint::Resources& resources) {
    if (!attacks.m_resources || index >= party.size() || party[index].figure == nullptr ||
        occurrence.slot >= TurboMove::kMaxCheckpointRows ||
        resources.sharedEffects.size() >
            EffectTrees::PoolCheckpoint::kMaxEffects + EffectTrees::PoolCheckpoint::kMaxBanks) {
        return {};
    }
    auto& bound = *attacks.m_resources;
    auto& player = party[index];
    auto& figure = *player.figure;
    const auto& animator = figure.animator();
    const auto* stats = bound.classes.stats(player.actor.save().character);
    if (stats == nullptr || animator.action() != PlayerAnimator::Action::StrongThrow ||
        animator.player().generation() == 0 || player.combo.active() || player.capture.active() ||
        (bound.multiplayer != nullptr && *bound.multiplayer != MultiplayerMode::Normal)) {
        return {};
    }
    const auto rows = stats->strikesOf(stats->moves.turboAThrow);
    if (occurrence.slot >= rows.size() || rows[occurrence.slot] != occurrence.row ||
        occurrence.row < 0 || static_cast<usize>(occurrence.row) >= stats->moveStrikes.size() ||
        stats->moveStrikes[static_cast<usize>(occurrence.row)].dimming() != 0) {
        return {};
    }
    // Resources were loaded before the transaction. Do not call figure.effects()
    // here: that getter may lazily create a new archive and invalidate the undo.
    const ItemArchive* classArchive = figure.loadedEffects();
    if (classArchive == nullptr) {
        return {};
    }
    const auto prepared = attacks.prepareStrike(index, occurrence.row, party);
    if (!prepared || prepared->effects.size() > std::numeric_limits<u16>::max()) {
        return {};
    }
    std::vector<Binding> bindings;
    bindings.reserve(prepared->effects.size());
    for (const auto& emission : prepared->effects) {
        if (emission.shake || !emission.setting.then.empty() || emission.setting.morphIn ||
            emission.setting.holdForMorph ||
            (emission.attachment && emission.attachment->actor != TurboEmission::Actor::Caster)) {
            return {};
        }
        Binding binding;
        if (emission.particle) {
            const auto& particle = *emission.particle;
            if (particle.partnerFallback || particle.previousEffect ||
                particle.descriptor.emitFrames == ParticleDescriptor::kEndless) {
                return {};
            }
            // Native standalone particles deliberately have no scene lenders.
            for (const ItemArchive* archive :
                 std::array<const ItemArchive*, 2>{classArchive, &bound.weapons}) {
                const auto slot = archive->textures.find(particle.descriptor.texture);
                if (!slot || archive->textures.entry(*slot).external() ||
                    archive->textures.entry(*slot).noPicture) {
                    continue;
                }
                // Resolve the mutable archive only through its trusted catalog.
                for (const auto& resource : resources.sharedEffects) {
                    if (resource.archive == archive) {
                        binding.archive = resource.archive;
                        break;
                    }
                }
                if (binding.archive == nullptr) {
                    return {};
                }
                binding.prepared = particleOf(*binding.archive, particle.descriptor, *slot,
                                              resources.sharedEffects);
                if (!binding.prepared) {
                    return {};
                }
                break;
            }
        } else {
            binding.tree = emission.authored.tree;
            if (!binding.tree.empty() && binding.tree != "NULLFX") {
                for (const ItemArchive* archive :
                     std::array<const ItemArchive*, 2>{classArchive, &bound.weapons}) {
                    if (!archive->trees.find(binding.tree)) {
                        continue;
                    }
                    for (const auto& resource : resources.sharedEffects) {
                        if (resource.archive == archive) {
                            binding.archive = resource.archive;
                            break;
                        }
                    }
                    if (binding.archive == nullptr) {
                        return {};
                    }
                    binding.prepared =
                        treeOf(*binding.archive, binding.tree, bound.effects.textureLenders(),
                               resources.sharedEffects);
                    if (!binding.prepared) {
                        return {};
                    }
                    break;
                }
            }
        }
        // Missing native trees/textures retain native no-art behavior. A known
        // archive with an unprepared topology is never replaced by no art.
        bindings.push_back(binding);
    }
    Result result;
    bool refused = false;
    const PlayerAttacks::StrikeEvents events{
        .tree = [&](ItemArchive& archive, std::string_view name, const Vec3& position,
                    const EffectTrees::Setting& setting) -> std::optional<u32> {
            const auto binding = std::ranges::find_if(bindings, [&](const auto& value) {
                return value.archive == &archive && value.tree == name && value.prepared &&
                       value.prepared->tree != nullptr;
            });
            if (binding == bindings.end()) {
                refused = true;
                return std::nullopt;
            }
            const auto id = bound.effects.startPrepared(*binding->prepared, position, setting);
            refused |= id == 0;
            return id != 0 ? std::optional{id} : std::nullopt;
        },
        .particle = [&](ItemArchive& archive, const ParticleDescriptor& descriptor,
                        u32 texture) -> std::optional<u32> {
            const auto binding = std::ranges::find_if(bindings, [&](const auto& value) {
                return value.archive == &archive && value.prepared &&
                       value.prepared->descriptor != nullptr &&
                       *value.prepared->descriptor == descriptor &&
                       value.prepared->textureSlot == texture;
            });
            if (binding == bindings.end()) {
                refused = true;
                return std::nullopt;
            }
            // This overload uses the ID it is about to allocate, exactly like
            // native startParticles. It never reseeds an existing emitter.
            const u32 id = bound.effects.startPreparedParticles(*binding->prepared, Mat4{1});
            refused |= id == 0;
            return id != 0 ? std::optional{id} : std::nullopt;
        },
        .sound =
            [&](usize actor, usize ordinal, std::string_view name) {
                if (actor != index || ordinal >= prepared->effects.size()) {
                    refused = true;
                    return;
                }
                Cue cue{player.actor.player(),
                        animator.player().generation(),
                        static_cast<u16>(occurrence.slot),
                        static_cast<u16>(ordinal),
                        std::string{name},
                        figure.voice().find(name)};
                result.cues.push_back(std::move(cue));
            },
        .shake = [&] { refused = true; }};
    result.advanced =
        attacks.applyStrike(index, *prepared, party, std::nullopt, events) && !refused;
    if (!result.advanced) {
        result.cues.clear();
    }
    return result;
}

OrdinaryStrongPhase::Result
OrdinaryStrongPhase::advance(PlayerAttacks& attacks, usize index, s32 ticks, f32 seconds,
                             std::span<PlayerRuntime> party,
                             const CombatRollbackCheckpoint::Resources& resources,
                             const std::function<bool(s32, usize)>& help) {
    if (!attacks.m_resources || index >= party.size() || party[index].figure == nullptr ||
        ticks < 1 || ticks > 2 || !std::isfinite(seconds) || seconds <= 0 || seconds > 1.0f / 30 ||
        !help) {
        return {};
    }
    const auto action = party[index].figure->animator().action();
    if (action != PlayerAnimator::Action::StrongThrow &&
        action != PlayerAnimator::Action::StrongThrowRecover) {
        return {};
    }
    Result result;
    bool refused = false;
    const TurboMove::Events events{
        .announce = [&](s32 id) { refused |= !help(id, index); },
        .dim = [&](f32) { refused = true; },
        .identifiedVolley = [&](const TurboMove::Volley&) { refused = true; },
        .identifiedStrike =
            [&](const TurboMove::Strike& strike) {
                if (refused) {
                    return;
                }
                auto emitted = emit(attacks, index, strike, party, resources);
                if (!emitted.advanced) {
                    refused = true;
                    return;
                }
                result.cues.insert(result.cues.end(), std::make_move_iterator(emitted.cues.begin()),
                                   std::make_move_iterator(emitted.cues.end()));
            }};
    attacks.stepTurbo(index, ticks, seconds, party, events,
                      [&](std::string_view) { refused = true; });
    result.advanced = !refused;
    if (refused) {
        result.cues.clear();
    }
    return result;
}

bool OrdinaryStrongPhase::stepStrikes(PlayerAttacks& attacks, f32 seconds,
                                      std::span<PlayerRuntime> party,
                                      const PlayerAttacks::Targets& targets) {
    constexpr usize kMaxWallSteps = 4096;
    constexpr usize kMaxTotalWallSteps = 8192;
    if (!supported(attacks, party) || !std::isfinite(seconds) || seconds <= 0 ||
        seconds > 1.0f / 30 || targets.multiplayer != MultiplayerMode::Normal) {
        return false;
    }
    const auto& bound = *attacks.m_resources;
    if (bound.multiplayer != nullptr && *bound.multiplayer != MultiplayerMode::Normal) {
        return false;
    }
    usize work = 0;
    for (usize index = 0; index < attacks.m_strikes.count(); ++index) {
        const auto& strike = attacks.m_strikes.strike(index);
        if (std::ranges::find(attacks.m_strikeSources, strike.id,
                              &PlayerAttacks::StrikeSource::strike) ==
            attacks.m_strikeSources.end()) {
            return false;
        }
        if (!strike.flies || !strike.collidesWorld) {
            continue;
        }
        // Match MoveStrikes::update before its float-to-integer conversion.
        const f32 liveTime = std::clamp(strike.secondsLeft, 0.0f, seconds);
        const f32 travelTime = liveTime - std::min(strike.launchIn, liveTime);
        const f32 steps = std::max(1.0f, std::ceil(strike.speed * travelTime / 0.25f));
        if (!std::isfinite(steps) || steps > static_cast<f32>(kMaxWallSteps)) {
            return false;
        }
        work += static_cast<usize>(steps);
        if (work > kMaxTotalWallSteps) {
            return false;
        }
    }
    const auto hits = attacks.advanceStrikes(seconds, party);
    for (const auto& hit : hits) {
        const auto source = std::ranges::find(attacks.m_strikeSources, hit.strike,
                                              &PlayerAttacks::StrikeSource::strike);
        if (source == attacks.m_strikeSources.end()) {
            continue; // The native phase likewise ignores an orphaned hit.
        }
        if (bound.world.fallingScenery().wouldShoot(hit.centre, hit.radius)) {
            return false;
        }
        // This is the same ordered geometry/cone/cooldown query used immediately
        // below by native dispatch, including stationary swarm height handling.
        const auto contacts = attacks.strikeContacts(hit, *source, targets);
        for (const auto& target : contacts) {
            if (target.id < PlayerAttacks::kEnemyTargetBase ||
                target.id >= PlayerAttacks::kGeneratorTargetBase) {
                return false;
            }
            const auto& enemies = targets.opponents.enemies();
            const s32 slot = target.id - PlayerAttacks::kEnemyTargetBase;
            const s32 kind = enemies.kindOf(slot);
            if (!enemies.observe(slot) || kind < 0 || kind == kDeathKind || kind == kItKind ||
                kind == kGarmBroodKind || enemies.algorithmOf(slot) == kSuicideWay) {
                return false;
            }
        }
        attacks.dispatchStrikeHit(hit, party, targets);
    }
    attacks.finishStrikes();
    return true;
}

} // namespace gdl::game
