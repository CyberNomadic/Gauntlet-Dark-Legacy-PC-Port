#include "game/screens/OpponentCombatCheckpoint.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <unordered_set>

#include "game/screens/HelpMessages.h"
#include "game/screens/LevelOpponents.h"
#include "game/world/FixtureCheckpointDetail.h"

namespace gdl::game {
namespace {
bool finite(f32 value) {
    return std::isfinite(value) && std::abs(value) <= 1e6f;
}
bool finite(const Vec3& value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}
bool ordinary(s32 kind) {
    return kind >= 0 && kind < kSwarmKindCount && kind != kGarmBroodKind;
}
bool effectReferences(const OpponentCombatCheckpoint::State& state,
                      const EffectTrees::PoolCheckpoint& effects) {
    // Expired cues legitimately remain until followCritterEffects. Retain them,
    // but never admit an identifier the shared pool has not issued.
    const auto issued = [&](u32 effect) { return effects.nextId == 0 || effect < effects.nextId; };
    return std::ranges::all_of(state.cueEffects, issued) &&
           std::ranges::all_of(state.critterEffects,
                               [&](const auto& effect) { return issued(effect.effect); }) &&
           std::ranges::all_of(state.moveEffects,
                               [&](const auto& effect) { return issued(effect.effect); });
}
} // namespace

OpponentCombatCheckpoint::State::State() {
    // A genuinely unopened empty population has one unused root per slot and
    // the same empty native lookout route, not an omitted population payload.
    for (auto& actor : critters.actors) {
        actor.parts.emplace_back();
        actor.lookouts = critters.lookouts;
    }
}

bool OpponentCombatCheckpoint::State::valid() const {
    return statues.valid() && critters.valid() && projectiles.valid() && validAggregate();
}
bool OpponentCombatCheckpoint::State::validAggregate() const {
    if (lessons.size() > kMaxRecords || generatorRewards.size() > kMaxRecords ||
        rockHits.size() > kMaxRecords || cueEffects.size() > kMaxRecords || hearers.size() > 4 ||
        !finite(shrink) || shrink <= 0 || shrink > 1 ||
        std::ranges::any_of(hitStreak, [](s32 value) { return value < 0; }) ||
        critterEffects.size() > kMaxRecords || moveEffects.size() > kMaxRecords ||
        barrelRams.size() > kMaxRecords || projectiles.catalog.size() != critters.banks.size()) {
        return false;
    }
    for (usize index = 0; index < projectiles.catalog.size(); ++index) {
        if (projectiles.catalog[index].id != critters.banks[index].id ||
            projectiles.catalog[index].kind != critters.banks[index].kind) {
            return false;
        }
    }
    if (!std::ranges::all_of(critterExperienceOwed,
                             [](f32 value) { return finite(value) && value >= 0 && value < 1; }) ||
        !std::ranges::all_of(barrelRams, [](const Ram& ram) {
            return ram.id >= -1 && finite(ram.damage) && ram.damage >= 0;
        })) {
        return false;
    }
    const auto critter = [](s32 id) { return id >= 0 && id < Critters::kMost; };
    for (const auto& effect : critterEffects) {
        if (effect.effect == 0 || effect.ofBoss || !critter(effect.critter) ||
            !finite(effect.offset) || !finite(effect.nodeOffset) || !finite(effect.pitchYaw.x) ||
            !finite(effect.pitchYaw.y) ||
            (effect.node && !fixture_checkpoint::name(*effect.node)) ||
            (effect.playerAttachment &&
             (*effect.playerAttachment < 0 || *effect.playerAttachment >= 4))) {
            return false;
        }
    }
    for (const auto& effect : moveEffects) {
        if (effect.effect == 0 || effect.ofBoss || !critter(effect.critter)) {
            return false;
        }
    }
    for (const auto& [lesson, player] : lessons) {
        if (lesson != HelpMessages::kDestroyGenerators || player < 0 || player >= 4) {
            return false;
        }
    }
    for (const auto& reward : generatorRewards) {
        if (reward.player < 0 || reward.player >= 4 || reward.experience < 0) {
            return false;
        }
    }
    for (const auto& hit : rockHits) {
        if (hit.rock > kMaxRecords || !finite(hit.damage) || hit.damage < 0) {
            return false;
        }
    }
    if (!std::ranges::all_of(hearers, [](const Vec3& value) { return finite(value); })) {
        return false;
    }
    std::unordered_set<u32> effects;
    for (const u32 effect : cueEffects) {
        if (effect == 0 || !effects.insert(effect).second) {
            return false;
        }
    }
    return true;
}

bool OpponentCombatCheckpoint::State::dormant() const {
    const auto quiet = [](const CombatantCheckpoint::Events& events) {
        return events.cues.empty() && events.grabs.empty() && events.spews.empty() &&
               events.shots.empty() && events.arena.empty() && events.tramples.empty() &&
               events.rams.empty() && events.pushes.empty() && events.blows.empty() &&
               events.losses.empty();
    };
    return quiet(critters.events) &&
           std::ranges::all_of(critters.actors,
                               [&](const auto& slot) {
                                   return slot.parts.size() == 1 &&
                                          slot.parts.front().actor.state == 0 &&
                                          slot.parts.front().actor.definition.resource == 0 &&
                                          quiet(slot.parts.front().events);
                               }) &&
           projectiles.flying.empty() && projectiles.emittedEffects.empty() &&
           projectiles.hits.empty() && projectiles.worldHits.empty() &&
           projectiles.rockHits.empty() && projectiles.generators.empty() &&
           projectiles.summons.empty() && projectiles.ricochetIn == 0 && critterEffects.empty() &&
           moveEffects.empty() && barrelRams.empty() &&
           std::ranges::all_of(critterExperienceOwed, [](f32 value) { return value == 0; });
}

bool OpponentCombatCheckpoint::State::references(u8 playerMask,
                                                 const EffectTrees::PoolCheckpoint& effects) const {
    const auto player = [&](s32 value) {
        return value == -1 ||
               (value >= 0 && value < 4 && (playerMask & (u8{1} << static_cast<u32>(value))) != 0);
    };
    const auto issued = [&](u32 id) {
        return id != 0 && (effects.nextId == 0 || id < effects.nextId);
    };
    const auto events = [&](const CombatantCheckpoint::Events& values) {
        return std::ranges::all_of(values.grabs, [&](const auto& v) { return player(v.player); }) &&
               std::ranges::all_of(values.pushes,
                                   [&](const auto& v) { return player(v.player); }) &&
               std::ranges::all_of(values.blows, [&](const auto& v) { return player(v.player); }) &&
               std::ranges::all_of(values.losses,
                                   [&](const auto& v) { return player(v.player); }) &&
               std::ranges::all_of(values.cues, [&](const auto& v) {
                   return !v.playerAttachment || player(*v.playerAttachment);
               });
    };
    if (!events(critters.events) || !std::ranges::all_of(cueEffects, issued)) {
        return false;
    }
    for (usize index = 0; index < critterExperienceOwed.size(); ++index) {
        if (critterExperienceOwed[index] != 0 && !player(static_cast<s32>(index))) {
            return false;
        }
    }
    for (const auto& slot : critters.actors) {
        for (const auto& part : slot.parts) {
            const auto& actor = part.actor;
            if (!events(part.events) || !player(actor.target) || !player(actor.moveTarget) ||
                !player(actor.grabbed) || !std::ranges::all_of(actor.struckThisMove, player) ||
                !std::ranges::all_of(actor.targets,
                                     [&](const auto& target) { return player(target.player); })) {
                return false;
            }
        }
    }
    return std::ranges::all_of(critterEffects,
                               [&](const auto& v) {
                                   return issued(v.effect) &&
                                          (!v.playerAttachment || player(*v.playerAttachment));
                               }) &&
           std::ranges::all_of(moveEffects, [&](const auto& v) { return issued(v.effect); }) &&
           std::ranges::all_of(projectiles.emittedEffects, issued) &&
           std::ranges::all_of(
               projectiles.flying,
               [&](const auto& v) { return issued(v.effect) && player(v.piercedPlayer); }) &&
           std::ranges::all_of(projectiles.hits, [&](const auto& v) { return player(v.player); });
}

bool OpponentCombatCheckpoint::unused(const Combatant& actor) {
    return !actor.present() && actor.m_actor.stock == nullptr && actor.m_children.empty() &&
           actor.m_cues.empty() && actor.m_grabs.empty() && actor.m_spews.empty() &&
           actor.m_shots.empty() && actor.m_arenaActivations.empty() && actor.m_tramples.empty() &&
           actor.m_rams.empty() && actor.m_pushes.empty() && actor.m_blows.empty() &&
           actor.m_losses.empty();
}

bool OpponentCombatCheckpoint::supported() const {
    const auto& owner = m_owner;
    if (!owner.m_resources || owner.m_bindingGeneration == 0 ||
        (owner.m_resources->world.level() && owner.m_resources->world.level()->bossType >= 0) ||
        !owner.m_clouds.empty() || !owner.m_deathShots.empty() || !owner.m_bags.empty() ||
        owner.m_deathSound != kNoSound || owner.m_deathContact || !owner.m_yells.empty() ||
        owner.m_bossMeter.bound() ||
        std::ranges::any_of(owner.m_carried, [](const auto& item) { return item.has_value(); }) ||
        std::ranges::any_of(owner.m_deathEffects, [](u32 effect) { return effect != 0; })) {
        return false;
    }
    const auto& boss = owner.m_bosses;
    if (!unused(boss.m_fighter) || !boss.m_assets.empty() || boss.m_id || boss.m_defeat ||
        boss.m_kind != -1 || boss.m_rite.stage() != LegendRite::Stage::None ||
        !boss.m_legendEvents.empty()) {
        return false;
    }
    const auto& swarm = owner.m_enemies;
    if (!swarm.m_deathEvents.empty() || !swarm.m_deathShots.empty() || !swarm.m_tagged.empty() ||
        std::ranges::any_of(swarm.m_enemies, [](const auto& enemy) {
            return enemy.state != Enemies::State::Inactive && !ordinary(enemy.kind);
        })) {
        return false;
    }
    // Pending definitions and statues are value-owned separately. Merely
    // retaining a special placement is not an active special combatant.
    return true;
}

struct OpponentCombatCheckpoint::Prepared::Data {
    const OpponentCombatCheckpoint* owner = nullptr;
    std::shared_ptr<const u8> lifetime;
    u64 generation = 0;
    u64 binding = 0;
    State state;
    std::vector<LevelOpponents::GeneratorReward> rewards;
    std::optional<CritterStatues::Prepared> statues;
    std::unique_ptr<CrittersCheckpoint> critterOwner;
    std::unique_ptr<CombatantProjectilesCheckpoint> projectileOwner;
    std::optional<CrittersCheckpoint::Prepared> critters;
    std::optional<CombatantProjectilesCheckpoint::Prepared> projectiles;
    std::vector<LevelOpponents::CritterEffect> critterEffects;
    std::vector<LevelOpponents::MoveEffect> moveEffects;
    std::vector<CombatantRam> barrelRams;
};

struct OpponentCombatCheckpoint::CaptureResources {
    u64 binding;
    EffectTrees* effects;
    CrittersCheckpoint critters;
    CombatantProjectilesCheckpoint projectiles;

    CaptureResources(u64 generation, Critters& population, CombatantProjectiles& missiles,
                     EffectTrees& sharedEffects,
                     std::span<const EffectTrees::PoolResource> resources)
        : binding(generation), effects(&sharedEffects), critters(population),
          projectiles(missiles, sharedEffects, critters.resources(), resources) {}
};

OpponentCombatCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data)
    : m_data(std::move(data)) {}
OpponentCombatCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
OpponentCombatCheckpoint::Prepared&
OpponentCombatCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
OpponentCombatCheckpoint::Prepared::~Prepared() = default;

OpponentCombatCheckpoint::OpponentCombatCheckpoint(
    LevelOpponents& owner, std::span<const EffectTrees::PoolResource> resources)
    : m_owner(owner), m_effectResources(resources.begin(), resources.end()) {}
OpponentCombatCheckpoint::~OpponentCombatCheckpoint() = default;

std::optional<OpponentCombatCheckpoint::State>
OpponentCombatCheckpoint::checkpoint(const EffectTrees::PoolCheckpoint& effects) const {
    if (!supported()) {
        return std::nullopt;
    }
    const auto bindCapture = [&] {
        auto resources = std::make_unique<CaptureResources>(
            m_owner.m_bindingGeneration, m_owner.m_critters, m_owner.m_combatantProjectiles,
            m_owner.m_resources->effects, m_effectResources);
        if (!resources->critters.bound()) {
            return false;
        }
        m_captureResources = std::move(resources);
        return true;
    };
    // The catalog is immutable during ordinary updates. Test the population's
    // binding before dereferencing any cached stock after close/reopen or loads.
    if ((!m_captureResources || m_captureResources->binding != m_owner.m_bindingGeneration ||
         m_captureResources->effects != &m_owner.m_resources->effects ||
         !m_captureResources->critters.bound()) &&
        !bindCapture()) {
        return std::nullopt;
    }
    auto critters = m_captureResources->critters.checkpoint();
    if (!critters) {
        // An independently reloaded stock can retain its address while changing
        // its resource token. Rebind once, preserving the fresh-wrapper capture
        // contract; malformed live values still fail the same full leaf checks.
        if (!bindCapture()) {
            return std::nullopt;
        }
        critters = m_captureResources->critters.checkpoint();
    }
    auto projectiles = m_captureResources->projectiles.checkpoint(effects);
    auto statues = m_owner.m_statues.checkpoint();
    if (!statues || !critters || !projectiles) {
        return std::nullopt;
    }
    State state;
    state.hitStreak = m_owner.m_hitStreak;
    state.lessons = m_owner.m_lessons;
    state.generatorRewards.reserve(m_owner.m_generatorRewards.size());
    for (const auto& reward : m_owner.m_generatorRewards) {
        state.generatorRewards.push_back({reward.player, reward.experience, reward.destroyed});
    }
    state.rockHits = m_owner.m_rockHits;
    state.hearers = m_owner.m_hearers;
    state.cueEffects = m_owner.m_cueEffects;
    state.shrink = m_owner.m_shrink;
    state.bagSeed = m_owner.m_bagSeed;
    state.statues = std::move(*statues);
    state.critters = std::move(*critters);
    state.projectiles = std::move(*projectiles);
    state.critterExperienceOwed = m_owner.m_critterExperienceOwed;
    for (const auto& effect : m_owner.m_critterEffects) {
        state.critterEffects.push_back({effect.effect, effect.critter, effect.ofBoss, effect.offset,
                                        effect.node, effect.nodeOffset, effect.rootAttachment,
                                        effect.pitchYaw, effect.playerAttachment});
    }
    for (const auto& effect : m_owner.m_moveEffects) {
        state.moveEffects.push_back({effect.effect, effect.critter, effect.ofBoss});
    }
    for (const auto& ram : m_owner.m_barrelRams) {
        state.barrelRams.push_back({ram.id, ram.damage});
    }
    // All children, including the supplied shared effects, were validated by
    // their captures. Preserve the adapter's scalars and cross-owner references.
    return state.validAggregate() && effectReferences(state, effects)
               ? std::optional{std::move(state)}
               : std::nullopt;
}

std::optional<OpponentCombatCheckpoint::Prepared>
OpponentCombatCheckpoint::prepareRestore(const State& state,
                                         const EffectTrees::PoolCheckpoint& effects) const {
    if (!supported() || m_generation == std::numeric_limits<u64>::max() || !state.valid() ||
        !effects.valid() || !effectReferences(state, effects)) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->owner = this;
    data->lifetime = m_lifetime;
    data->generation = m_generation;
    data->binding = m_owner.m_bindingGeneration;
    data->state = state;
    data->statues = m_owner.m_statues.prepareRestore(state.statues);
    data->critterOwner = std::make_unique<CrittersCheckpoint>(m_owner.m_critters);
    data->projectileOwner = std::make_unique<CombatantProjectilesCheckpoint>(
        m_owner.m_combatantProjectiles, m_owner.m_resources->effects,
        data->critterOwner->resources(), m_effectResources);
    data->critters = data->critterOwner->prepareRestore(state.critters);
    data->projectiles = data->projectileOwner->prepareRestore(state.projectiles, effects);
    if (!data->statues || !data->critters || !data->projectiles) {
        return std::nullopt;
    }
    for (const auto& effect : state.critterEffects) {
        data->critterEffects.push_back({effect.effect, effect.critter, effect.ofBoss, effect.offset,
                                        effect.node, effect.nodeOffset, effect.rootAttachment,
                                        effect.pitchYaw, effect.playerAttachment});
    }
    for (const auto& effect : state.moveEffects) {
        data->moveEffects.push_back({effect.effect, effect.critter, effect.ofBoss});
    }
    for (const auto& ram : state.barrelRams) {
        data->barrelRams.push_back({ram.id, ram.damage});
    }
    data->rewards.reserve(state.generatorRewards.size());
    for (const auto& reward : state.generatorRewards) {
        data->rewards.push_back({reward.player, reward.experience, reward.destroyed});
    }
    return Prepared{std::move(data)};
}

bool OpponentCombatCheckpoint::canCommit(const Prepared& prepared) const {
    if (!(prepared.m_data && prepared.m_data->owner == this &&
          prepared.m_data->lifetime == m_lifetime && prepared.m_data->generation == m_generation &&
          m_generation != std::numeric_limits<u64>::max() &&
          prepared.m_data->binding == m_owner.m_bindingGeneration && supported())) {
        return false;
    }
    const auto& data = *prepared.m_data;
    return data.statues && m_owner.m_statues.canCommit(*data.statues) && data.critterOwner &&
           data.projectileOwner && data.critters && data.projectiles &&
           data.critterOwner->canCommit(*data.critters) &&
           data.projectileOwner->canCommit(*data.projectiles);
}

bool OpponentCombatCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    if (!m_owner.m_statues.commitRestore(std::move(*data->statues)) ||
        !data->critterOwner->commitRestore(std::move(*data->critters)) ||
        !data->projectileOwner->commitRestore(std::move(*data->projectiles))) {
        std::terminate(); // The joint preflight succeeded; no callbacks may interleave.
    }
    m_owner.m_hitStreak = data->state.hitStreak;
    m_owner.m_lessons.swap(data->state.lessons);
    m_owner.m_generatorRewards.swap(data->rewards);
    m_owner.m_rockHits.swap(data->state.rockHits);
    m_owner.m_hearers.swap(data->state.hearers);
    m_owner.m_cueEffects.swap(data->state.cueEffects);
    m_owner.m_shrink = data->state.shrink;
    m_owner.m_bagSeed = data->state.bagSeed;
    m_owner.m_critterExperienceOwed = data->state.critterExperienceOwed;
    m_owner.m_critterEffects.swap(data->critterEffects);
    m_owner.m_moveEffects.swap(data->moveEffects);
    m_owner.m_barrelRams.swap(data->barrelRams);
    ++m_generation;
    return true;
}

bool OpponentCombatCheckpoint::restore(const State& state,
                                       const EffectTrees::PoolCheckpoint& effects) {
    auto prepared = prepareRestore(state, effects);
    return prepared && commitRestore(std::move(*prepared));
}

} // namespace gdl::game
