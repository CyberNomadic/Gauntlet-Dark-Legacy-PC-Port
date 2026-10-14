#include "game/screens/OrdinaryCombatCheckpoint.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <utility>

#include "game/screens/LevelOpponents.h"

namespace gdl::game {
namespace {
bool ordinary(s32 kind) {
    return kind >= 0 && kind < kSwarmKindCount && kind != kGarmBroodKind;
}
bool ordinary(const OpponentState& state) {
    // Check the INCOMING live populations, not only the adapter's currently
    // bound ones. Dormant placement definitions remain intact; the runner
    // preflights their next activation before any native spawn can occur.
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

// Floor owners are collision identities, not item or layout indices. Moving
// riders additionally require the exact mutable transform in the joint stage.
template <class Present>
bool supports(const OrdinaryCombatCheckpoint::State& state, const Present& present) {
    const auto& stage = *state.stage;
    const auto moving = [&](s32 object) {
        return std::ranges::any_of(stage.world.core.moving,
                                   [&](const auto& body) { return body.object == object; });
    };
    const auto optionalFloor = [&](const auto& floor) {
        return !floor || (present(floor->object) && moving(floor->object));
    };
    for (const auto& player : state.combat.players) {
        if ((player.floor.object != -1 && !present(player.floor.object)) ||
            (player.floor.placement && !moving(player.floor.object))) {
            return false;
        }
    }
    for (const auto& item : stage.world.pickups.items) {
        if (!optionalFloor(item.floor)) {
            return false;
        }
    }
    for (const auto& chest : stage.fixtures.chests.chests) {
        if (!optionalFloor(chest.floor)) {
            return false;
        }
    }
    const auto supported = [&](const auto& body) {
        return body.support.object == -1 ||
               (present(body.support.object) && moving(body.support.object));
    };
    if (!std::ranges::all_of(stage.fixtures.gates.gates, supported) ||
        !std::ranges::all_of(stage.fixtures.barrels.barrels, supported) ||
        !std::ranges::all_of(stage.fixtures.traps.traps, supported) ||
        !std::ranges::all_of(stage.fixtures.scenery.props, supported)) {
        return false;
    }
    for (const auto& enemy : state.opponents.enemies.enemies) {
        if (!optionalFloor(enemy.floor)) {
            return false;
        }
    }
    for (const auto& slot : state.adapter.critters.actors) {
        for (const auto& part : slot.parts) {
            if (!optionalFloor(part.actor.floor)) {
                return false;
            }
        }
    }
    if (stage.portals && !std::ranges::all_of(stage.portals->portals, [&](const auto& portal) {
            return portal.support == -1 || (present(portal.support) && moving(portal.support));
        })) {
        return false;
    }
    if (stage.transporters && !std::ranges::all_of(stage.transporters->pads, [&](const auto& pad) {
            // A post-trigger boundary may retain the preceding native pad
            // placement. Authenticate its owner, not equality to the new floor pose.
            return pad.support == -1 || (present(pad.support) && moving(pad.support));
        })) {
        return false;
    }
    const auto generator = [&](const auto& body) {
        return body.support == -1 || (present(body.support) && moving(body.support));
    };
    return std::ranges::all_of(state.opponents.generators.roster, generator) &&
           std::ranges::all_of(state.opponents.generators.generators, generator);
}

bool stageLinks(const OrdinaryCombatCheckpoint::State& state) {
    if (!state.stage) {
        return true;
    }
    const auto& stage = *state.stage;
    if ((stage.camera && stage.camera->level != stage.world.level) ||
        !supports(state, [](s32 object) { return object >= 0; })) {
        return false;
    }
    const auto pickup = [&](usize index) { return index < stage.world.pickups.items.size(); };
    if (stage.portals) {
        const auto partyIndex = [&](s32 index) {
            return index >= 0 && static_cast<usize>(index) < state.combat.players.size();
        };
        if (!std::ranges::all_of(stage.portals->waiting, partyIndex) ||
            !std::ranges::all_of(stage.portals->visitors,
                                 [&](const auto& visitor) { return partyIndex(visitor.party); })) {
            return false;
        }
    }
    const auto slot = [&](s32 index) {
        return index == -1 || std::ranges::any_of(state.combat.players, [&](const auto& player) {
                   return player.player == index;
               });
    };
    for (const auto& item : stage.world.pickups.items) {
        if (!slot(item.opener)) {
            return false;
        }
    }
    // Containers/blasts address the ordered party, whereas pickup/attack owners
    // address sparse controller slots. Destroyed/collected records remain valid.
    for (const auto& chest : stage.fixtures.chests.chests) {
        if ((chest.held != -1 && !pickup(static_cast<usize>(chest.held))) ||
            (chest.opener != -1 &&
             static_cast<usize>(chest.opener) >= state.combat.players.size())) {
            return false;
        }
    }
    for (const auto& blast : stage.fixtures.blasts) {
        if (std::ranges::any_of(
                blast.players, [&](usize index) { return index >= state.combat.players.size(); })) {
            return false;
        }
    }
    for (const auto& cloud : stage.fixtures.clouds) {
        if (cloud.effect != 0 && state.combat.effects.nextId != 0 &&
            cloud.effect >= state.combat.effects.nextId) {
            return false;
        }
    }
    const auto carried = [&](const auto& placement) {
        return !placement.carried || pickup(*placement.carried);
    };
    if (!std::ranges::all_of(state.opponents.placements->roster, carried) ||
        !std::ranges::all_of(state.adapter.statues.risen, carried)) {
        return false;
    }
    for (const auto& statue : state.adapter.statues.roster) {
        if (!carried(statue.placement)) {
            return false;
        }
    }
    const auto rock = [&](const auto& hit) { return hit.rock < stage.fixtures.rocks.rocks.size(); };
    return std::ranges::all_of(state.opponents.missiles.rockHits, rock) &&
           std::ranges::all_of(state.adapter.projectiles.rockHits, rock) &&
           std::ranges::all_of(state.adapter.rockHits, rock);
}
// Only called after complete child validation, either by public valid() or by
// successful child captures. No caller-authored value bypasses child validation.
bool combatLinks(const CombatRollbackCheckpoint::State& combat, const OpponentState& opponents,
                 const OpponentCombatCheckpoint::State& adapter) {
    if (!ordinary(opponents)) {
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
    if (!adapter.references(static_cast<u8>(slots), combat.effects)) {
        return false;
    }
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
} // namespace

bool OrdinaryCombatCheckpoint::State::valid() const {
    return OrdinaryCombatCheckpoint::valid(combat, opponents, adapter) &&
           (!stage || (stage->world.valid() && stage->fixtures.valid() &&
                       (!stage->portals || stage->portals->valid()) &&
                       (!stage->transporters || stage->transporters->valid()) &&
                       (!stage->camera || stage->camera->valid()))) &&
           stageLinks(*this);
}
bool OrdinaryCombatCheckpoint::valid(const CombatRollbackCheckpoint::State& combat,
                                     const OpponentState& opponents,
                                     const OpponentCombatCheckpoint::State& adapter) {
    return combat.valid() && opponents.valid() && adapter.valid() &&
           combatLinks(combat, opponents, adapter);
}

struct OrdinaryCombatCheckpoint::Prepared::Data {
    const OrdinaryCombatCheckpoint* owner = nullptr;
    std::weak_ptr<const u8> lifetime;
    u64 generation = 0;
    std::optional<CombatRollbackCheckpoint::Prepared> combat;
    std::optional<OpponentSimulation::Prepared> pools;
    std::optional<OpponentCombatCheckpoint::Prepared> adapter;
    std::optional<LevelWorldCheckpoint::Prepared> world;
    std::optional<LevelFixturesCheckpoint::Prepared> fixtures;
    std::optional<TowerCameraCheckpoint::Prepared> camera;
    std::optional<ExitPortalsCheckpoint::Prepared> portals;
    std::optional<LevelTransportersCheckpoint::Prepared> transporters;
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
    std::optional<LevelWorldCheckpoint::Prepared> world;
    std::optional<LevelFixturesCheckpoint::Prepared> fixtures;
    std::optional<TowerCameraCheckpoint::Prepared> camera;
    std::optional<ExitPortalsCheckpoint::Prepared> portals;
    std::optional<LevelTransportersCheckpoint::Prepared> transporters;
};
OrdinaryCombatCheckpoint::Rewind::Rewind(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
OrdinaryCombatCheckpoint::Rewind::Rewind(Rewind&&) noexcept = default;
OrdinaryCombatCheckpoint::Rewind&
OrdinaryCombatCheckpoint::Rewind::operator=(Rewind&&) noexcept = default;
OrdinaryCombatCheckpoint::Rewind::~Rewind() = default;

OrdinaryCombatCheckpoint::OrdinaryCombatCheckpoint(
    CombatRollbackCheckpoint& combat, LevelOpponents& opponents,
    std::span<const EnemyMissiles::Resource> missiles, LevelWorldCheckpoint* world,
    LevelFixturesCheckpoint* fixtures, TowerCameraCheckpoint* camera,
    ExitPortalsCheckpoint* portals, LevelTransportersCheckpoint* transporters)
    : m_combat(combat), m_opponents(opponents),
      m_pools(opponents.m_generators, opponents.m_enemies, opponents.m_enemyMissiles,
              &opponents.m_placements),
      m_adapter(opponents, combat.m_sharedEffects), m_world(world), m_fixtures(fixtures),
      m_camera(camera), m_portals(portals), m_transporters(transporters) {
    if (missiles.size() <= EnemyMissiles::Checkpoint::kMaxResources) {
        m_missiles.assign(missiles.begin(), missiles.end());
        m_configurationValid = true;
    }
}
OrdinaryCombatCheckpoint::~OrdinaryCombatCheckpoint() = default;
bool OrdinaryCombatCheckpoint::bindingsValid() const {
    if (!m_configurationValid || m_generation == 0 ||
        m_generation == std::numeric_limits<u64>::max() || !m_opponents.m_resources ||
        (m_world != nullptr) != (m_fixtures != nullptr) ||
        ((m_camera || m_portals || m_transporters) && !m_world)) {
        return false;
    }
    const auto& resources = *m_opponents.m_resources;
    return m_combat.binds(resources.world, resources.weapons, resources.effects,
                          resources.device) &&
           (!m_camera || (&m_camera->m_world == &resources.world && m_camera->bound())) &&
           (!m_portals ||
            m_portals->binds(resources.world.layout(), resources.world.items(),
                             &resources.world.realmItems(), resources.world.collision())) &&
           (!m_transporters ||
            m_transporters->binds(resources.world.layout(), &resources.world.collision())) &&
           (!m_world || (m_world->binds(resources.world) &&
                         m_fixtures->binds(resources.world, resources.weapons, resources.effects,
                                           resources.device)));
}
bool OrdinaryCombatCheckpoint::stageBound() const {
    return m_world && bindingsValid();
}
std::optional<OrdinaryCombatCheckpoint::State>
OrdinaryCombatCheckpoint::checkpoint(CaptureTimings* timings) const {
    if (timings) {
        *timings = {};
    }
    const auto measured = [&](CaptureOwner owner, auto&& operation) {
        if (!timings) {
            return operation();
        }
        const auto begin = std::chrono::steady_clock::now();
        auto result = operation();
        (*timings)[static_cast<usize>(owner)] =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - begin)
                                 .count());
        return result;
    };
    if (!measured(CaptureOwner::Bindings, [&] { return bindingsValid(); })) {
        return std::nullopt;
    }
    auto combat = measured(CaptureOwner::Combat, [&] { return m_combat.checkpoint(); });
    auto pools = measured(CaptureOwner::Pools, [&] { return m_pools.checkpoint(m_missiles); });
    if (!combat || !pools) {
        return std::nullopt;
    }
    auto adapter =
        measured(CaptureOwner::Adapter, [&] { return m_adapter.checkpoint(combat->effects); });
    if (!adapter) {
        return std::nullopt;
    }
    State result{std::move(*combat), std::move(*pools), std::move(*adapter)};
    if (m_world) {
        auto world = measured(CaptureOwner::World, [&] { return m_world->checkpoint(); });
        auto fixtures = measured(CaptureOwner::Fixtures, [&] { return m_fixtures->checkpoint(); });
        if (!world || !fixtures) {
            return std::nullopt;
        }
        result.stage = Stage{std::move(*world), std::move(*fixtures)};
        if (m_camera) {
            result.stage->camera =
                measured(CaptureOwner::Camera, [&] { return m_camera->checkpoint(); });
            if (!result.stage->camera) {
                return std::nullopt;
            }
        }
        if (m_portals) {
            result.stage->portals =
                measured(CaptureOwner::Portals, [&] { return m_portals->checkpoint(); });
            if (!result.stage->portals) {
                return std::nullopt;
            }
        }
        if (m_transporters) {
            result.stage->transporters =
                measured(CaptureOwner::Transporters, [&] { return m_transporters->checkpoint(); });
            if (!result.stage->transporters) {
                return std::nullopt;
            }
        }
    }
    // Child captures already performed complete value/resource validation.
    // Keep every ordinary-domain and cross-owner check, without rescanning
    // their immutable geometry and particle rosters at this aggregate level.
    return measured(CaptureOwner::Links,
                    [&] {
                        return combatLinks(result.combat, result.opponents, result.adapter) &&
                               stageLinks(result);
                    })
               ? std::optional{std::move(result)}
               : std::nullopt;
}
std::optional<OrdinaryCombatCheckpoint::Prepared>
OrdinaryCombatCheckpoint::prepareRestore(const State& state, RestoreIssue* issue) const {
    if (issue) {
        *issue = RestoreIssue::None;
    }
    const auto reject = [&](RestoreIssue why) -> std::optional<Prepared> {
        if (issue) {
            *issue = why;
        }
        return std::nullopt;
    };
    if (!bindingsValid() || state.stage.has_value() != (m_world != nullptr) ||
        (state.stage && state.stage->camera.has_value() != (m_camera != nullptr)) ||
        (state.stage && state.stage->portals.has_value() != (m_portals != nullptr)) ||
        (state.stage && state.stage->transporters.has_value() != (m_transporters != nullptr))) {
        return reject(RestoreIssue::Bindings);
    }
    if (!state.valid()) {
        return reject(RestoreIssue::State);
    }
    if (state.stage) {
        // Every rider shares the same immutable collision identity catalog.
        // Validate it once, not once per player, fixture and enemy. Child
        // preparation and commit still fence the live binding independently.
        const auto objects = m_world->collisionObjects();
        if (!supports(state,
                      [&](s32 object) { return std::ranges::binary_search(objects, object); })) {
            return reject(RestoreIssue::Supports);
        }
    }
    auto data = std::make_unique<Prepared::Data>();
    data->owner = this;
    data->lifetime = m_lifetime;
    data->generation = m_generation;
    data->combat = m_combat.prepareRestore(state.combat);
    if (!data->combat) {
        return reject(RestoreIssue::Combat);
    }
    data->pools = m_pools.prepareRestore(state.opponents, m_missiles);
    if (!data->pools) {
        return reject(RestoreIssue::Pools);
    }
    data->adapter = m_adapter.prepareRestore(state.adapter, state.combat.effects);
    if (!data->adapter) {
        return reject(RestoreIssue::Adapter);
    }
    if (state.stage) {
        data->world = m_world->prepareRestore(state.stage->world);
        if (!data->world) {
            return reject(RestoreIssue::World);
        }
        data->fixtures = m_fixtures->prepareRestore(state.stage->fixtures);
        if (!data->fixtures) {
            return reject(RestoreIssue::Fixtures);
        }
        if (m_portals) {
            data->portals = m_portals->prepareRestore(*state.stage->portals);
            if (!data->portals) {
                return reject(RestoreIssue::Portals);
            }
        }
        if (m_transporters) {
            data->transporters = m_transporters->prepareRestore(*state.stage->transporters);
            if (!data->transporters) {
                return reject(RestoreIssue::Transporters);
            }
        }
        if (m_camera) {
            data->camera = m_camera->prepareRestore(*state.stage->camera);
            if (!data->camera) {
                return reject(RestoreIssue::Camera);
            }
        }
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
           m_adapter.canCommit(*prepared.m_data->adapter) &&
           (!m_portals ||
            (prepared.m_data->portals && m_portals->canCommit(*prepared.m_data->portals))) &&
           (!m_transporters || (prepared.m_data->transporters &&
                                m_transporters->canCommit(*prepared.m_data->transporters))) &&
           (!m_camera ||
            (prepared.m_data->camera && m_camera->canCommit(*prepared.m_data->camera))) &&
           (!m_world || (prepared.m_data->world && prepared.m_data->fixtures &&
                         m_world->canCommit(*prepared.m_data->world) &&
                         m_fixtures->canCommit(*prepared.m_data->fixtures)));
}
bool OrdinaryCombatCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    if (m_transporters && !m_transporters->commitRestore(std::move(*data->transporters))) {
        std::terminate();
    }
    if (m_portals && !m_portals->commitRestore(std::move(*data->portals))) {
        std::terminate();
    }
    if (m_camera && !m_camera->commitRestore(std::move(*data->camera))) {
        std::terminate();
    }
    if (m_world && (!m_world->commitRestore(std::move(*data->world)) ||
                    !m_fixtures->commitRestore(std::move(*data->fixtures)))) {
        std::terminate();
    }
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
bool OrdinaryCombatCheckpoint::restore(const State& state, RestoreIssue* issue) {
    auto prepared = prepareRestore(state, issue);
    if (!prepared) {
        return false;
    }
    if (!commitRestore(std::move(*prepared))) {
        if (issue) {
            *issue = RestoreIssue::Commit;
        }
        return false;
    }
    return true;
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
    if (state->stage) {
        data->world = m_world->prepareRestore(state->stage->world);
        data->fixtures = m_fixtures->prepareRestore(state->stage->fixtures);
        if (m_portals) {
            data->portals = m_portals->prepareRestore(*state->stage->portals);
            if (!data->portals) {
                return std::nullopt;
            }
        }
        if (m_transporters) {
            data->transporters = m_transporters->prepareRestore(*state->stage->transporters);
            if (!data->transporters) {
                return std::nullopt;
            }
        }
        if (m_camera) {
            data->camera = m_camera->prepareRestore(*state->stage->camera);
            if (!data->camera) {
                return std::nullopt;
            }
        }
        if (!data->world || !data->fixtures) {
            return std::nullopt;
        }
    }
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
           m_adapter.canCommit(*data->adapter) &&
           (!m_portals || (data->portals && m_portals->canCommit(*data->portals))) &&
           (!m_transporters ||
            (data->transporters && m_transporters->canCommit(*data->transporters))) &&
           (!m_camera || (data->camera && m_camera->canCommit(*data->camera))) &&
           (!m_world || (data->world && data->fixtures && m_world->canCommit(*data->world) &&
                         m_fixtures->canCommit(*data->fixtures)));
}
bool OrdinaryCombatCheckpoint::rewind(Rewind&& rewind) {
    if (!canRewind(rewind)) {
        return false;
    }
    auto data = std::move(rewind.m_data);
    if (m_transporters && !m_transporters->commitRestore(std::move(*data->transporters))) {
        std::terminate();
    }
    if (m_portals && !m_portals->commitRestore(std::move(*data->portals))) {
        std::terminate();
    }
    if (m_camera && !m_camera->commitRestore(std::move(*data->camera))) {
        std::terminate();
    }
    if (m_world && (!m_world->commitRestore(std::move(*data->world)) ||
                    !m_fixtures->commitRestore(std::move(*data->fixtures)))) {
        std::terminate();
    }
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
