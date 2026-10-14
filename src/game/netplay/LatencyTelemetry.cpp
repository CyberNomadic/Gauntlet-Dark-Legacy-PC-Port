#include "game/netplay/LatencyTelemetry.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "game/players/PlayerAnimator.h"

namespace gdl::game {
namespace {
f64 milliseconds(LatencyTelemetry::Time start, LatencyTelemetry::Time end) {
    return std::chrono::duration<f64, std::milli>(end - start).count();
}
nlohmann::json distribution(const LatencyTelemetry::Distribution& values) {
    const auto summary = values.summary();
    if (summary.count == 0) {
        return {{"count", 0}, {"window", 0}};
    }
    return {{"count", summary.count}, {"window", summary.window}, {"median_ms", summary.median},
            {"p95_ms", summary.p95},  {"p99_ms", summary.p99},    {"max_ms", summary.maximum}};
}
bool ordinaryThrow(u32 action) {
    using Action = PlayerAnimator::Action;
    return action >= static_cast<u32>(Action::Throw) &&
           action <= static_cast<u32>(Action::ThrowMovingRecover);
}
bool idle(u32 action) {
    return action <= static_cast<u32>(PlayerAnimator::Action::Idle2Loop);
}
bool standing(const CombatSnapshot& state, usize seat) {
    const auto& player = state.players[seat];
    const auto& pose = state.motion.players[seat];
    return player && pose && pose->grant != 0 && pose->continuity != 0 &&
           std::isfinite(pose->position.x) && std::isfinite(pose->position.y) &&
           std::isfinite(pose->position.z) && std::isfinite(pose->yaw) &&
           player->life == ReplicaPlayerLife::Standing && !player->portalPhase;
}
} // namespace
void LatencyTelemetry::Distribution::add(f64 value) {
    if (!std::isfinite(value) || value < 0) {
        return;
    }
    ++m_count;
    m_samples.push_back(value);
    if (m_samples.size() > 512) {
        m_samples.pop_front();
    }
}
LatencyTelemetry::Summary LatencyTelemetry::Distribution::summary() const {
    if (m_samples.empty()) {
        return {};
    }
    std::vector<f64> sorted(m_samples.begin(), m_samples.end());
    std::ranges::sort(sorted);
    const auto percentile = [&](f64 rank) {
        return sorted[static_cast<usize>(std::ceil(rank * static_cast<f64>(sorted.size()))) - 1];
    };
    return {m_count,          sorted.size(),    percentile(0.5),
            percentile(0.95), percentile(0.99), sorted.back()};
}
void LatencyTelemetry::begin(u64 epoch) {
    *this = LatencyTelemetry{};
    m_epoch = epoch;
}
void LatencyTelemetry::sampled(std::span<const InputCommand> commands, Time now) {
    for (const auto& command : commands) {
        if (command.epoch != m_epoch || !command.valid() ||
            std::ranges::any_of(m_pending, [&](const Pending& value) {
                return value.seat == command.seat && value.tick >= command.tick;
            })) {
            continue;
        }
        m_pending.push_back({command.tick, command.seat, now});
        if (m_pending.size() > 512) {
            m_pending.pop_front();
            ++m_expiredInputs;
        }
    }
}
void LatencyTelemetry::sent(std::span<const InputCommand> commands, Time now) {
    for (auto& pending : m_pending) {
        if (!pending.sent && std::ranges::any_of(commands, [&](const InputCommand& command) {
                return command.epoch == m_epoch && command.seat == pending.seat &&
                       command.tick == pending.tick;
            })) {
            m_sendQueue.add(milliseconds(pending.sampled, now));
            pending.sent = true;
        }
    }
}
void LatencyTelemetry::checkpoint(const CombatSnapshot& state, Time now) {
    if (state.motion.epoch != m_epoch) {
        return;
    }
    for (auto& pending : m_pending) {
        if (!pending.resolved && pending.tick <= state.motion.tick) {
            m_resolved.add(milliseconds(pending.sampled, now));
            pending.resolved = true;
        }
    }
    for (usize seat = 0; seat < m_throwAuthority.size(); ++seat) {
        auto& request = m_throwAuthority[seat];
        auto& previous = m_authoritativeActions[seat];
        const auto& pose = state.motion.players[seat];
        const auto& player = state.players[seat];
        if (previous && state.motion.tick <= previous->tick) {
            continue;
        }
        if (!pose || !player) {
            if (request) {
                ++m_interruptedThrowAuthority;
                request.reset();
            }
            previous.reset();
            continue;
        }
        // The resumable native action clock is authoritative. The public pose
        // can carry a presentation cursor interpolated independently of it.
        const auto generation = player->rollback ? player->rollback->animation.generation
                                                 : player->animation.generation;
        const auto action = player->rollback ? static_cast<u32>(player->rollback->action)
                                             : player->animation.action;
        if (request) {
            if (pose->grant != request->origin.grant ||
                pose->continuity != request->origin.continuity) {
                ++m_interruptedThrowAuthority;
                request.reset();
            } else if (ordinaryThrow(action) && generation != request->generation) {
                if (state.motion.tick < request->tick) {
                    // An older attack overlapped this probe. Later phases of
                    // that same throw cannot count as the requested response.
                    ++m_interruptedThrowAuthority;
                    request.reset();
                } else if (const auto& consumed = player->rollbackInput;
                           consumed && consumed->valid() && consumed->epoch == m_epoch &&
                           consumed->seat == seat && consumed->grant == pose->grant &&
                           consumed->tick >= request->tick && consumed->tick <= state.motion.tick) {
                    m_throwConfirmed.add(milliseconds(request->sampled, now));
                    request.reset();
                }
            }
        }
        previous = AuthoritativeAction{*pose, generation, state.motion.tick, action,
                                       player->movement.enabled};
    }
    if (!m_projectileTick || state.motion.tick > *m_projectileTick) {
        m_projectileTick = state.motion.tick;
        for (usize seat = 0; seat < m_projectileOwners.size(); ++seat) {
            const auto& player = state.players[seat];
            const auto& pose = state.motion.players[seat];
            m_projectileOwners[seat] = player && pose &&
                                               player->life == ReplicaPlayerLife::Standing &&
                                               !player->portalPhase
                                           ? pose
                                           : std::nullopt;
        }
        observeProjectiles(state, now, true);
    }
}
void LatencyTelemetry::motionRequest(std::span<const InputCommand> commands,
                                     const CombatSnapshot* shown, const CombatSnapshot* authority,
                                     Time now) {
    for (const auto& command : commands) {
        if (!command.valid() || command.epoch != m_epoch) {
            continue;
        }
        const auto seat = command.seat;
        const bool moving = command.magnitude > 0 && glm::length(command.direction) > 0;
        if (moving && !m_moving[seat] && command.heldButtons == 0 && command.pressedButtons == 0) {
            auto& request = m_motion[seat];
            if (request) {
                ++m_missedMotion;
                request.reset();
            }
            const auto& known = m_authoritativeActions[seat];
            const auto& owner = m_projectileOwners[seat];
            const bool latest = authority != nullptr && m_projectileTick &&
                                authority->motion.epoch == m_epoch &&
                                authority->motion.tick == *m_projectileTick && known && owner &&
                                known->tick == authority->motion.tick && known->movementEnabled &&
                                command.tick > authority->motion.tick;
            if (latest && shown != nullptr && shown->motion.epoch == m_epoch &&
                standing(*authority, seat) && standing(*shown, seat)) {
                const auto& host = *authority->motion.players[seat];
                const auto& origin = *shown->motion.players[seat];
                if (authority->players[seat]->movement.enabled && host.grant == command.grant &&
                    host.grant == owner->grant && host.continuity == owner->continuity &&
                    host.grant == known->origin.grant &&
                    host.continuity == known->origin.continuity &&
                    host.position == known->origin.position && host.yaw == known->origin.yaw &&
                    origin.grant == host.grant && origin.continuity == host.continuity) {
                    request = Motion{now, origin, shown->motion.tick};
                }
            }
            if (!request) {
                ++m_missedMotion;
            }
        }
        m_moving[seat] = moving;
    }
}
void LatencyTelemetry::throwRequest(const InputCommand& input, const CombatSnapshot& shown,
                                    Time now) {
    if (!input.valid() || input.epoch != m_epoch || shown.motion.epoch != m_epoch ||
        !input.pressed(CommandPress::Attack)) {
        return;
    }
    const auto seat = input.seat;
    const auto& pose = shown.motion.players[seat];
    const auto& player = shown.players[seat];
    const auto& authoritative = m_authoritativeActions[seat];
    if (!pose || !player || pose->grant != input.grant || !idle(player->animation.action) ||
        (authoritative && !idle(authoritative->action))) {
        return;
    }
    if (m_throws[seat]) {
        ++m_missedThrows;
    }
    m_throws[seat] = Throw{now, *pose, player->animation.generation, input.tick};
    if (m_throwAuthority[seat]) {
        ++m_supersededThrowAuthority;
    }
    const auto generation = authoritative && authoritative->origin.grant == pose->grant &&
                                    authoritative->origin.continuity == pose->continuity
                                ? authoritative->generation
                                : player->animation.generation;
    m_throwAuthority[seat] = Throw{now, *pose, generation, input.tick};
}
void LatencyTelemetry::localRelease(const ShotIdentity& origin, Time now) {
    if (!origin.valid() || origin.epoch != m_epoch || !m_projectileTick ||
        origin.tick <= *m_projectileTick) {
        return;
    }
    const auto& owner = m_projectileOwners[origin.seat];
    auto& seen = m_releaseTicks[origin.seat];
    if (!owner || origin.grant != owner->grant || (seen && origin.tick < *seen)) {
        return;
    }
    auto& identities = m_releaseIdentities[origin.seat];
    auto& saturated = m_releaseSaturated[origin.seat];
    if (seen && origin.tick == *seen) {
        // Ordinary throws retain the old one-sample-per-seat/tick fence, even if
        // a rewind changes generation. Turbo rows may catch up several distinct
        // emissions in one update, so those use the complete release identity.
        if (origin.emission == 0 || saturated || identities.contains(origin)) {
            return;
        }
    } else {
        identities.clear();
        saturated = false;
    }
    if (identities.size() == 512) {
        saturated = true;
        ++m_expiredReleases; // surface diagnostic overflow instead of claiming complete coverage
        return;
    }
    seen = origin.tick;
    identities.insert(origin);
    m_releases.push_back({origin, *owner, now, false, false, m_releaseCount++});
    m_releaseSamples.push_back({origin, now, std::nullopt, std::nullopt});
    if (m_releaseSamples.size() > 512) {
        m_releaseSamples.pop_front();
    }
    if (m_releases.size() > 512) {
        m_releases.pop_front();
        ++m_expiredReleases;
    }
}
LatencyTelemetry::ReleaseSample* LatencyTelemetry::releaseSample(const Release& release) {
    const auto first = m_releaseCount - static_cast<u64>(m_releaseSamples.size());
    if (release.sample < first || release.sample >= m_releaseCount) {
        return nullptr;
    }
    return &m_releaseSamples[static_cast<usize>(release.sample - first)];
}
void LatencyTelemetry::observeProjectiles(const CombatSnapshot& state, Time now,
                                          bool authoritative) {
    std::erase_if(m_releases, [&](Release& release) {
        auto* sample = releaseSample(release);
        const auto seat = release.shot.seat;
        const auto& owner = state.motion.players[seat];
        const auto& player = state.players[seat];
        if (!owner || !player || owner->grant != release.owner.grant ||
            owner->continuity != release.owner.continuity ||
            player->life != ReplicaPlayerLife::Standing || player->portalPhase) {
            ++m_interruptedReleases;
            if (sample != nullptr) {
                sample->outcome = ReleaseOutcome::Interrupted;
            }
            return true;
        }
        if (authoritative && state.motion.tick < release.shot.tick) {
            return false;
        }
        const bool present = std::ranges::any_of(state.projectiles, [&](const auto& shot) {
            return (shot.source == ProjectileSource::Player ||
                    shot.source == ProjectileSource::PlayerEffect ||
                    shot.source == ProjectileSource::PlayerRider) &&
                   shot.rollback && shot.rollback->origin == release.shot && shot.valid() &&
                   shot.alpha > 0;
        });
        if (authoritative && !release.authoritative) {
            if (!present) {
                // Passing a deadline without the exact projectile is not launch
                // confirmation. A rejected or already impacted throw is unobserved.
                ++m_unobservedProjectiles;
                if (sample != nullptr) {
                    sample->outcome = ReleaseOutcome::Unobserved;
                }
                return true;
            }
            const auto elapsed = milliseconds(release.observed, now);
            m_projectileConfirmed.add(elapsed);
            if (sample != nullptr) {
                sample->hostMs = elapsed;
            }
            release.authoritative = true;
        } else if (!authoritative && !release.presented && present) {
            const auto elapsed = milliseconds(release.observed, now);
            m_projectileFrames.add(elapsed);
            if (sample != nullptr) {
                sample->firstFrameMs = elapsed;
            }
            release.presented = true;
        }
        const bool complete = release.authoritative && release.presented;
        if (complete && sample != nullptr) {
            sample->outcome = ReleaseOutcome::Complete;
        }
        return complete;
    });
}
void LatencyTelemetry::presented(const CombatSnapshot& shown, Time now) {
    if (shown.motion.epoch != m_epoch) {
        return;
    }
    if (m_rendered) {
        m_renderIntervals.add(milliseconds(*m_rendered, now));
    }
    m_rendered = now;
    std::erase_if(m_pending, [&](const Pending& pending) {
        if (pending.resolved && pending.tick <= shown.motion.tick) {
            m_confirmedFrames.add(milliseconds(pending.sampled, now));
            return true;
        }
        return false;
    });
    for (usize seat = 0; seat < m_motion.size(); ++seat) {
        auto& request = m_motion[seat];
        const auto& pose = shown.motion.players[seat];
        if (!request) {
            continue;
        }
        if (!standing(shown, seat) || pose->grant != request->origin.grant ||
            pose->continuity != request->origin.continuity) {
            ++m_missedMotion;
            request.reset();
        } else if (shown.motion.tick >= request->shownTick &&
                   glm::distance(pose->position, request->origin.position) > 0.005f) {
            m_motionFrames.add(milliseconds(request->sampled, now));
            request.reset();
        }
    }
    for (usize seat = 0; seat < m_throws.size(); ++seat) {
        auto& request = m_throws[seat];
        const auto& pose = shown.motion.players[seat];
        const auto& player = shown.players[seat];
        if (!request || !pose || !player) {
            continue;
        }
        if (pose->grant != request->origin.grant ||
            pose->continuity != request->origin.continuity) {
            ++m_missedThrows;
            request.reset();
            continue;
        }
        if (ordinaryThrow(player->animation.action) &&
            player->animation.generation != request->generation) {
            m_throwFrames.add(milliseconds(request->sampled, now));
            request.reset();
        }
    }
    observeProjectiles(shown, now, false);
}
void LatencyTelemetry::network(const PacketTransport::Statistics& stats) {
    if (stats.pingMs < 0) {
        return; // An unavailable path is not a zero-millisecond connection.
    }
    if (m_network && m_network->relayed != stats.relayed) {
        ++m_pathChanges;
    }
    m_network = stats;
    m_rtt.add(stats.pingMs);
}
void LatencyTelemetry::update(Time now) {
    if (m_updated) {
        m_updateIntervals.add(milliseconds(*m_updated, now));
    }
    m_updated = now;
    for (auto& request : m_motion) {
        if (request && now - request->sampled > std::chrono::seconds(1)) {
            ++m_missedMotion;
            request.reset();
        }
    }
    for (auto& request : m_throws) {
        if (request && now - request->sampled > std::chrono::seconds(1)) {
            ++m_missedThrows;
            request.reset();
        }
    }
    for (auto& request : m_throwAuthority) {
        if (request && now - request->sampled > std::chrono::seconds(1)) {
            ++m_expiredThrowAuthority;
            request.reset();
        }
    }
    while (!m_pending.empty() && now - m_pending.front().sampled > std::chrono::seconds(5)) {
        m_pending.pop_front();
        ++m_expiredInputs;
    }
    std::erase_if(m_releases, [&](const Release& release) {
        if (now - release.observed <= std::chrono::seconds(1)) {
            return false;
        }
        ++m_expiredReleases;
        if (auto* sample = releaseSample(release)) {
            sample->outcome = ReleaseOutcome::Expired;
        }
        return true;
    });
}
std::string LatencyTelemetry::report(bool includeReleases) const {
    nlohmann::json result{
        {"schema", 3},
        {"epoch", m_epoch},
        {"rtt", distribution(m_rtt)},
        {"sample_to_send", distribution(m_sendQueue)},
        {"sample_to_resolved", distribution(m_resolved)},
        {"sample_to_confirmed_frame", distribution(m_confirmedFrames)},
        {"motion_to_first_frame", distribution(m_motionFrames)},
        {"throw_to_first_frame", distribution(m_throwFrames)},
        {"throw_to_authority", distribution(m_throwConfirmed)},
        {"release_to_first_projectile_frame", distribution(m_projectileFrames)},
        {"release_to_host_projectile", distribution(m_projectileConfirmed)},
        {"pending_releases", m_releases.size()},
        {"expired_releases", m_expiredReleases},
        {"interrupted_releases", m_interruptedReleases},
        {"unobserved_host_projectiles", m_unobservedProjectiles},
        {"update_interval", distribution(m_updateIntervals)},
        {"render_interval", distribution(m_renderIntervals)},
        {"pending", m_pending.size()},
        {"expired_inputs", m_expiredInputs},
        {"missed_motion", m_missedMotion},
        {"missed_throws", m_missedThrows},
        {"pending_throw_authority",
         std::ranges::count_if(m_throwAuthority,
                               [](const auto& value) { return value.has_value(); })},
        {"expired_throw_authority", m_expiredThrowAuthority},
        {"interrupted_throw_authority", m_interruptedThrowAuthority},
        {"superseded_throw_authority", m_supersededThrowAuthority},
        {"path_changes", m_pathChanges}};
    if (includeReleases) {
        result["release_sample_count"] = m_releaseCount;
        auto& releases = result["release_samples"] = nlohmann::json::array();
        constexpr std::array kOutcomes{"pending", "complete", "expired", "interrupted",
                                       "unobserved"};
        auto ordinal = m_releaseCount - static_cast<u64>(m_releaseSamples.size());
        for (const auto& sample : m_releaseSamples) {
            const auto& shot = sample.shot;
            // This local steady-clock reference correlates probe operation costs. It
            // is not a wall-clock timestamp and is never sent to another machine.
            nlohmann::json entry{{"ordinal", ++ordinal},
                                 {"epoch", shot.epoch},
                                 {"tick", shot.tick},
                                 {"generation", shot.generation},
                                 {"grant", shot.grant},
                                 {"seat", shot.seat},
                                 {"strong", shot.strong},
                                 {"fan_ordinal", shot.ordinal},
                                 {"emission", shot.emission},
                                 {"release_monotonic_ms", milliseconds(Time{}, sample.observed)},
                                 {"first_frame_ms", nullptr},
                                 {"host_ms", nullptr},
                                 {"outcome", kOutcomes[static_cast<usize>(sample.outcome)]}};
            if (sample.firstFrameMs) {
                entry["first_frame_ms"] = *sample.firstFrameMs;
            }
            if (sample.hostMs) {
                entry["host_ms"] = *sample.hostMs;
            }
            releases.push_back(std::move(entry));
        }
    }
    if (m_network) {
        result["relayed"] = m_network->relayed;
        result["queued_bytes"] = m_network->pendingBytes;
    }
    return result.dump();
}
} // namespace gdl::game
