#include "game/screens/LocalTurboEffectPrediction.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace gdl::game {

void LocalTurboEffectPrediction::clear() {
    m_seats = {};
    m_observed.clear();
    m_confirmed.clear();
    m_tick.reset();
    m_context = {};
    m_local = 0;
}

void LocalTurboEffectPrediction::begin(const MatchContext& context, u8 localPeer) {
    clear();
    if (context.valid() && localPeer != 0) {
        m_context = context;
        m_local = localPeer;
    }
}

bool LocalTurboEffectPrediction::bind(u8 seat, const TurboEffectResources& resources,
                                      const PlayerFigure& figure, const ItemArchive& weapons,
                                      f32 bodyScale) {
    if (seat >= m_seats.size() || m_context.owners[seat] != m_local || m_local == 0 ||
        resources.stats() == nullptr || figure.actionTree() == nullptr || !weapons.loaded() ||
        !std::isfinite(bodyScale) || bodyScale <= 0 || bodyScale > 16) {
        return false;
    }
    m_seats[seat] = {&resources, &figure, &weapons, bodyScale, nullptr};
    return true;
}

bool LocalTurboEffectPrediction::owned(const EffectIdentity& origin) const {
    return origin.valid() && origin.epoch == m_context.epoch &&
           m_context.owners[origin.seat] == m_local &&
           m_context.grants[origin.seat] == origin.grant;
}

bool LocalTurboEffectPrediction::replay(Seat& seat, u8 index, const CombatSnapshot& authority,
                                        const LocalMovementPrediction::Replay& frames) {
    const auto& state = authority.players[index];
    const auto& motion = authority.motion.players[index];
    const u64 baseline = authority.motion.tick;
    if (seat.resources == nullptr || seat.figure == nullptr || seat.weapons == nullptr || !state ||
        !motion || !state->turbo || !state->turboGeometry || !state->turboEffects ||
        !state->turboGeometry->moving || !state->turboEffects->valid() ||
        state->life != ReplicaPlayerLife::Standing || frames.endTick <= baseline ||
        frames.endTick - baseline > state->turboGeometry->moving->ticks ||
        frames.frames.size() != frames.endTick - baseline) {
        return false;
    }
    auto next = std::make_unique<TurboEffectSimulation>();
    next->bind(*seat.resources);
    TurboEffectSimulation::Checkpoint seed;
    bool classSeeded = false;
    bool weaponsSeeded = false;
    const auto& clocks = *state->turboEffects;
    for (usize row = 0; row < seat.resources->stats()->moveEffects.size(); ++row) {
        const auto* resource = seat.resources->resource(static_cast<u32>(row) + 1);
        if (resource == nullptr || resource->kind != TurboEffectResources::Kind::Tree) {
            continue;
        }
        const bool weapon = resource->archive == seat.weapons;
        auto& seeded = weapon ? weaponsSeeded : classSeeded;
        const auto& clock = weapon ? clocks.weaponBank : clocks.classBank;
        if (!seeded && clock) {
            seed.banks.push_back({resource->id, *clock});
            seeded = true;
        }
    }
    if (!next->restore(seed)) {
        return false;
    }
    PlayerAnimator animator;
    if (!animator.bind(*seat.figure->actionTree(), false)) {
        return false;
    }
    const f32 seconds = 1.0f / static_cast<f32>(m_context.tickRate);
    u64 tick = baseline;
    for (const auto& frame : frames.frames) {
        if (frame.tick != ++tick || frame.motion.grant != motion->grant ||
            frame.motion.continuity != motion->continuity ||
            !animator.restore(frame.animation, frame.transitionPose)) {
            return false;
        }
        const Mat4 body = glm::scale(glm::rotate(glm::translate(Mat4{1}, frame.motion.position),
                                                 frame.motion.yaw, Vec3{0, 1, 0}),
                                     Vec3{seat.bodyScale});
        const TurboEffectSimulation::Pose pose{
            body, seat.figure->rootAttachment(body, animator.pose()), {}};
        const Vec3 facing{std::sin(frame.motion.yaw), 0, std::cos(frame.motion.yaw)};
        for (const auto& strike : frame.strikes) {
            if (!PlayerAnimator::rollbackTurboAction(frame.animation.action) ||
                strike.slot >= EffectIdentity::kMaxOccurrences) {
                return false;
            }
            const auto prepared = seat.resources->prepare(
                strike.row, {seat.resources->character(), frame.motion.position, facing},
                clocks.meleeDamage);
            if (!prepared || std::ranges::any_of(prepared->effects, [](const auto& effect) {
                    return effect.particle.has_value();
                })) {
                // Even a withheld tree can create the shared native texture bank.
                // Later strikes cannot guess that first-use clock, so discard this
                // visual prefix until a newer authority checkpoint supplies it.
                return false;
            }
            if (!clocks.weaponBank &&
                std::ranges::any_of(prepared->effects, [&](const auto& effect) {
                    const auto* resource =
                        seat.resources->resource(static_cast<u32>(effect.row) + 1);
                    return resource != nullptr &&
                           resource->kind == TurboEffectResources::Kind::Tree &&
                           resource->archive == seat.weapons;
                })) {
                // Another player's action, an impact or a fixture may create this
                // shared bank before our predicted strike. Its first-use clock
                // is unknown until authority supplies a seed. Class-only chains
                // still use their private per-figure bank normally.
                return false;
            }
            const EffectIdentity origin{m_context.epoch,
                                        frame.tick,
                                        frame.animation.animation.generation,
                                        motion->grant,
                                        index,
                                        static_cast<u16>(strike.slot),
                                        0};
            if (!next->start(origin, *prepared, pose)) {
                return false;
            }
        }
        if (!next->step(seconds, pose)) {
            return false;
        }
    }
    seat.endpoint = frames.frames.back().motion;
    seat.replay = std::move(next);
    return true;
}

void LocalTurboEffectPrediction::update(const CombatSnapshot* authority,
                                        const LocalMovementPrediction& movement) {
    for (auto& seat : m_seats) {
        seat.replay.reset();
        seat.endpoint.reset();
    }
    m_confirmed.clear();
    if (authority == nullptr || !authority->valid() || authority->motion.epoch != m_context.epoch ||
        (m_tick && authority->motion.tick < *m_tick) ||
        std::ranges::any_of(authority->projectiles, [&](const auto& effect) {
            const auto* shot = effect.releaseOrigin();
            return (effect.effectOrigin && effect.effectOrigin->tick > authority->motion.tick) ||
                   (shot != nullptr && shot->tick > authority->motion.tick);
        })) {
        m_tick.reset();
        m_observed.clear();
        return;
    }
    m_tick = authority->motion.tick;
    for (const auto& effect : authority->projectiles) {
        if (effect.effectOrigin) {
            if (const auto found = m_observed.find(*effect.effectOrigin);
                found != m_observed.end()) {
                found->second = *m_tick;
            }
        }
    }
    std::erase_if(m_observed, [&](const auto& known) {
        return !owned(known.first) || (known.second < *m_tick &&
                                       *m_tick - known.second > LocalMovementPrediction::kHistory);
    });
    for (u8 index = 0; index < m_seats.size(); ++index) {
        if (m_context.owners[index] != m_local) {
            continue;
        }
        const auto* frames = movement.replayed(index);
        if (frames == nullptr || !replay(m_seats[index], index, *authority, *frames)) {
            continue;
        }
        const auto& emitted = m_seats[index].replay->emitted();
        const auto fresh = static_cast<usize>(std::ranges::count_if(
            emitted, [&](const auto& origin) { return !m_observed.contains(origin); }));
        if (m_observed.size() + fresh > kMaxOrigins) {
            m_seats[index].replay.reset();
            continue;
        }
        for (const auto& origin : emitted) {
            m_observed.try_emplace(origin, origin.tick);
        }
    }
    for (const auto& effect : authority->projectiles) {
        if (effect.effectOrigin && m_observed.contains(*effect.effectOrigin) &&
            owned(*effect.effectOrigin)) {
            m_confirmed.push_back(effect);
        }
    }
}

bool LocalTurboEffectPrediction::present(CombatSnapshot& displayed) const {
    if (!compatible(displayed)) {
        return false;
    }
    auto shots = displayed.projectiles;
    std::erase_if(shots, [&](const auto& shot) {
        return shot.effectOrigin && m_observed.contains(*shot.effectOrigin);
    });
    std::set<std::pair<ProjectileSource, u64>> keys;
    for (const auto& shot : shots) {
        keys.insert(shot.key());
    }
    for (const auto& effect : m_confirmed) {
        if (keys.insert(effect.key()).second) {
            if (shots.size() >= CombatSnapshot::kMaxProjectiles) {
                return false;
            }
            shots.push_back(effect);
        }
    }
    std::ranges::sort(shots, {}, &ProjectileState::key);
    displayed.projectiles = std::move(shots);
    return true;
}

void LocalTurboEffectPrediction::draw(RenderDevice& device, const CombatSnapshot& displayed,
                                      const Mat4& clip, const WorldLighting& lighting,
                                      const CameraFrame& camera, f32 blend) const {
    if (!compatible(displayed)) {
        return;
    }
    for (const auto& seat : m_seats) {
        if (seat.replay) {
            seat.replay->effects().draw(device, clip, lighting, &camera, blend);
        }
    }
}

bool LocalTurboEffectPrediction::compatible(const CombatSnapshot& displayed) const {
    if (!m_tick || displayed.motion.epoch != m_context.epoch) {
        return false;
    }
    for (usize index = 0; index < m_seats.size(); ++index) {
        const auto& seat = m_seats[index];
        const auto& shown = displayed.motion.players[index];
        if (seat.replay && (!seat.endpoint || !shown || shown->grant != seat.endpoint->grant ||
                            shown->continuity != seat.endpoint->continuity)) {
            return false;
        }
    }
    return std::ranges::all_of(m_confirmed, [&](const auto& effect) {
        const auto& origin = *effect.effectOrigin;
        const auto& shown = displayed.motion.players[origin.seat];
        return shown && shown->grant == origin.grant;
    });
}

const TurboEffectSimulation* LocalTurboEffectPrediction::simulation(u8 seat) const {
    return seat < m_seats.size() ? m_seats[seat].replay.get() : nullptr;
}
} // namespace gdl::game
