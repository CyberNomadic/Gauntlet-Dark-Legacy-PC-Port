#include "game/screens/StageReplayPresentation.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "game/screens/StageCombatPresentation.h"
#include "game/screens/StageParticlePresentation.h"

namespace gdl::game {
namespace {
bool refuse(StageReplayPresentation::Refusal value, StageReplayPresentation::Refusal* output) {
    if (output != nullptr) {
        *output = value;
    }
    return false;
}
bool drawingOnly(const CombatSnapshot& state) {
    return !state.combatState && !state.combatFrame && !state.opponents && !state.opponentFrame &&
           !state.worldMotion && state.worldMotionTicks == 0 &&
           std::ranges::none_of(state.players, [](const auto& player) {
               return player && (player->rollbackInput || player->movement.enabled ||
                                 player->movement.current || player->movement.ranged ||
                                 player->movement.platform || player->knockbackTicks != 0 ||
                                 player->locomotion || player->potion || player->melee ||
                                 player->turbo || player->turboGeometry || player->turboEffects ||
                                 player->shove || player->weapon || player->modifiers);
           });
}
} // namespace

std::optional<StageReplayPresentation::Authority>
StageReplayPresentation::authority(const MatchContext& context, u8 localPeer,
                                   const CombatSnapshot& snapshot,
                                   std::weak_ptr<const void> resources) {
    return snapshot.valid() ? validatedAuthority(context, localPeer, snapshot, std::move(resources))
                            : std::nullopt;
}

std::optional<StageReplayPresentation::Authority>
StageReplayPresentation::validatedAuthority(const MatchContext& context, u8 localPeer,
                                            const CombatSnapshot& snapshot,
                                            std::weak_ptr<const void> resources) {
    if (!context.valid() || localPeer == 0 || resources.expired() ||
        snapshot.motion.epoch != context.epoch || snapshot.motion.cameraContinuity == 0) {
        return std::nullopt;
    }
    Authority result;
    result.m_context = context;
    result.m_local = localPeer;
    result.m_tick = snapshot.motion.tick;
    result.m_cameraContinuity = snapshot.motion.cameraContinuity;
    result.m_resources = std::move(resources);
    bool local = false;
    for (usize seat = 0; seat < context.owners.size(); ++seat) {
        const bool occupied = context.owners[seat] != 0;
        if (snapshot.players[seat].has_value() != occupied ||
            snapshot.motion.players[seat].has_value() != occupied) {
            return std::nullopt;
        }
        if (!occupied) {
            continue;
        }
        const auto& motion = *snapshot.motion.players[seat];
        const auto& player = *snapshot.players[seat];
        if (motion.grant != context.grants[seat] || motion.continuity == 0 || player.portalPhase) {
            return std::nullopt;
        }
        result.m_seats[seat] = Authority::Seat{motion.continuity, player.life};
        local |= context.owners[seat] == localPeer;
    }
    return local ? std::optional{std::move(result)} : std::nullopt;
}

bool StageReplayPresentation::sameAuthority(const Authority& first, const Authority& second) {
    return !first.m_resources.expired() && !second.m_resources.expired() &&
           !first.m_resources.owner_before(second.m_resources) &&
           !second.m_resources.owner_before(first.m_resources) &&
           first.m_context == second.m_context && first.m_local == second.m_local &&
           first.m_tick == second.m_tick && first.m_cameraContinuity == second.m_cameraContinuity &&
           first.m_seats == second.m_seats;
}

bool StageReplayPresentation::interval(const Authority& identity, const CombatSnapshot& previous,
                                       const CombatSnapshot& current) {
    return !identity.m_resources.expired() && previous.motion.epoch == identity.m_context.epoch &&
           current.motion.epoch == identity.m_context.epoch &&
           previous.motion.tick >= identity.m_tick && current.motion.tick > previous.motion.tick &&
           current.motion.tick - previous.motion.tick == 1 &&
           current.motion.tick - identity.m_tick <= kMaxTicks &&
           previous.motion.cameraContinuity == identity.m_cameraContinuity &&
           current.motion.cameraContinuity == identity.m_cameraContinuity;
}

std::shared_ptr<const StageReplayPresentation::Publication> StageReplayPresentation::publish(
    const Authority& identity, u64 revision, const CombatSnapshot& previous,
    const CombatSnapshot& current,
    std::shared_ptr<const StageParticlePresentation> previousParticles,
    std::shared_ptr<const StageParticlePresentation> currentParticles) {
    if (revision == 0 || !previous.valid() || !current.valid() || !drawingOnly(previous) ||
        !drawingOnly(current) || !interval(identity, previous, current) || !previousParticles ||
        !currentParticles ||
        !previousParticles->matches(previous.motion.epoch, previous.motion.tick,
                                    identity.m_resources) ||
        !currentParticles->matches(current.motion.epoch, current.motion.tick,
                                   identity.m_resources)) {
        return nullptr;
    }
    // Endpoint rosters must match the original authority, not only one another.
    for (usize seat = 0; seat < identity.m_seats.size(); ++seat) {
        for (const auto* endpoint : {&previous, &current}) {
            const auto& expected = identity.m_seats[seat];
            if (endpoint->players[seat].has_value() != expected.has_value() ||
                endpoint->motion.players[seat].has_value() != expected.has_value()) {
                return nullptr;
            }
            if (expected &&
                (endpoint->motion.players[seat]->grant != identity.m_context.grants[seat] ||
                 endpoint->motion.players[seat]->continuity != expected->continuity ||
                 endpoint->players[seat]->life != expected->life ||
                 endpoint->players[seat]->portalPhase)) {
                return nullptr;
            }
        }
    }
    auto result = std::make_shared<Publication>();
    result->m_authority = identity;
    result->m_revision = revision;
    result->m_previous = std::make_unique<const CombatSnapshot>(previous);
    result->m_current = std::make_unique<const CombatSnapshot>(current);
    result->m_previousParticles = std::move(previousParticles);
    result->m_currentParticles = std::move(currentParticles);
    return result;
}

bool StageReplayPresentation::sample(
    CombatSnapshot& shown, EnemyPresentation& presentation, const Authority& identity,
    const CombatSnapshot& previous, const CombatSnapshot& current,
    const std::shared_ptr<const StageParticlePresentation>& previousParticles,
    const std::shared_ptr<const StageParticlePresentation>& currentParticles, f32 blend,
    Refusal* refusal) {
    if (identity.m_resources.expired()) {
        return refuse(Refusal::Authority, refusal);
    }
    if (shown.motion.epoch != identity.m_context.epoch || shown.motion.tick > identity.m_tick ||
        !presentation.matches(shown) ||
        shown.motion.cameraContinuity != identity.m_cameraContinuity || !std::isfinite(blend)) {
        return refuse(Refusal::Snapshot, refusal);
    }
    for (usize seat = 0; seat < identity.m_seats.size(); ++seat) {
        const auto& expected = identity.m_seats[seat];
        if (shown.players[seat].has_value() != expected.has_value() ||
            shown.motion.players[seat].has_value() != expected.has_value() ||
            current.players[seat].has_value() != expected.has_value() ||
            current.motion.players[seat].has_value() != expected.has_value()) {
            return refuse(Refusal::Roster, refusal);
        }
        if (expected && (shown.motion.players[seat]->continuity != expected->continuity ||
                         current.motion.players[seat]->continuity != expected->continuity ||
                         shown.motion.players[seat]->grant != identity.m_context.grants[seat] ||
                         current.motion.players[seat]->grant != identity.m_context.grants[seat] ||
                         shown.players[seat]->life != expected->life ||
                         current.players[seat]->life != expected->life ||
                         shown.players[seat]->portalPhase || current.players[seat]->portalPhase)) {
            return refuse(Refusal::Roster, refusal);
        }
    }
    if (!previousParticles || !currentParticles ||
        !previousParticles->matches(previous.motion.epoch, previous.motion.tick,
                                    identity.m_resources) ||
        !currentParticles->matches(current.motion.epoch, current.motion.tick,
                                   identity.m_resources)) {
        return refuse(Refusal::ParticleInterval, refusal);
    }
    if (!interval(identity, previous, current) ||
        !StageCombatPresentation::present(shown, presentation, previous, current, blend,
                                          currentParticles->swarm(), currentParticles->party())) {
        return refuse(Refusal::Sampling, refusal);
    }
    // The native draw gathers current-owner point lights without frame blending.
    // Keep that complete list on the same immutable endpoint as its particle roster.
    presentation.particles = currentParticles;
    presentation.particleTick = current.motion.tick;
    presentation.particleBlend = blend;
    if (refusal != nullptr) {
        *refusal = Refusal::None;
    }
    return true;
}

bool StageReplayPresentation::adopt(std::shared_ptr<const Publication> publication,
                                    const Authority& current, Time now) {
    if (!publication || !publication->m_previous || !publication->m_current ||
        !sameAuthority(publication->m_authority, current) ||
        (m_publication && sameAuthority(m_publication->m_authority, current) &&
         publication->m_revision < m_publication->m_revision)) {
        clear();
        return false;
    }
    if (m_publication == publication) {
        return true;
    }
    const bool repeated = m_publication && sameAuthority(m_publication->m_authority, current) &&
                          m_publication->m_revision == publication->m_revision &&
                          m_publication->tick() == publication->tick();
    m_publication = std::move(publication);
    if (!repeated) {
        m_adopted = now;
        m_blend = 0;
    }
    return true;
}

bool StageReplayPresentation::present(CombatSnapshot& shown, EnemyPresentation& presentation,
                                      const Authority& current, Time now, Refusal* refusal) {
    if (!m_publication) {
        return refuse(Refusal::Inactive, refusal);
    }
    if (!sameAuthority(m_publication->m_authority, current)) {
        clear();
        return refuse(Refusal::Authority, refusal);
    }
    // Subtract in a floating duration: extreme injected time points cannot
    // overflow the clock's signed integer representation.
    const long double elapsed =
        std::chrono::duration<long double>(now.time_since_epoch()).count() -
        std::chrono::duration<long double>(m_adopted.time_since_epoch()).count();
    m_blend = std::max(
        m_blend, static_cast<f32>(std::clamp(elapsed * current.m_context.tickRate, 0.0L, 1.0L)));
    if (!sample(shown, presentation, m_publication->m_authority, *m_publication->m_previous,
                *m_publication->m_current, m_publication->m_previousParticles,
                m_publication->m_currentParticles, m_blend, refusal)) {
        clear();
        return false;
    }
    return true;
}

void StageReplayPresentation::clear() {
    m_publication.reset();
    m_adopted = {};
    m_blend = 0;
}
bool StageReplayPresentation::active() const {
    return m_publication && !m_publication->m_authority.m_resources.expired();
}
} // namespace gdl::game
