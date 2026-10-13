#include "game/screens/LocalPotionPrediction.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "game/world/WorldMotion.h"

namespace gdl::game {
namespace {
constexpr u64 kHorizon = PotionSimulation::kMaxTicks;

void fly(PlayerMissiles& pool, u64 ticks, const WorldCollision& collision) {
    for (u64 tick = 0; tick < ticks && (pool.count() != 0 || !pool.visualHistory().empty());
         ++tick) {
        pool.update(PotionSimulation::kStep, &collision);
        pool.takeImpacts(); // Observations only: no damage, sound, heal or inventory callbacks.
    }
}

bool visibleTail(const ProjectileState& shot, const ProjectileResources& resources) {
    if (!shot.particles || !shot.particles->flight.retiring) {
        return true;
    }
    const auto& history = shot.particles->flight;
    return history.valid() && history.activeSteps != 0 &&
           static_cast<f32>(history.totalSteps - history.activeSteps) * history.stepSeconds <=
               resources.particleLifetime(shot.resource);
}

PotionIdentity identity(const PotionSimulation::Identity& value) {
    return {value.owner.epoch,
            value.tick,
            value.generation,
            value.owner.grant,
            value.owner.seat,
            value.kind == PotionSimulation::Kind::Throw ? PotionIdentity::Kind::Throw
                                                        : PotionIdentity::Kind::Use};
}
} // namespace

void LocalPotionPrediction::clear() {
    *this = LocalPotionPrediction{};
}

void LocalPotionPrediction::begin(const MatchContext& context, u8 localPeer,
                                  const WorldCollision& collision, const ItemArchive& weapons,
                                  const ProjectileResources& resources, const WorldScene* scene,
                                  const WorldLayout* layout) {
    clear();
    if (!context.valid() || context.tickRate != 60 || localPeer == 0 || !weapons.loaded()) {
        return;
    }
    m_context = context;
    m_local = localPeer;
    m_collision = collision;
    m_resources = &resources;
    m_layout = layout;
    if (scene != nullptr && layout != nullptr) {
        m_geometry = std::make_unique<WorldScene>(*scene);
    }
    for (s32 kind = 1; kind <= 4; ++kind) {
        const auto tree = weapons.trees.find(PotionEmission::look(kind).bottle);
        if (!tree) {
            continue;
        }
        const u32 id = resources.treeId(&weapons, &weapons.trees.tree(*tree));
        if (id == 0) {
            continue;
        }
        ProjectileState probe;
        probe.source = ProjectileSource::PlayerEffect;
        probe.instance = 1;
        probe.resource = id;
        if (resources.sampleWeapon(probe, 0, 0)) {
            m_bottles[static_cast<usize>(kind)] = id;
        }
    }
}

bool LocalPotionPrediction::owned(const PotionIdentity& origin) const {
    return m_local != 0 && origin.valid() && origin.epoch == m_context.epoch &&
           origin.kind == PotionIdentity::Kind::Throw && m_context.owners[origin.seat] == m_local &&
           m_context.grants[origin.seat] == origin.grant;
}

bool LocalPotionPrediction::remember(ProjectileState shot,
                                     const std::map<PotionIdentity, ProjectileState>& previous) {
    const auto* origin = shot.potionReleaseOrigin();
    if (origin == nullptr || !m_resources->accepts(shot)) {
        return false;
    }
    const auto found = previous.find(*origin);
    m_shots.push_back({shot, found == previous.end() ? shot : found->second});
    return true;
}

void LocalPotionPrediction::update(const CombatSnapshot* authority,
                                   const LocalMovementPrediction& movement,
                                   std::span<const InputCommand> sent) {
    ++m_clock;
    std::map<PotionIdentity, ProjectileState> previous;
    for (const auto& shot : m_shots) {
        previous.emplace(*shot.current.potionReleaseOrigin(), shot.current);
    }
    m_shots.clear();
    m_present = {};
    if (m_resources == nullptr) {
        return;
    }
    if (authority != nullptr && authority->motion.epoch == m_context.epoch &&
        (!m_authority || authority->motion.tick > m_authority->motion.tick)) {
        if (!authority->valid() ||
            std::ranges::any_of(authority->projectiles,
                                [&](const auto& shot) {
                                    return shot.potionReleaseOrigin() &&
                                           !m_resources->accepts(shot);
                                }) ||
            (m_geometry &&
             (!authority->geometry || !m_geometry->applyGeometry(*authority->geometry)))) {
            m_authority.reset();
            return;
        }
        if (m_geometry) {
            for (usize i = 0; i < m_layout->objects().size(); ++i) {
                const auto id = static_cast<s32>(i);
                m_collision.setObjectTransform(id, m_geometry->worldTransform(i));
                if (authority->worldMotion) {
                    const u8 flags = authority->worldMotion->collision[i];
                    m_collision.setSolid(id, (flags & WorldMotion::kSolid) != 0);
                    m_collision.setContactOnly(id, (flags & WorldMotion::kContactOnly) != 0);
                    m_collision.setFloorExitBlocked(id,
                                                    (flags & WorldMotion::kFloorExitBlocked) != 0);
                } else {
                    m_collision.setSolid(id, m_geometry->objectVisible(i));
                }
            }
        }
        m_authority = *authority;
        m_receivedAt = m_clock;
    }
    if (!m_authority) {
        return;
    }
    const auto baseline = m_authority->motion.tick;
    const bool stale = m_clock - m_receivedAt > LocalMovementPrediction::kStaleTicks;
    for (const auto& input : sent) {
        if (input.valid() && input.epoch == m_context.epoch &&
            m_context.owners[input.seat] == m_local &&
            input.grant == m_context.grants[input.seat] && input.tick > baseline &&
            input.tick - baseline <= kHorizon) {
            m_inputTicks[input.seat] = std::max(m_inputTicks[input.seat], input.tick);
        }
    }
    std::erase_if(m_instances, [&](const auto& entry) { return entry.first.tick <= baseline; });
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        if (m_context.owners[seat] != m_local || !m_authority->players[seat]) {
            continue;
        }
        const auto* replay = stale ? nullptr : movement.replayed(seat);
        const auto& player = *m_authority->players[seat];
        if (replay && replay->endTick >= baseline && replay->endTick - baseline <= kHorizon) {
            m_inputTicks[seat] = std::max(m_inputTicks[seat], replay->endTick);
        }
        const u64 advance =
            !stale && m_inputTicks[seat] > baseline && m_inputTicks[seat] - baseline <= kHorizon
                ? m_inputTicks[seat] - baseline
                : 0;
        const usize begin = m_shots.size();
        bool valid = true;
        for (const auto& source : m_authority->projectiles) {
            const auto* origin = source.potionReleaseOrigin();
            if (origin == nullptr || !owned(*origin) || origin->seat != seat) {
                continue;
            }
            const auto kind = source.potion ? source.potion->kind : source.particles->potion;
            if (kind < 1 || kind > 4 || m_bottles[kind] != source.resource ||
                source.source != ProjectileSource::PlayerEffect || origin->tick > baseline ||
                (m_resources->hasParticles(source.resource) && !source.particles)) {
                valid = false;
                break;
            }
            const bool held = source.potion ? source.potion->held : source.particles->held;
            const u64 ticks = held ? 0 : advance;
            if (ticks == 0) {
                if (!remember(source, previous)) {
                    valid = false;
                    break;
                }
                continue;
            }
            auto shot = source;
            if (!source.potion) {
                if (shot.particles && shot.particles->flight.retiring) {
                    shot.particles->flight.totalSteps += static_cast<u32>(ticks);
                    shot.textureFrame += static_cast<f32>(ticks) * 0.5f;
                    if (visibleTail(shot, *m_resources)) {
                        remember(shot, previous);
                    }
                }
                continue;
            }
            const auto& state = *source.potion;
            PlayerMissiles pool;
            auto launch =
                PotionEmission::throwPotion({seat, {}, {0, 0, 1}, state.kind, 0, 1, 0}, 0);
            launch.potionRelease = MissilePotionRelease{origin->generation};
            if (!pool.launch(launch) ||
                !pool.restorePotionFlight(pool.missile(0).instance, state.flight) ||
                (source.particles &&
                 !pool.restoreVisualFlight(pool.missile(0).instance, source.particles->flight))) {
                valid = false;
                break;
            }
            fly(pool, ticks, m_collision);
            if (shot.particles) {
                if (pool.visualHistory().empty()) {
                    valid = false;
                    break;
                }
                const auto& history = pool.visualHistory().front();
                shot.particles->flight = history.flight;
                shot.placement = history.placement;
                shot.direction = history.direction;
            }
            if (pool.count() == 0) {
                shot.potion.reset();
                if (shot.particles && visibleTail(shot, *m_resources) &&
                    m_resources->sampleWeapon(shot,
                                              static_cast<f32>(shot.particles->flight.activeSteps) *
                                                  PotionSimulation::kStep,
                                              source.textureFrame +
                                                  static_cast<f32>(ticks) * 0.5f)) {
                    remember(shot, previous);
                }
                continue;
            }
            const auto& missile = pool.missile(0);
            shot.placement = PlayerMissiles::transformOf(missile);
            shot.direction = missile.velocity;
            shot.age = missile.age;
            shot.potion->flight = PlayerMissiles::flightCheckpoint(missile);
            if (!m_resources->sampleWeapon(shot, missile.age,
                                           source.textureFrame + static_cast<f32>(ticks) * 0.5f) ||
                !remember(shot, previous)) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            m_shots.resize(begin);
            continue;
        }
        m_present[seat] = true;
        if (!replay || !player.potion || !player.potion->textureFrame ||
            replay->endTick <= baseline || replay->endTick - baseline > player.potion->ticks) {
            continue;
        }
        for (const auto& release : replay->potions) {
            if (release.origin.kind != PotionSimulation::Kind::Throw) {
                continue;
            }
            const auto origin = identity(release.origin);
            const auto* launch = std::get_if<MissileLaunch>(&release.emission);
            if (!owned(origin) || origin.seat != seat || origin.tick <= baseline ||
                origin.tick > replay->endTick || launch == nullptr || launch->potion < 1 ||
                launch->potion > 4 || m_bottles[static_cast<usize>(launch->potion)] == 0 ||
                m_instances.size() >= kHorizon * InputCommand::kSeats ||
                m_nextInstance == std::numeric_limits<u64>::max()) {
                continue;
            }
            PlayerMissiles pool;
            auto emitted = *launch;
            emitted.potionRelease = MissilePotionRelease{origin.generation};
            if (!pool.launch(emitted)) {
                continue;
            }
            // Missiles advance after release in the same native simulation tick.
            fly(pool, replay->endTick - origin.tick + 1, m_collision);
            const auto* history =
                pool.visualHistory().empty() ? nullptr : &pool.visualHistory().front();
            const auto* missile = pool.count() == 0 ? nullptr : &pool.missile(0);
            if ((missile == nullptr && history == nullptr) ||
                (m_resources->hasParticles(m_bottles[static_cast<usize>(launch->potion)]) &&
                 history == nullptr)) {
                continue;
            }
            const auto [entry, inserted] = m_instances.try_emplace(origin, m_nextInstance);
            if (inserted) {
                ++m_nextInstance;
            }
            ProjectileState shot;
            shot.source = ProjectileSource::PlayerEffect;
            shot.instance = entry->second;
            shot.resource = m_bottles[static_cast<usize>(launch->potion)];
            shot.placement = missile ? PlayerMissiles::transformOf(*missile) : history->placement;
            shot.direction = missile ? missile->velocity : history->direction;
            shot.age = missile ? missile->age : 0;
            shot.flags |= launch->spec->spin == 0 ? ProjectileState::kAlong : 0;
            if (missile) {
                shot.potion = ProjectileState::PotionRollback{
                    origin, static_cast<u8>(missile->potion),
                    PlayerMissiles::flightCheckpoint(*missile), false};
            }
            if (history && m_resources->hasParticles(shot.resource)) {
                shot.particles.emplace();
                shot.particles->potion = static_cast<u8>(launch->potion);
                shot.particles->flight = history->flight;
                shot.particles->potionOrigin = origin;
            }
            const f32 texture =
                *player.potion->textureFrame + static_cast<f32>(replay->endTick - baseline) * 0.5f;
            const f32 age =
                missile ? missile->age
                        : static_cast<f32>(history->flight.activeSteps) * PotionSimulation::kStep;
            if (visibleTail(shot, *m_resources) && m_resources->sampleWeapon(shot, age, texture)) {
                remember(shot, previous);
            }
        }
    }
}

void LocalPotionPrediction::present(CombatSnapshot& displayed, f32 blend) const {
    if (!m_authority || displayed.motion.epoch != m_context.epoch ||
        displayed.motion.tick > m_authority->motion.tick) {
        return;
    }
    auto shots = displayed.projectiles;
    std::erase_if(shots, [&](const auto& shot) {
        const auto* origin = shot.potionReleaseOrigin();
        return origin && owned(*origin) && m_present[origin->seat];
    });
    std::set<std::pair<ProjectileSource, u64>> keys;
    for (const auto& shot : shots) {
        keys.insert(shot.key());
    }
    for (const auto& value : m_shots) {
        if (shots.size() >= CombatSnapshot::kMaxProjectiles) {
            break;
        }
        auto shot = value.current;
        if (shot.continuity == value.previous.continuity &&
            glm::distance(Vec3{shot.placement[3]}, Vec3{value.previous.placement[3]}) < 2) {
            shot.placement[3] = glm::mix(value.previous.placement[3], shot.placement[3],
                                         std::clamp(blend, 0.0f, 1.0f));
        }
        if (keys.insert(shot.key()).second) {
            shots.push_back(shot);
        }
    }
    std::ranges::sort(shots, {}, &ProjectileState::key);
    displayed.projectiles = std::move(shots);
}
} // namespace gdl::game
