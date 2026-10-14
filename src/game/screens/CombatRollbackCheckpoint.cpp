#include "game/screens/CombatRollbackCheckpoint.h"

#include <algorithm>
#include <exception>
#include <type_traits>
#include <utility>

namespace gdl::game {
namespace {
constexpr usize kEffectResources =
    EffectTrees::PoolCheckpoint::kMaxEffects + EffectTrees::PoolCheckpoint::kMaxBanks;
std::vector<PlayerAttacksCheckpoint::Actor>
actorBindings(std::span<const PlayerRuntimeCheckpoint::State> players) {
    std::vector<PlayerAttacksCheckpoint::Actor> actors;
    actors.reserve(players.size());
    for (const auto& player : players) {
        actors.push_back({player.player, player.save.character});
    }
    return actors;
}
std::vector<s32> playerSlots(std::span<const PlayerRuntimeCheckpoint::State> players) {
    std::vector<s32> slots;
    slots.reserve(players.size());
    for (const auto& player : players) {
        slots.push_back(player.player);
    }
    return slots;
}
} // namespace

bool CombatRollbackCheckpoint::State::valid() const {
    return players.size() <= 4 && attacks.valid() && arsenal.valid() && effects.valid() &&
           std::ranges::all_of(players, [](const auto& player) { return player.valid(); }) &&
           validAggregate();
}

bool CombatRollbackCheckpoint::State::validAggregate() const {
    if (players.size() > 4 || !health.valid() || !levels.valid() ||
        attacks.actors.size() != players.size()) {
        return false;
    }
    u32 occupied = 0;
    for (usize index = 0; index < players.size(); ++index) {
        const auto& player = players[index];
        if (player.player < 0 || player.player >= 4 || (occupied & (1U << player.player)) != 0 ||
            attacks.actors[index] !=
                PlayerAttacksCheckpoint::Actor{player.player, player.save.character}) {
            return false;
        }
        occupied |= 1U << player.player;
    }
    const auto present = [&](s32 slot) {
        return slot >= 0 && slot < 4 && (occupied & (1U << slot)) != 0;
    };
    for (const auto& level : levels.marks) {
        if (!present(level.player)) {
            return false;
        }
    }
    for (const auto& cast : arsenal.casts) {
        if (!present(cast.owner)) {
            return false;
        }
    }
    // A shot can outlive its shooter, but its complete participant must still be
    // in this transaction (including fallen/departed members). Never silently
    // accept a flight or pending impact whose gameplay recipient was omitted.
    for (const auto& missile : arsenal.missiles.missiles) {
        if (!present(missile.owner)) {
            return false;
        }
        for (const auto& contact : missile.playerContacts) {
            if (!present(contact.player)) {
                return false;
            }
        }
    }
    for (const auto& impact : arsenal.missiles.impacts) {
        if ((impact.owner >= 0 && !present(impact.owner)) ||
            (impact.player >= 0 && !present(impact.player))) {
            return false;
        }
    }
    for (const auto& history : arsenal.missiles.history) {
        if (!present(history.owner)) {
            return false;
        }
    }
    for (const auto& potion : attacks.potions) {
        if (potion.impact.player >= 0 && !present(potion.impact.player)) {
            return false;
        }
    }
    return true;
}

struct CombatRollbackCheckpoint::Prepared::Data {
    const CombatRollbackCheckpoint* owner = nullptr;
    std::weak_ptr<const u8> lifetime;
    u64 generation = 0;
    std::vector<PlayerRuntimeCheckpoint::Prepared> players;
    PlayerHealth::Checkpoint previousHealth;
    PlayerHealth::Checkpoint health;
    LevelWatch::Checkpoint previousLevels;
    LevelWatch levels;
    std::optional<PlayerAttacksCheckpoint::Prepared> attacks;
    std::optional<PlayerArsenalCheckpoint::Prepared> arsenal;
    std::optional<EffectTrees::PreparedRestore> effects;
};
CombatRollbackCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data)
    : m_data(std::move(data)) {}
CombatRollbackCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
CombatRollbackCheckpoint::Prepared&
CombatRollbackCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
CombatRollbackCheckpoint::Prepared::~Prepared() = default;
CombatRollbackCheckpoint::Rewind::Rewind(Prepared&& state) : m_state(std::move(state)) {}
CombatRollbackCheckpoint::Rewind::Rewind(Rewind&&) noexcept = default;
CombatRollbackCheckpoint::Rewind&
CombatRollbackCheckpoint::Rewind::operator=(Rewind&&) noexcept = default;
CombatRollbackCheckpoint::Rewind::~Rewind() = default;

CombatRollbackCheckpoint::CombatRollbackCheckpoint(
    std::span<PlayerRuntimeCheckpoint* const> players, PlayerHealth& health, LevelWatch& levels,
    PlayerAttacks& attacks, PlayerArsenal& arsenal, EffectTrees& effects,
    const Resources& resources)
    : m_health(health), m_levels(levels), m_attacks(attacks), m_arsenal(arsenal),
      m_effects(effects) {
    if (players.size() > 4 ||
        resources.missiles.size() > PlayerMissiles::Checkpoint::kMaxResources ||
        resources.missileEffects.size() > kEffectResources ||
        resources.sharedEffects.size() > kEffectResources) {
        return;
    }
    m_players.assign(players.begin(), players.end());
    m_missiles.assign(resources.missiles.begin(), resources.missiles.end());
    m_missileEffects.assign(resources.missileEffects.begin(), resources.missileEffects.end());
    m_sharedEffects.assign(resources.sharedEffects.begin(), resources.sharedEffects.end());
    m_configurationValid = true;
}
CombatRollbackCheckpoint::~CombatRollbackCheckpoint() = default;

bool CombatRollbackCheckpoint::bindingsValid() const {
    if (!m_configurationValid || m_generation == 0) {
        return false;
    }
    if (m_attacks.m_resources.has_value() != m_arsenal.m_resources.has_value()) {
        return false;
    }
    if (m_attacks.m_resources) {
        const auto& attacks = *m_attacks.m_resources;
        const auto& arsenal = *m_arsenal.m_resources;
        if (&attacks.arsenal != &m_arsenal || &attacks.effects != &m_effects ||
            &arsenal.effects != &m_effects || &attacks.classes != &arsenal.classes ||
            &attacks.world.collision() != &arsenal.collision ||
            &attacks.weapons != &arsenal.weapons || &attacks.device != &arsenal.device) {
            return false;
        }
    }
    for (usize index = 0; index < m_players.size(); ++index) {
        if (m_players[index] == nullptr ||
            std::find(m_players.begin(), m_players.begin() + static_cast<std::ptrdiff_t>(index),
                      m_players[index]) != m_players.begin() + static_cast<std::ptrdiff_t>(index)) {
            return false;
        }
    }
    return true;
}

bool CombatRollbackCheckpoint::binds(const LevelWorld& world, const ItemArchive& weapons,
                                     const EffectTrees& effects, const RenderDevice& device) const {
    return bindingsValid() && m_attacks.m_resources && m_arsenal.m_resources &&
           &m_attacks.m_resources->world == &world && &m_attacks.m_resources->weapons == &weapons &&
           &m_effects == &effects && &m_attacks.m_resources->device == &device;
}

std::optional<CombatRollbackCheckpoint::State> CombatRollbackCheckpoint::checkpoint() const {
    if (!bindingsValid()) {
        return std::nullopt;
    }
    State state;
    state.players.reserve(m_players.size());
    for (const auto* owner : m_players) {
        auto player = owner->checkpoint();
        if (!player) {
            return std::nullopt;
        }
        state.players.push_back(std::move(*player));
    }
    state.health = m_health.checkpoint();
    state.levels = m_levels.checkpoint();
    const auto actors = actorBindings(state.players);
    auto attacks = PlayerAttacksCheckpoint::capture(m_attacks, actors, m_missiles);
    auto arsenal = PlayerArsenalCheckpoint::capture(m_arsenal, m_missiles, m_missileEffects);
    auto effects = m_effects.checkpointPool(m_sharedEffects);
    if (!attacks || !arsenal || !effects) {
        return std::nullopt;
    }
    state.attacks = std::move(*attacks);
    state.arsenal = std::move(*arsenal);
    state.effects = std::move(*effects);
    // Optional children are validated by their capture APIs. Health/level
    // values and all party ownership links are checked by the aggregate.
    return state.validAggregate() ? std::optional{std::move(state)} : std::nullopt;
}

std::optional<CombatRollbackCheckpoint::Prepared>
CombatRollbackCheckpoint::prepareRestore(const State& state) const {
    if (!bindingsValid() || !state.valid() || state.players.size() != m_players.size()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->owner = this;
    data->lifetime = m_lifetime;
    data->generation = m_generation;
    data->previousHealth = m_health.checkpoint();
    data->health = state.health;
    data->previousLevels = m_levels.checkpoint();
    // LevelWatch::restore copies its vector. Stage that work now, never after
    // the first participant commits, and later move the prepared native owner.
    if (!data->levels.restore(state.levels)) {
        return std::nullopt;
    }
    data->players.reserve(m_players.size());
    for (usize index = 0; index < m_players.size(); ++index) {
        auto player = m_players[index]->prepareRestore(state.players[index]);
        if (!player) {
            return std::nullopt;
        }
        data->players.push_back(std::move(*player));
    }
    const auto actors = actorBindings(state.players);
    const auto slots = playerSlots(state.players);
    data->attacks = PlayerAttacksCheckpoint::prepare(m_attacks, state.attacks, actors,
                                                     state.effects, m_missiles);
    data->arsenal = PlayerArsenalCheckpoint::prepare(m_arsenal, state.arsenal, slots, state.effects,
                                                     m_missiles, m_missileEffects);
    // This is the sole restore of the shared world effect pool. The arsenal's
    // separate missile visuals are already inside its own child preparation.
    data->effects = m_effects.prepareRestorePool(state.effects, m_sharedEffects);
    if (!data->attacks || !data->arsenal || !data->effects) {
        return std::nullopt;
    }
    return Prepared{std::move(data)};
}

bool CombatRollbackCheckpoint::canCommit(const Prepared& prepared) const {
    return canCommit(prepared, true);
}
bool CombatRollbackCheckpoint::canCommit(const Prepared& prepared, bool unchangedValues) const {
    if (!bindingsValid() || !prepared.m_data || prepared.m_data->owner != this ||
        prepared.m_data->lifetime.lock() != m_lifetime || m_generation == 0 ||
        prepared.m_data->generation != m_generation ||
        prepared.m_data->players.size() != m_players.size() ||
        (unchangedValues && (prepared.m_data->previousHealth != m_health.checkpoint() ||
                             !m_levels.matches(prepared.m_data->previousLevels))) ||
        !prepared.m_data->attacks || !prepared.m_data->arsenal || !prepared.m_data->effects) {
        return false;
    }
    for (usize index = 0; index < m_players.size(); ++index) {
        if (!m_players[index]->canCommit(prepared.m_data->players[index])) {
            return false;
        }
    }
    return PlayerAttacksCheckpoint::canCommit(m_attacks, *prepared.m_data->attacks) &&
           PlayerArsenalCheckpoint::canCommit(m_arsenal, *prepared.m_data->arsenal) &&
           m_effects.canCommit(*prepared.m_data->effects);
}

bool CombatRollbackCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    commitReady(std::move(prepared));
    return true;
}
void CombatRollbackCheckpoint::commitReady(Prepared&& prepared) {
    auto data = std::move(prepared.m_data);
    // All mutable resource owners were preflighted before the first change.
    // No callbacks or intervening updates are permitted. A child refusal now
    // violates that contract; never report a partly restored state as a refusal.
    for (usize index = 0; index < m_players.size(); ++index) {
        if (!m_players[index]->commitRestore(std::move(data->players[index]))) {
            std::terminate();
        }
    }
    if (!PlayerAttacksCheckpoint::commit(m_attacks, std::move(*data->attacks)) ||
        !PlayerArsenalCheckpoint::commit(m_arsenal, std::move(*data->arsenal)) ||
        !m_effects.commitRestore(std::move(*data->effects)) || !m_health.restore(data->health)) {
        std::terminate();
    }
    static_assert(std::is_nothrow_move_assignable_v<LevelWatch>);
    m_levels = std::move(data->levels);
    ++m_generation;
}

bool CombatRollbackCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}

std::optional<CombatRollbackCheckpoint::Rewind> CombatRollbackCheckpoint::prepareRewind() const {
    const auto state = checkpoint();
    return state ? prepareRewind(*state) : std::nullopt;
}
std::optional<CombatRollbackCheckpoint::Rewind>
CombatRollbackCheckpoint::prepareRewind(const State& target) const {
    auto prepared = prepareRestore(target);
    return prepared ? std::optional{Rewind{std::move(*prepared)}} : std::nullopt;
}
bool CombatRollbackCheckpoint::canRewind(const Rewind& rewind) const {
    return canCommit(rewind.m_state, false);
}
bool CombatRollbackCheckpoint::rewind(Rewind&& rewind) {
    if (!canRewind(rewind)) {
        return false;
    }
    commitReady(std::move(rewind.m_state));
    return true;
}
} // namespace gdl::game
