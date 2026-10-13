#include "game/screens/LocalProjectilePrediction.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "game/world/WeaponGlow.h"
#include "game/world/WorldMotion.h"

namespace gdl::game {
namespace {
ShotIdentity identity(u64 epoch, u8 seat, const LocalMovementPrediction::Release& release,
                      u8 ordinal) {
    return {epoch,          release.tick, release.generation, release.motion.grant, seat,
            release.strong, ordinal,      release.emission};
}

bool supportedFan(u8 shots) {
    return shots == 1 || shots == 3 || shots == ShotIdentity::kMaxShots;
}

void fly(PlayerMissiles& pool, u64 ticks, f32 seconds, const WorldCollision& collision) {
    for (u64 i = 0; i < ticks && (pool.count() != 0 || !pool.visualHistory().empty()); ++i) {
        pool.update(seconds, &collision);
        // Contacts are visual termination only; no damage, sound, rumble or inventory callback.
        pool.takeImpacts();
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
} // namespace

void LocalProjectilePrediction::clear() {
    *this = LocalProjectilePrediction{};
}

void LocalProjectilePrediction::begin(const MatchContext& context, u8 localPeer,
                                      const WorldCollision& collision, const WorldScene* scene,
                                      const WorldLayout* layout) {
    clear();
    m_context = context;
    m_local = localPeer;
    PredictedShotLedger::Grants grants{};
    for (usize i = 0; i < grants.size(); ++i) {
        if (localPeer != 0 && context.owners[i] == localPeer) {
            grants[i] = context.grants[i];
        }
    }
    m_ledger.begin(context.epoch, grants);
    m_collision = collision;
    m_layout = layout;
    if (scene != nullptr && layout != nullptr) {
        m_geometry = std::make_unique<WorldScene>(*scene);
    }
}

void LocalProjectilePrediction::remember(ProjectileState state) {
    const auto* origin = state.releaseOrigin();
    const auto before =
        origin ? m_previousByOrigin.find({state.source, *origin}) : m_previousByOrigin.end();
    m_shots.push_back(
        {state, before != m_previousByOrigin.end() ? m_previous[before->second].current : state});
}

void LocalProjectilePrediction::update(const CombatSnapshot* authority,
                                       const LocalMovementPrediction& movement,
                                       const ProjectileResources& resources,
                                       std::span<const InputCommand> sent) {
    ++m_clock;
    m_soundReleases.clear();
    m_previous = std::move(m_shots);
    m_shots.clear();
    m_previousByOrigin.clear();
    for (usize i = 0; i < m_previous.size(); ++i) {
        const auto& shot = m_previous[i].current;
        if (const auto* origin = shot.releaseOrigin()) {
            m_previousByOrigin.emplace(std::pair{shot.source, *origin}, i);
        }
    }
    if (authority != nullptr && authority->motion.epoch == m_context.epoch &&
        (!m_authority || authority->motion.tick > m_authority->motion.tick)) {
        if (!authority->valid() ||
            std::ranges::any_of(authority->projectiles,
                                [&](const auto& shot) {
                                    return shot.releaseOrigin() && !resources.accepts(shot);
                                }) ||
            (m_geometry &&
             (!authority->geometry || !m_geometry->applyGeometry(*authority->geometry)))) {
            m_authority.reset();
            return;
        }
        if (m_geometry) {
            for (usize i = 0; i < m_layout->objects().size(); ++i) {
                m_collision.setObjectTransform(static_cast<s32>(i), m_geometry->worldTransform(i));
                const auto id = static_cast<s32>(i);
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
        std::vector<ShotIdentity> origins;
        for (const auto& shot : authority->projectiles) {
            if (const auto* origin = shot.releaseOrigin()) {
                origins.push_back(*origin);
            }
        }
        if (!m_ledger.observe(m_context.epoch, authority->motion.tick, origins)) {
            m_authority.reset();
            return;
        }
        m_authority = *authority;
        m_receivedAt = m_clock;
    }
    if (!m_authority || m_ledger.failed()) {
        return;
    }
    const bool stale = m_clock - m_receivedAt > LocalMovementPrediction::kStaleTicks;
    const auto baseline = m_authority->motion.tick;
    for (const auto& input : sent) {
        if (input.valid() && input.epoch == m_context.epoch && m_local != 0 &&
            m_context.owners[input.seat] == m_local &&
            input.grant == m_context.grants[input.seat] && input.tick > baseline &&
            input.tick - baseline <= PredictedShotLedger::kHorizon) {
            m_inputTicks[input.seat] = std::max(m_inputTicks[input.seat], input.tick);
        }
    }
    const f32 seconds = 1.0f / static_cast<f32>(m_context.tickRate);
    for (usize seat = 0; seat < m_context.owners.size(); ++seat) {
        if (m_local == 0 || m_context.owners[seat] != m_local) {
            continue;
        }
        const auto index = static_cast<u8>(seat);
        const auto* replay = !stale ? movement.replayed(index) : nullptr;
        const auto& player = m_authority->players[seat];
        const bool predicting = replay != nullptr && player && player->weapon &&
                                resources.acceptsWeapon(*player->weapon) &&
                                replay->endTick >= baseline &&
                                replay->endTick - baseline <= PredictedShotLedger::kHorizon;
        if (predicting) {
            m_inputTicks[seat] = std::max(m_inputTicks[seat], replay->endTick);
        }
        const u64 flightTicks =
            !stale && m_inputTicks[seat] > baseline &&
                    m_inputTicks[seat] - baseline <= PredictedShotLedger::kHorizon
                ? m_inputTicks[seat] - baseline
                : 0;
        std::vector<ShotIdentity> releases;
        if (predicting) {
            for (const auto& release : replay->releases) {
                if (supportedFan(release.shots)) {
                    for (u8 ordinal = 0; ordinal < release.shots; ++ordinal) {
                        releases.push_back(identity(m_context.epoch, index, release, ordinal));
                    }
                }
            }
            if (!m_ledger.replace(index, baseline, replay->endTick, releases)) {
                m_ledger.withdraw(index);
                continue;
            }
        } else {
            m_ledger.withdraw(index);
        }

        // Confirmed shots retain their native flight even if their owner begins a
        // non-predictable action. Revoking speculative input does not erase them.
        for (const auto& source : m_authority->projectiles) {
            const auto* origin = source.releaseOrigin();
            if (origin == nullptr || origin->seat != seat || !m_ledger.suppress(*origin)) {
                continue;
            }
            auto shot = source;
            if (!shot.rollback) {
                // The native weapon no longer exists. Only its already-emitted
                // particles survive; there is no collision or launch to replay.
                if (shot.particles && shot.particles->flight.retiring) {
                    const u64 ticks = shot.particles->held ? 0 : flightTicks;
                    shot.particles->flight.totalSteps += static_cast<u32>(ticks);
                    shot.textureFrame += static_cast<f32>(ticks) * seconds * 30;
                    if (visibleTail(shot, resources) && resources.accepts(shot)) {
                        remember(shot);
                    }
                }
                continue;
            }
            PlayerMissiles pool;
            MissileLaunch launch;
            launch.spec = &MissileSpec::of(static_cast<s32>(shot.rollback->character));
            launch.release =
                MissileRelease{static_cast<s32>(shot.rollback->character), origin->generation,
                               origin->strong, origin->ordinal, origin->emission};
            if (!pool.launch(launch) ||
                !pool.restoreFlight(pool.missile(0).instance, shot.rollback->flight) ||
                (shot.particles &&
                 !pool.restoreVisualFlight(pool.missile(0).instance, shot.particles->flight))) {
                continue;
            }
            const u64 ticks = shot.rollback->held ? 0 : flightTicks;
            fly(pool, ticks, seconds, m_collision);
            if (shot.particles) {
                if (pool.visualHistory().empty()) {
                    continue;
                }
                const auto& history = pool.visualHistory().front();
                shot.particles->flight = history.flight;
                shot.placement = history.placement;
                shot.direction = history.direction;
            }
            if (pool.count() == 0) {
                shot.rollback.reset();
                if (shot.particles && visibleTail(shot, resources) &&
                    resources.sampleWeapon(
                        shot, static_cast<f32>(shot.particles->flight.activeSteps) * seconds,
                        source.textureFrame + static_cast<f32>(ticks) * seconds * 30)) {
                    remember(shot);
                }
                continue;
            }
            const auto& missile = pool.missile(0);
            shot.placement = PlayerMissiles::transformOf(missile);
            shot.direction = missile.velocity;
            shot.age = missile.age;
            shot.rollback->flight = PlayerMissiles::flightCheckpoint(missile);
            if ((shot.source == ProjectileSource::PlayerEffect ||
                 shot.source == ProjectileSource::PlayerRider) &&
                ticks != 0 &&
                !resources.sampleWeapon(shot, missile.age,
                                        source.textureFrame +
                                            static_cast<f32>(ticks) * seconds * 30)) {
                continue;
            }
            remember(shot);
        }
        if (!predicting) {
            continue;
        }
        const auto& weapon = *player->weapon;
        std::optional<u64> firstTextureTick;
        for (const auto& release : replay->releases) {
            if (!supportedFan(release.shots) ||
                !PlayerMissiles::replayModifiers(release.weaponFlags) ||
                ((release.weaponFlags & 0xFU) != 0) != (weapon.rider != 0)) {
                continue;
            }
            PlayerMissiles::WeaponThrow request;
            request.owner = static_cast<s32>(seat);
            request.character = static_cast<s32>(weapon.character);
            request.position = release.motion.position;
            request.center = request.position + Vec3{0, player->movement.followHeight, 0};
            request.hand = weapon.hand;
            request.direction = release.emission != 0 ? release.direction
                                                      : Vec3{std::sin(release.motion.yaw), 0,
                                                             std::cos(release.motion.yaw)};
            request.speed = weapon.speed;
            request.scale = release.strong ? 2.0f : 1.0f;
            request.attackSeconds = release.attackSeconds;
            request.flags = release.weaponFlags;
            request.target =
                release.cursorAiming || release.emission != 0 ? std::nullopt : weapon.target;
            request.shots = release.shots;
            const auto launches = PlayerMissiles::weaponLaunches(request);
            if (launches.size() != release.shots) {
                continue;
            }
            bool sounded = false;
            for (usize ordinal = 0; ordinal < launches.size(); ++ordinal) {
                const auto origin =
                    identity(m_context.epoch, index, release, static_cast<u8>(ordinal));
                const auto found = m_ledger.entries().find(origin);
                if (found == m_ledger.entries().end() ||
                    found->second.status != PredictedShotLedger::Status::Pending) {
                    continue;
                }
                auto launch = launches[ordinal];
                launch.release =
                    MissileRelease{static_cast<s32>(weapon.character), origin.generation,
                                   origin.strong, origin.ordinal, origin.emission};
                const Vec3 velocity = launch.velocity.value_or(PlayerMissiles::launchVelocity(
                    launch.direction, launch.speed, launch.reach, launch.spec->weight));
                // Network metadata may be finite yet produce an unbounded vertical
                // velocity through near-zero target flight time. Reject it before the
                // native substep integrator converts travel distance to an integer.
                if (!PlayerMissiles::FlightCheckpoint{launch.position, velocity, 0, 0, launch.scale,
                                                      1}
                         .valid()) {
                    continue;
                }
                const f32 radius = launch.spec->radius;
                const Vec3 clear = m_collision.resolveWalls(launch.position, radius,
                                                            launch.position.y - radius * 0.5f,
                                                            launch.position.y + radius * 0.5f);
                if (glm::distance(clear, launch.position) > 1e-4f) {
                    continue;
                }
                PlayerMissiles pool;
                if (!pool.launch(launch)) {
                    continue;
                }
                if (!sounded) {
                    m_soundReleases.push_back({identity(m_context.epoch, index, release, 0),
                                               release.shots, release.weaponFlags});
                    sounded = true;
                }
                if (!firstTextureTick) {
                    firstTextureTick = release.tick;
                }
                // The live scene advances projectiles after dispatching this tick's release.
                fly(pool, replay->endTick - release.tick + 1, seconds, m_collision);
                const auto* history =
                    pool.visualHistory().empty() ? nullptr : &pool.visualHistory().front();
                if (pool.count() == 0 && history == nullptr) {
                    continue;
                }
                const auto* missile = pool.count() == 0 ? nullptr : &pool.missile(0);
                ProjectileState shot;
                shot.source = ProjectileSource::PlayerEffect;
                shot.instance = found->second.instance;
                shot.resource = weapon.resource;
                shot.placement =
                    missile ? PlayerMissiles::transformOf(*missile) : history->placement;
                shot.direction = missile ? missile->velocity : history->direction;
                shot.age = missile ? missile->age : 0;
                shot.flags |= launch.spec->spin == 0 ? ProjectileState::kAlong : 0;
                if (missile) {
                    shot.rollback = ProjectileState::Rollback{
                        origin, weapon.character, PlayerMissiles::flightCheckpoint(*missile)};
                }
                const f32 animationAge =
                    missile ? missile->age
                            : static_cast<f32>(history->flight.activeSteps) * seconds;
                const auto emitTree = [&](ProjectileState component, f32 frame) {
                    if (history && resources.hasParticles(component.resource)) {
                        component.particles =
                            ProjectileState::Particles{weapon.character, history->flight, origin};
                    }
                    if ((missile || component.particles) && visibleTail(component, resources) &&
                        resources.sampleWeapon(component, animationAge, frame)) {
                        remember(component);
                    }
                };
                // Native missile texture banks start at their first release, not the
                // level's texture clock, and stay alive between consecutive throws.
                const u64 textureTicks = weapon.textureFrame
                                             ? replay->endTick - baseline
                                             : replay->endTick - *firstTextureTick + 1;
                const f32 textureFrame =
                    weapon.textureFrame.value_or(0) + static_cast<f32>(textureTicks) * seconds * 30;
                shot.textureFrame = textureFrame;
                if (weapon.rider == 0 ||
                    !WeaponGlow::throwsEffectAlone(static_cast<s32>(weapon.character))) {
                    emitTree(shot, textureFrame);
                }
                if (weapon.rider != 0) {
                    shot.source = ProjectileSource::PlayerRider;
                    shot.resource = weapon.rider;
                    const u64 riderTicks = weapon.riderTextureFrame
                                               ? replay->endTick - baseline
                                               : replay->endTick - *firstTextureTick + 1;
                    const f32 riderFrame = weapon.riderTextureFrame.value_or(0) +
                                           static_cast<f32>(riderTicks) * seconds * 30;
                    emitTree(shot, riderFrame);
                }
                if (weapon.streak != 0 && missile) {
                    shot.source = ProjectileSource::Streak;
                    shot.resource = weapon.streak;
                    shot.animation = {};
                    shot.radius = missile->spec->radius;
                    shot.forward = weapon.forward;
                    shot.tint = weapon.tint;
                    if (resources.accepts(shot)) {
                        remember(shot);
                    }
                }
            }
        }
    }
}

void LocalProjectilePrediction::present(CombatSnapshot& displayed, f32 blend) {
    if (!m_authority || displayed.motion.epoch != m_context.epoch || m_ledger.failed()) {
        return;
    }
    m_ledger.advanceDisplayed(m_context.epoch, displayed.motion.tick);
    auto shots = displayed.projectiles;
    std::erase_if(shots, [this](const auto& shot) {
        const auto* origin = shot.releaseOrigin();
        return origin != nullptr && m_ledger.suppress(*origin);
    });
    std::set<std::pair<ProjectileSource, u64>> keys;
    for (const auto& shot : shots) {
        keys.insert(shot.key());
    }
    auto append = [&](const Shot& value) {
        if (shots.size() >= CombatSnapshot::kMaxProjectiles) {
            return;
        }
        auto shot = value.current;
        if (!(shot.particles && shot.particles->flight.retiring) &&
            shot.continuity == value.previous.continuity &&
            glm::distance(Vec3{shot.placement[3]}, Vec3{value.previous.placement[3]}) < 2) {
            shot.placement[3] = glm::mix(value.previous.placement[3], shot.placement[3],
                                         std::clamp(blend, 0.0f, 1.0f));
        }
        if (keys.insert(shot.key()).second) {
            shots.push_back(shot);
        }
    };
    // Preserve unowned host shots and confirmed local shots before speculative
    // visuals. A full roster must never restore obsolete, suppressed host copies.
    for (const auto& value : m_shots) {
        if (value.current.instance < PredictedShotLedger::kPrivateInstance) {
            append(value);
        }
    }
    for (const auto& value : m_shots) {
        if (value.current.instance >= PredictedShotLedger::kPrivateInstance) {
            append(value);
        }
    }
    std::ranges::sort(shots, {}, &ProjectileState::key);
    displayed.projectiles = std::move(shots);
}
} // namespace gdl::game
