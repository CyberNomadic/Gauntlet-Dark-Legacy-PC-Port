#include "game/screens/OrdinaryCombatCheckpoint.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "game/screens/LevelOpponents.h"

namespace gdl::game {
namespace {
bool ordinary(s32 kind) {
    return kind >= 0 && kind < kSwarmKindCount && kind != kGarmBroodKind;
}
bool ordinary(const OpponentState& state) {
    // Check the INCOMING populations, not only the adapter's currently bound
    // ones. Installing Death or a carried placement before the adapter commits
    // would otherwise invalidate that child after other owners had changed.
    const auto& swarm = state.enemies;
    if (!state.placements || !swarm.combatants.empty() || !swarm.deathEvents.empty() ||
        !swarm.deathShots.empty() || !swarm.tagged.empty() || swarm.scales.bossEncounter) {
        return false;
    }
    for (const auto& enemy : swarm.enemies) {
        if (enemy.state != Enemies::State::Inactive && !ordinary(enemy.kind)) {
            return false;
        }
    }
    const auto generator = [](const Generators::Slot& body) {
        return ordinary(body.kind) && !body.boss;
    };
    return std::ranges::all_of(state.generators.roster, generator) &&
           std::ranges::all_of(state.generators.generators, generator) &&
           std::ranges::all_of(state.placements->roster,
                               [](const auto& placement) {
                                   return ordinary(placement.kind) &&
                                          ordinary(placement.spawn.kind) && !placement.carried;
                               }) &&
           std::ranges::all_of(swarm.blows, [](const auto& blow) { return ordinary(blow.kind); }) &&
           std::ranges::all_of(swarm.losses,
                               [](const auto& loss) { return ordinary(loss.kind); }) &&
           std::ranges::all_of(swarm.feedback,
                               [](const auto& feedback) { return ordinary(feedback.kind); }) &&
           std::ranges::all_of(swarm.cues,
                               [](const auto& cue) { return ordinary(cue.enemyKind); }) &&
           std::ranges::all_of(swarm.bursts,
                               [](const auto& burst) { return ordinary(burst.kind); }) &&
           std::ranges::all_of(state.missiles.missiles, [](const auto& missile) {
               return missile.kind.slot != EnemyMissileKind::kDeathShot;
           });
}
} // namespace

bool OrdinaryCombatCheckpoint::State::valid() const {
    if (!combat.valid() || !opponents.valid() || !adapter.valid() || !ordinary(opponents)) {
        return false;
    }
    u32 slots = 0;
    for (const auto& player : combat.players) {
        slots |= 1U << player.player;
        // The associated combo, transporter and captor scene phases are not
        // participants of this ordinary transaction, even when scalar state is.
        if (player.combo.active() || player.capture.body || player.capture.flying ||
            player.transport.timer != 0 || player.deathHeld != -1 || player.itTicks != 0) {
            return false;
        }
    }
    const auto present = [&](s32 player) {
        return player >= 0 && player < 4 && (slots & (1U << player)) != 0;
    };
    const auto reference = [&](s32 player) { return player == -1 || present(player); };
    for (const auto& enemy : opponents.enemies.enemies) {
        if (enemy.state != Enemies::State::Inactive &&
            (!reference(enemy.target) || !reference(enemy.targetBefore) ||
             !reference(enemy.contact) || !reference(enemy.attackIndex) ||
             !reference(enemy.hurtBy))) {
            return false;
        }
    }
    for (const auto& blow : opponents.enemies.blows) {
        if (!present(blow.player)) {
            return false;
        }
    }
    for (const auto& loss : opponents.enemies.losses) {
        if (!reference(loss.player)) {
            return false;
        }
    }
    for (const auto& missile : opponents.missiles.missiles) {
        for (const auto& [player, remaining] : missile.pierced) {
            if (!present(player)) {
                return false;
            }
        }
    }
    for (const auto& burst : opponents.missiles.bursts) {
        if (!reference(burst.blast.spared)) {
            return false;
        }
        for (const auto& held : burst.players) {
            if (!present(held.id)) {
                return false;
            }
        }
        for (const auto& held : burst.swarm) {
            if (held.id >= OpponentSimulation::kCritterIds) {
                return false;
            }
        }
    }
    for (const auto& hit : opponents.missiles.hits) {
        if (!reference(hit.player) || hit.target >= OpponentSimulation::kCritterIds) {
            return false;
        }
    }
    for (usize slot = 0; slot < adapter.hitStreak.size(); ++slot) {
        if (adapter.hitStreak[slot] != 0 && !present(static_cast<s32>(slot))) {
            return false;
        }
    }
    return std::ranges::all_of(adapter.lessons,
                               [&](const auto& lesson) { return present(lesson.second); }) &&
           std::ranges::all_of(adapter.generatorRewards,
                               [&](const auto& reward) { return present(reward.player); }) &&
           std::ranges::all_of(adapter.cueEffects, [&](u32 effect) {
               return combat.effects.nextId == 0 || effect < combat.effects.nextId;
           });
}

struct OrdinaryCombatCheckpoint::Prepared::Data {
    const OrdinaryCombatCheckpoint* owner = nullptr;
    std::weak_ptr<const u8> lifetime;
    u64 generation = 0;
    std::optional<CombatRollbackCheckpoint::Prepared> combat;
    std::optional<OpponentSimulation::Prepared> pools;
    std::optional<OpponentCombatCheckpoint::Prepared> adapter;
};
OrdinaryCombatCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data)
    : m_data(std::move(data)) {}
OrdinaryCombatCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
OrdinaryCombatCheckpoint::Prepared&
OrdinaryCombatCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
OrdinaryCombatCheckpoint::Prepared::~Prepared() = default;

struct OrdinaryCombatCheckpoint::Rewind::Data {
    const OrdinaryCombatCheckpoint* owner = nullptr;
    std::weak_ptr<const u8> lifetime;
    u64 generation = 0;
    std::optional<CombatRollbackCheckpoint::Rewind> combat;
    std::optional<Generators::Prepared> generators;
    std::optional<Enemies::Prepared> enemies;
    std::optional<EnemyMissiles::Prepared> missiles;
    std::optional<PlacedEnemies::Rewind> placements;
    std::optional<OpponentCombatCheckpoint::Prepared> adapter;
};
OrdinaryCombatCheckpoint::Rewind::Rewind(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
OrdinaryCombatCheckpoint::Rewind::Rewind(Rewind&&) noexcept = default;
OrdinaryCombatCheckpoint::Rewind&
OrdinaryCombatCheckpoint::Rewind::operator=(Rewind&&) noexcept = default;
OrdinaryCombatCheckpoint::Rewind::~Rewind() = default;

OrdinaryCombatCheckpoint::OrdinaryCombatCheckpoint(
    CombatRollbackCheckpoint& combat, LevelOpponents& opponents,
    std::span<const EnemyMissiles::Resource> missiles)
    : m_combat(combat), m_opponents(opponents),
      m_pools(opponents.m_generators, opponents.m_enemies, opponents.m_enemyMissiles,
              &opponents.m_placements),
      m_adapter(opponents) {
    if (missiles.size() <= EnemyMissiles::Checkpoint::kMaxResources) {
        m_missiles.assign(missiles.begin(), missiles.end());
        m_configurationValid = true;
    }
}
OrdinaryCombatCheckpoint::~OrdinaryCombatCheckpoint() = default;
bool OrdinaryCombatCheckpoint::bindingsValid() const {
    if (!m_configurationValid || m_generation == 0 || !m_opponents.m_resources) {
        return false;
    }
    const auto& resources = *m_opponents.m_resources;
    return m_combat.binds(resources.world, resources.weapons, resources.effects, resources.device);
}
std::optional<OrdinaryCombatCheckpoint::State> OrdinaryCombatCheckpoint::checkpoint() const {
    if (!bindingsValid()) {
        return std::nullopt;
    }
    auto combat = m_combat.checkpoint();
    auto pools = m_pools.checkpoint(m_missiles);
    if (!combat || !pools) {
        return std::nullopt;
    }
    auto adapter = m_adapter.checkpoint(combat->effects);
    if (!adapter) {
        return std::nullopt;
    }
    State result{std::move(*combat), std::move(*pools), std::move(*adapter)};
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}
std::optional<OrdinaryCombatCheckpoint::Prepared>
OrdinaryCombatCheckpoint::prepareRestore(const State& state) const {
    if (!bindingsValid() || !state.valid()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->owner = this;
    data->lifetime = m_lifetime;
    data->generation = m_generation;
    data->combat = m_combat.prepareRestore(state.combat);
    data->pools = m_pools.prepareRestore(state.opponents, m_missiles);
    data->adapter = m_adapter.prepareRestore(state.adapter, state.combat.effects);
    if (!data->combat || !data->pools || !data->adapter) {
        return std::nullopt;
    }
    return Prepared{std::move(data)};
}
bool OrdinaryCombatCheckpoint::canCommit(const Prepared& prepared) const {
    return bindingsValid() && prepared.m_data && prepared.m_data->owner == this &&
           prepared.m_data->lifetime.lock() == m_lifetime &&
           prepared.m_data->generation == m_generation && prepared.m_data->combat &&
           prepared.m_data->pools && prepared.m_data->adapter &&
           m_combat.canCommit(*prepared.m_data->combat) &&
           m_pools.canCommit(*prepared.m_data->pools) &&
           m_adapter.canCommit(*prepared.m_data->adapter);
}
bool OrdinaryCombatCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    // The incoming ordinary-only gate guarantees pool installation cannot
    // invalidate the adapter's unsupported-owner preflight. Shared effects are
    // restored just once, inside the player-combat child.
    if (!m_combat.commitRestore(std::move(*data->combat)) ||
        !m_pools.commitRestore(std::move(*data->pools)) ||
        !m_adapter.commitRestore(std::move(*data->adapter))) {
        std::terminate();
    }
    ++m_generation;
    return true;
}
bool OrdinaryCombatCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}

std::optional<OrdinaryCombatCheckpoint::Rewind> OrdinaryCombatCheckpoint::prepareRewind() const {
    const auto state = checkpoint();
    if (!state) {
        return std::nullopt;
    }
    auto data = std::make_unique<Rewind::Data>();
    data->owner = this;
    data->lifetime = m_lifetime;
    data->generation = m_generation;
    data->combat = m_combat.prepareRewind();
    // These pools already fence resource/restore generations independently of
    // normal gameplay. Placements additionally change their stale-command
    // revision on observe/activate, so they need the distinct rewind token.
    data->generators = m_opponents.m_generators.prepareRestore(state->opponents.generators);
    data->enemies = m_opponents.m_enemies.prepareRestore(state->opponents.enemies);
    data->missiles =
        m_opponents.m_enemyMissiles.prepareRestore(state->opponents.missiles, m_missiles);
    data->placements = m_opponents.m_placements.prepareRewind();
    data->adapter = m_adapter.prepareRestore(state->adapter, state->combat.effects);
    if (!data->combat || !data->generators || !data->enemies || !data->missiles ||
        !data->placements || !data->adapter) {
        return std::nullopt;
    }
    return Rewind{std::move(data)};
}
bool OrdinaryCombatCheckpoint::canRewind(const Rewind& rewind) const {
    const auto& data = rewind.m_data;
    return bindingsValid() && data && data->owner == this && data->lifetime.lock() == m_lifetime &&
           data->generation == m_generation && data->combat && data->generators && data->enemies &&
           data->missiles && data->placements && data->adapter &&
           m_combat.canRewind(*data->combat) &&
           m_opponents.m_generators.canCommit(*data->generators) &&
           m_opponents.m_enemies.canCommit(*data->enemies) &&
           m_opponents.m_enemyMissiles.canCommit(*data->missiles) &&
           m_opponents.m_placements.canRewind(*data->placements) &&
           m_adapter.canCommit(*data->adapter);
}
bool OrdinaryCombatCheckpoint::rewind(Rewind&& rewind) {
    if (!canRewind(rewind)) {
        return false;
    }
    auto data = std::move(rewind.m_data);
    // Every owner passed the common preflight. No callbacks or gameplay may
    // interleave here. Only the combat child restores the shared effect pool.
    if (!m_combat.rewind(std::move(*data->combat)) ||
        !m_opponents.m_generators.commitRestore(std::move(*data->generators)) ||
        !m_opponents.m_enemies.commitRestore(std::move(*data->enemies)) ||
        !m_opponents.m_enemyMissiles.commitRestore(std::move(*data->missiles)) ||
        !m_opponents.m_placements.rewind(std::move(*data->placements)) ||
        !m_adapter.commitRestore(std::move(*data->adapter))) {
        std::terminate();
    }
    ++m_generation;
    return true;
}
} // namespace gdl::game
