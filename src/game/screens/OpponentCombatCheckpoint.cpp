#include "game/screens/OpponentCombatCheckpoint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "game/screens/HelpMessages.h"
#include "game/screens/LevelOpponents.h"

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
bool references(const OpponentCombatCheckpoint::State& state,
                const EffectTrees::PoolCheckpoint& effects) {
    // Expired cues legitimately remain until followCritterEffects. Retain them,
    // but never admit an identifier the shared pool has not issued.
    return effects.valid() && std::ranges::all_of(state.cueEffects, [&](u32 effect) {
               return effects.nextId == 0 || effect < effects.nextId;
           });
}
} // namespace

bool OpponentCombatCheckpoint::State::valid() const {
    if (lessons.size() > kMaxRecords || generatorRewards.size() > kMaxRecords ||
        rockHits.size() > kMaxRecords || cueEffects.size() > kMaxRecords || hearers.size() > 4 ||
        !finite(shrink) || shrink <= 0 || shrink > 1 ||
        std::ranges::any_of(hitStreak, [](s32 value) { return value < 0; })) {
        return false;
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
        !owner.m_barrelRams.empty() || !owner.m_critterEffects.empty() ||
        !owner.m_moveEffects.empty() || owner.m_deathSound != kNoSound || owner.m_deathContact ||
        !owner.m_yells.empty() || owner.m_bossMeter.bound() ||
        std::ranges::any_of(owner.m_carried, [](const auto& item) { return item.has_value(); }) ||
        std::ranges::any_of(owner.m_deathEffects, [](u32 effect) { return effect != 0; }) ||
        std::ranges::any_of(owner.m_critterExperienceOwed, [](f32 owed) { return owed != 0; })) {
        return false;
    }
    const auto& critters = owner.m_critters;
    if (!critters.m_stocks.empty() || !critters.m_blows.empty() || !critters.m_grabs.empty() ||
        !critters.m_losses.empty() || !critters.m_cues.empty() || !critters.m_spews.empty() ||
        !critters.m_shots.empty() || !critters.m_rams.empty() || !critters.m_pushes.empty() ||
        !std::ranges::all_of(critters.m_critters, unused)) {
        return false;
    }
    const auto& boss = owner.m_bosses;
    if (!unused(boss.m_fighter) || !boss.m_assets.empty() || boss.m_id || boss.m_defeat ||
        boss.m_kind != -1 || boss.m_rite.stage() != LegendRite::Stage::None ||
        !boss.m_legendEvents.empty()) {
        return false;
    }
    const auto& missiles = owner.m_combatantProjectiles;
    if (!missiles.m_flying.empty() || !missiles.m_emittedEffects.empty() ||
        !missiles.m_hits.empty() || !missiles.m_worldHits.empty() || !missiles.m_rockHits.empty() ||
        !missiles.m_generators.empty() || !missiles.m_summons.empty() ||
        missiles.m_ricochetIn != 0) {
        return false;
    }
    const auto& statues = owner.m_statues;
    if (!statues.m_statues.empty() || !statues.m_prototypes.empty() || !statues.m_risen.empty()) {
        return false;
    }
    const auto& swarm = owner.m_enemies;
    if (!swarm.m_deathEvents.empty() || !swarm.m_deathShots.empty() || !swarm.m_tagged.empty() ||
        std::ranges::any_of(swarm.m_enemies, [](const auto& enemy) {
            return enemy.state != Enemies::State::Inactive && !ordinary(enemy.kind);
        })) {
        return false;
    }
    return std::ranges::all_of(owner.m_placements.roster(), [](const auto& placement) {
        return ordinary(placement.kind) && !placement.carried;
    });
}

struct OpponentCombatCheckpoint::Prepared::Data {
    const OpponentCombatCheckpoint* owner = nullptr;
    std::shared_ptr<const u8> lifetime;
    u64 generation = 0;
    u64 binding = 0;
    State state;
    std::vector<LevelOpponents::GeneratorReward> rewards;
};

OpponentCombatCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data)
    : m_data(std::move(data)) {}
OpponentCombatCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
OpponentCombatCheckpoint::Prepared&
OpponentCombatCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
OpponentCombatCheckpoint::Prepared::~Prepared() = default;

OpponentCombatCheckpoint::OpponentCombatCheckpoint(LevelOpponents& owner) : m_owner(owner) {}
OpponentCombatCheckpoint::~OpponentCombatCheckpoint() = default;

std::optional<OpponentCombatCheckpoint::State>
OpponentCombatCheckpoint::checkpoint(const EffectTrees::PoolCheckpoint& effects) const {
    if (!supported()) {
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
    return state.valid() && references(state, effects) ? std::optional{std::move(state)}
                                                       : std::nullopt;
}

std::optional<OpponentCombatCheckpoint::Prepared>
OpponentCombatCheckpoint::prepareRestore(const State& state,
                                         const EffectTrees::PoolCheckpoint& effects) const {
    if (!supported() || m_generation == std::numeric_limits<u64>::max() || !state.valid() ||
        !references(state, effects)) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->owner = this;
    data->lifetime = m_lifetime;
    data->generation = m_generation;
    data->binding = m_owner.m_bindingGeneration;
    data->state = state;
    data->rewards.reserve(state.generatorRewards.size());
    for (const auto& reward : state.generatorRewards) {
        data->rewards.push_back({reward.player, reward.experience, reward.destroyed});
    }
    return Prepared{std::move(data)};
}

bool OpponentCombatCheckpoint::canCommit(const Prepared& prepared) const {
    return prepared.m_data && prepared.m_data->owner == this &&
           prepared.m_data->lifetime == m_lifetime && prepared.m_data->generation == m_generation &&
           m_generation != std::numeric_limits<u64>::max() &&
           prepared.m_data->binding == m_owner.m_bindingGeneration && supported();
}

bool OpponentCombatCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    m_owner.m_hitStreak = data->state.hitStreak;
    m_owner.m_lessons.swap(data->state.lessons);
    m_owner.m_generatorRewards.swap(data->rewards);
    m_owner.m_rockHits.swap(data->state.rockHits);
    m_owner.m_hearers.swap(data->state.hearers);
    m_owner.m_cueEffects.swap(data->state.cueEffects);
    m_owner.m_shrink = data->state.shrink;
    m_owner.m_bagSeed = data->state.bagSeed;
    ++m_generation;
    return true;
}

bool OpponentCombatCheckpoint::restore(const State& state,
                                       const EffectTrees::PoolCheckpoint& effects) {
    auto prepared = prepareRestore(state, effects);
    return prepared && commitRestore(std::move(*prepared));
}

} // namespace gdl::game
