#include "game/screens/OrdinaryCritterPhase.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "game/screens/CritterCueEmission.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/PartyRecords.h"

namespace gdl::game {
namespace {
bool cueSupported(const CombatCue& cue) {
    return !cue.shakes && !cue.arena && !cue.playerAttachment;
}
bool lossSupported(const CombatLoss& loss) {
    return !loss.killed && !loss.drop;
}
bool eventsSupported(const CombatantCheckpoint::Events& events) {
    return events.grabs.empty() && events.spews.empty() && events.shots.empty() &&
           events.arena.empty() && events.rams.empty() &&
           std::ranges::all_of(events.cues, cueSupported) &&
           std::ranges::all_of(events.losses, lossSupported);
}
bool ordinary(CombatantKind kind) {
    return kind == CombatantKind::General || kind == CombatantKind::Golem ||
           kind == CombatantKind::Gargoyle;
}
} // namespace

bool OrdinaryCritterPhase::supported(const OpponentCombatCheckpoint::State& state) {
    const auto& missiles = state.projectiles;
    if (!eventsSupported(state.critters.events) || !state.bags.empty() ||
        std::ranges::any_of(state.carried, [](const auto& item) { return item.has_value(); }) ||
        std::ranges::any_of(state.critterEffects,
                            [](const auto& effect) {
                                return effect.ofBoss || effect.playerAttachment.has_value();
                            }) ||
        std::ranges::any_of(state.moveEffects, [](const auto& effect) { return effect.ofBoss; }) ||
        !state.barrelRams.empty() || !missiles.flying.empty() || !missiles.emittedEffects.empty() ||
        !missiles.hits.empty() || !missiles.worldHits.empty() || !missiles.rockHits.empty() ||
        !missiles.generators.empty() || !missiles.summons.empty() || missiles.ricochetIn != 0) {
        return false;
    }
    for (const auto& slot : state.critters.actors) {
        if (slot.parts.size() != 1 || !eventsSupported(slot.parts.front().events)) {
            return false;
        }
        const auto& actor = slot.parts.front().actor;
        if (actor.state == 0 && actor.definition.resource == 0) {
            continue;
        }
        const auto stock = std::ranges::find(state.critters.banks, actor.definition.resource,
                                             &CrittersCheckpoint::Bank::id);
        if (actor.state != 1 || actor.health <= 0 || stock == state.critters.banks.end() ||
            !ordinary(stock->kind) || actor.definition.index != 0 || actor.parent != -1 ||
            actor.grabbed != -1 || !actor.arenaAnchors.empty() || !actor.arenaTargets.empty()) {
            return false;
        }
    }
    // The enclosing native capture/restore already excludes boss/Death/cloud
    // owners. Keep its separate statue, hold and stage-contact gates. Only the
    // ordinary population's AI, local following cues and fractional nonlethal
    // reward ledger are added here; carried loot and player attachments are not.
    return true;
}

bool OrdinaryCritterPhase::outputs(const Critters& pool) {
    return pool.m_grabs.empty() && pool.m_spews.empty() && pool.m_shots.empty() &&
           pool.m_rams.empty() && pool.m_cues.size() <= CombatantCheckpoint::kMaxEvents &&
           pool.m_blows.size() <= CombatantCheckpoint::kMaxEvents &&
           pool.m_losses.size() <= CombatantCheckpoint::kMaxEvents &&
           std::ranges::all_of(pool.m_cues, cueSupported) &&
           std::ranges::all_of(pool.m_losses, lossSupported) &&
           std::ranges::all_of(pool.fighters(), [](const Combatant& actor) {
               return !actor.present() || (actor.alive() && ordinary(actor.kind()));
           });
}

OrdinaryCritterPhase::Result OrdinaryCritterPhase::advance(
    LevelOpponents& owner, s32 ticks, f32 seconds, std::span<const EnemyView> views,
    std::span<const CombatantObstacle> fixtures, std::span<PlayerRuntime> party) {
    if (!owner.m_resources || ticks < 1 || ticks > 2 || !std::isfinite(seconds) ||
        std::abs(seconds * 60 - static_cast<f32>(ticks)) > 0.0001f || views.size() > 4 ||
        party.size() > 4 || !outputs(owner.m_critters)) {
        return {Refusal::State};
    }
    owner.advanceCritters(ticks, seconds, views, false, fixtures, party);
    // Native CPU work may produce an unsupported event late in this phase.
    // Leave those queues intact for the encompassing undo; never consume and
    // silently pretend that the corresponding projectile/grab/drop happened.
    return {outputs(owner.m_critters) ? Refusal::None : Refusal::Output};
}

OrdinaryCritterPhase::Result
OrdinaryCritterPhase::finish(LevelOpponents& owner, std::span<PlayerRuntime> party,
                             PlayerHealth& health, const ClassDataSet& classes,
                             const PartyDamage::Context& context,
                             std::span<const EffectTrees::PoolResource> resources,
                             const std::function<bool(const Vec3&, const Vec3&)>& blocksBreath,
                             const std::function<bool(const Vec3&, const Vec3&)>& blocksArea) {
    if (!owner.m_resources || !outputs(owner.m_critters) || !context.valid()) {
        return {Refusal::State};
    }
    auto& effects = owner.m_resources->effects;
    // Preflight the complete current cue batch against already-bound native
    // resources before starting any effect. A later damage refusal still uses
    // the caller's shared effect/actor/player rewind.
    for (const auto& cue : owner.m_critters.m_cues) {
        if (cue.stopMoveEffect) {
            continue;
        }
        const auto emission = CritterCueEmission::resolve(
            cue, owner.m_critters.archiveOf(cue.critter), owner.m_resources->weapons);
        if (emission.archive && !emission.prepare(resources, effects.textureLenders())) {
            return {Refusal::Resources};
        }
    }
    Result result;
    for (const auto& cue : owner.m_critters.takeCues()) {
        if (!owner.showCritterCue(cue, owner.m_critters.archiveOf(cue.critter), false,
                                  [&](const CritterCueEmission& emission) -> std::optional<u32> {
                                      const auto prepared =
                                          emission.prepare(resources, effects.textureLenders());
                                      if (!prepared) {
                                          return std::nullopt;
                                      }
                                      const auto id = effects.startPrepared(
                                          *prepared, emission.position, emission.setting);
                                      return id != 0 ? std::optional{id} : std::nullopt;
                                  })) {
            return {Refusal::Resources};
        }
        if (!cue.stopMoveEffect && !cue.sound.empty()) {
            constexpr f32 kLoud = 224.0f / 255;
            const f32 volume =
                cue.attenuated
                    ? kLoud * LevelOpponents::attenuation(cue.soundPosition, owner.m_hearers)
                    : kLoud;
            if (volume > 0) {
                result.sounds.push_back({cue.sound, cue.soundPosition, volume});
            }
        }
    }
    owner.followCritterEffects(party);
    bool refused = false;
    const LevelOpponents::Events events{
        .hurt =
            [&](usize index, f32 amount, HurtKind kind, bool directed, const PlayerImpact& impact) {
                auto hit = PartyDamage::stageHit(party[index], party, health, classes, amount, kind,
                                                 directed, context, impact);
                refused |= hit.status != PartyDamage::Status::Applied;
                result.damage.insert(result.damage.end(),
                                     std::make_move_iterator(hit.outputs.begin()),
                                     std::make_move_iterator(hit.outputs.end()));
            },
        .award =
            [&](s32 player, s32 amount, bool killed) {
                PartyRecords::award(party, player, amount, killed,
                                    owner.m_resources->world.level());
            },
        .blocksBreath = blocksBreath,
        .blocksArea = blocksArea};
    for (const auto& blow : owner.m_critters.takeBlows()) {
        if (LevelOpponents::applyCritterBlow(blow, party, events)) {
            if (refused) {
                return {Refusal::Damage};
            }
            owner.m_critters.damagedPlayer(blow.critter, blow.player, blow.damage);
        }
    }
    owner.awardCritterLosses(party, events);
    return result;
}
} // namespace gdl::game
