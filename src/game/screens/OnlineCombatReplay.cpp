#include "game/screens/OnlineCombatReplay.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>

#include "game/netplay/InputTimeline.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/SessionInputs.h"
#include "game/screens/StageCombatPresentation.h"
#include "game/screens/StageParticlePresentation.h"

namespace gdl::game {
namespace {
bool sameInput(const InputCommand& first, const InputCommand& second) {
    const auto real = [](f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); };
    return first.epoch == second.epoch && first.tick == second.tick &&
           first.grant == second.grant && first.seat == second.seat &&
           real(first.direction.x, second.direction.x) &&
           real(first.direction.y, second.direction.y) && real(first.magnitude, second.magnitude) &&
           first.aimDirection.has_value() == second.aimDirection.has_value() &&
           (!first.aimDirection || (real(first.aimDirection->x, second.aimDirection->x) &&
                                    real(first.aimDirection->y, second.aimDirection->y) &&
                                    real(first.aimDirection->z, second.aimDirection->z))) &&
           first.heldButtons == second.heldButtons && first.pressedButtons == second.pressedButtons;
}
} // namespace
OnlineCombatReplay::Domain OnlineCombatReplay::domainOf(const CombatSnapshot* state) {
    if (state == nullptr || !state->combatState || !state->combatFrame || !state->opponents ||
        !state->opponentFrame) {
        return Domain::Unseeded;
    }
    return state->combatState->stage ? Domain::Stage : Domain::Legacy;
}

bool OnlineCombatReplay::refuse(Refusal reason) {
    // Reuse no longer resets at entry. Every rejected admission must revoke the
    // old picture as well as any newly advanced prefix, back to host authority.
    discard();
    auto& counters = m_diagnostics.domains[static_cast<usize>(m_domain)];
    ++counters.refusals;
    ++counters.reasons[static_cast<usize>(reason)];
    return false;
}

bool OnlineCombatReplay::begin(const MatchContext& context, u8 localPeer,
                               CombatPrediction& prediction) {
    clear();
    if (!context.valid() || localPeer == 0 || !prediction.loaded()) {
        return false;
    }
    RollbackInputHistory::Grants grants{};
    for (usize seat = 0; seat < grants.size(); ++seat) {
        if (context.owners[seat] == localPeer) {
            grants[seat] = context.grants[seat];
        }
    }
    if (!m_history.begin(context.epoch, grants)) {
        return false;
    }
    m_context = context;
    m_local = localPeer;
    m_prediction = &prediction;
    return true;
}

void OnlineCombatReplay::discard() {
    if (m_prediction != nullptr) {
        m_prediction->resetReplay();
    }
    m_previous.reset();
    m_shown.reset();
    m_previousParticles.reset();
    m_shownParticles.reset();
    m_executed.clear();
    m_nextFrame.reset();
    m_revision = 0;
}
void OnlineCombatReplay::clear() {
    discard();
    *this = OnlineCombatReplay{};
}
bool OnlineCombatReplay::active() const {
    if (m_prediction == nullptr || !m_prediction->active() || !m_shown || !m_seed ||
        m_revision != m_prediction->replayRevision() ||
        m_shown->motion.tick != m_prediction->tick()) {
        return false;
    }
    const auto lifetime = m_seedLifetime.lock();
    return lifetime && lifetime == m_prediction->resourceLifetime().lock();
}
bool OnlineCombatReplay::staged() const {
    return active() && m_seed && m_seed->stage.has_value();
}
u64 OnlineCombatReplay::tick() const {
    return active() ? m_prediction->tick() : 0;
}

std::unique_ptr<CombatSnapshot>
OnlineCombatReplay::picture(std::shared_ptr<const StageParticlePresentation>& particles) const {
    particles.reset();
    if (!m_authority || m_prediction == nullptr || !m_prediction->active() ||
        m_prediction->enemies() == nullptr) {
        return nullptr;
    }
    auto motion = m_authority->motion;
    motion.tick = m_prediction->tick();
    for (const auto& player : m_prediction->players()) {
        const auto seat = static_cast<usize>(player.actor.player());
        if (seat >= motion.players.size() || !motion.players[seat]) {
            return nullptr;
        }
        motion.players[seat]->position = player.actor.position();
        motion.players[seat]->yaw = std::remainder(player.actor.yaw(), kTwoPi);
    }
    if (m_seed && m_seed->stage) {
        auto result = m_prediction->stagePicture(*m_authority, motion, &particles);
        if (!result || !particles || !particles->matches(motion.epoch, motion.tick)) {
            particles.reset();
            return nullptr;
        }
        return result;
    }
    auto state = CombatCapture::capture(motion, m_prediction->players(), *m_prediction->enemies(),
                                        nullptr, m_prediction->world());
    return state ? std::make_unique<CombatSnapshot>(std::move(*state)) : nullptr;
}

bool OnlineCombatReplay::update(const CombatSnapshot* authority,
                                std::span<const InputCommand> sent) {
    const bool incoming = authority != nullptr && authority->motion.epoch == m_context.epoch &&
                          (!m_authority || authority->motion.tick > m_authority->motion.tick);
    m_domain = domainOf(incoming ? authority : m_authority.get());
    auto& counters = m_diagnostics.domains[static_cast<usize>(m_domain)];
    ++counters.updates;
    try {
        const bool accepted = updatePass(authority, sent);
        counters.passes += accepted ? 1 : 0;
        return accepted;
    } catch (...) {
        // Presentation allocation or a refused native restore must never leave
        // a partly replayed party available to the next rendered frame.
        discard();
        refuse(Refusal::Exception);
        throw;
    }
}

bool OnlineCombatReplay::updatePass(const CombatSnapshot* authority,
                                    std::span<const InputCommand> sent) {
    ++m_clock;
    if (m_prediction == nullptr) {
        return refuse(Refusal::Unbound);
    }
    if (authority != nullptr && authority->motion.epoch == m_context.epoch &&
        (!m_authority || authority->motion.tick > m_authority->motion.tick)) {
        discard();
        if (!authority->valid()) {
            m_authority.reset();
            m_seed.reset();
            m_seedLifetime.reset();
            return refuse(Refusal::InvalidAuthority);
        }
        for (usize seat = 0; seat < m_context.owners.size(); ++seat) {
            const auto& body = authority->motion.players[seat];
            if (m_context.owners[seat] == m_local &&
                (!body || body->grant != m_context.grants[seat] ||
                 (m_authority &&
                  (authority->motion.cameraContinuity != m_authority->motion.cameraContinuity ||
                   (m_authority->motion.players[seat] &&
                    body->continuity != m_authority->motion.players[seat]->continuity))))) {
                m_history.invalidate(seat);
            }
        }
        m_history.resolve(authority->motion.epoch, authority->motion.tick);
        m_authority = std::make_unique<CombatSnapshot>(*authority);
        m_seed.reset();
        m_seedLifetime.reset();
        m_receivedAt = m_clock;
    }
    for (const auto& input : sent) {
        if (!input.valid() || input.epoch != m_context.epoch ||
            m_context.owners[input.seat] != m_local ||
            input.grant != m_context.grants[input.seat]) {
            return refuse(Refusal::InvalidInput);
        }
        if ((!m_authority || input.tick > m_authority->motion.tick) &&
            !m_history.record(std::span{&input, 1}, m_clock)) {
            return refuse(Refusal::InputHistory);
        }
    }
    if (!m_authority || !m_authority->opponents || !m_authority->opponentFrame ||
        !m_authority->combatState || !m_authority->combatFrame) {
        return refuse(Refusal::MissingSeed);
    }
    if (m_authority->opponentFrame->held || m_authority->opponentFrame->timeStopped) {
        return refuse(Refusal::Held);
    }
    if (m_authority->combatFrame->damage.cutscene) {
        return refuse(Refusal::Cutscene);
    }
    if (m_history.resyncRequired()) {
        return refuse(Refusal::Resync);
    }
    if (m_clock - m_receivedAt > 30) {
        return refuse(Refusal::StaleAuthority);
    }
    const auto baseline = m_authority->motion.tick;
    std::array<std::span<const RollbackInputHistory::Entry>, InputCommand::kSeats> commands;
    u64 end = baseline;
    for (usize seat = 0; seat < commands.size(); ++seat) {
        if (m_context.owners[seat] == 0) {
            continue;
        }
        if (!m_authority->players[seat] || !m_authority->motion.players[seat] ||
            m_authority->motion.players[seat]->grant != m_context.grants[seat] ||
            m_authority->players[seat]->portalPhase) {
            return refuse(Refusal::SeatMismatch);
        }
        if (m_context.owners[seat] == m_local) {
            commands[seat] = m_history.replay(seat, m_authority->players[seat]->rollbackInput);
            if (commands[seat].empty()) {
                return refuse(Refusal::NoLocalInput);
            }
            if (end != baseline && end != commands[seat].back().input.tick) {
                return refuse(Refusal::DivergentHorizon);
            }
            end = commands[seat].back().input.tick;
        }
    }
    if (end == baseline || end - baseline > CombatPrediction::kMaxTicks) {
        return refuse(Refusal::Horizon);
    }
    std::vector<Commands> effective;
    effective.reserve(static_cast<usize>(end - baseline));
    for (u64 offset = 1; offset <= end - baseline; ++offset) {
        const auto tick = baseline + offset;
        Commands inputs{};
        for (usize seat = 0; seat < commands.size(); ++seat) {
            if (m_context.owners[seat] == 0) {
                continue;
            }
            if (m_context.owners[seat] == m_local) {
                if (offset > commands[seat].size() ||
                    commands[seat][offset - 1].input.tick != tick) {
                    return refuse(Refusal::InputGap);
                }
                inputs[seat] = commands[seat][offset - 1].input;
            } else {
                inputs[seat] = InputTimeline::heldOrNeutral(
                    m_authority->players[seat]->rollbackInput, m_context.epoch, tick,
                    m_context.grants[seat], static_cast<u8>(seat));
            }
        }
        effective.push_back(std::move(inputs));
    }
    auto& counters = m_diagnostics.domains[static_cast<usize>(m_domain)];
    const auto lifetime = m_prediction->resourceLifetime().lock();
    if (!lifetime || m_seedLifetime.lock() != lifetime) {
        discard();
        m_seed.reset();
        m_seedLifetime.reset();
    }
    if (!lifetime) {
        return refuse(Refusal::SeedRestore);
    }
    if (!m_seed) {
        auto seed = std::make_unique<OrdinaryCombatCheckpoint::State>();
        seed->combat = m_authority->combatState->combat;
        seed->opponents = *m_authority->opponents;
        seed->adapter = m_authority->combatState->adapter;
        if (m_authority->combatState->stage) {
            const auto begin = std::chrono::steady_clock::now();
            seed->stage = m_prediction->decodeStage(*m_authority->combatState->stage);
            counters.decodeNanoseconds +=
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count());
            ++counters.decodeAttempts;
            if (!seed->stage) {
                ++counters.decodeFailures;
                return refuse(Refusal::StageDecode);
            }
            ++counters.decodeSuccesses;
        }
        m_seed = std::move(seed);
        m_seedLifetime = lifetime;
    }
    const bool continuing =
        active() && m_nextFrame && m_executed.size() <= effective.size() &&
        baseline + m_executed.size() == m_prediction->tick() &&
        std::equal(m_executed.begin(), m_executed.end(), effective.begin(),
                   [](const Commands& first, const Commands& second) {
                       return std::equal(first.begin(), first.end(), second.begin(), sameInput);
                   });
    if (!continuing) {
        discard();
        OrdinaryCombatCheckpoint::RestoreIssue seedIssue{};
        if (!m_prediction->begin(m_context.epoch, baseline, *m_seed, &seedIssue)) {
            ++counters.seedIssues[static_cast<usize>(seedIssue)];
            return refuse(Refusal::SeedRestore);
        }
    } else if (m_executed.size() == effective.size()) {
        // Preserve the complete previous/current pair and its native particle
        // history. A repeated publication is not a simulation or render step.
        return true;
    }
    auto frame = continuing ? *m_nextFrame : *m_authority->combatFrame;
    for (usize index = m_executed.size(); index < effective.size(); ++index) {
        const auto tick = baseline + index + 1;
        SessionInputs::Frame inputs{};
        for (usize seat = 0; seat < commands.size(); ++seat) {
            if (m_context.owners[seat] == 0) {
                continue;
            }
            inputs[seat] = SessionInputs::playInput(effective[index][seat]);
        }
        if (tick == end) {
            m_previous = picture(m_previousParticles);
            if (!m_previous) {
                discard();
                return refuse(Refusal::Picture);
            }
        }
        const auto begin = std::chrono::steady_clock::now();
        const auto result = m_prediction->advance(tick, inputs, frame);
        counters.advanceNanoseconds +=
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - begin)
                                 .count());
        if (result.status != OrdinaryCombatTick::Status::Advanced) {
            discard();
            ++counters.tickRefusals[static_cast<usize>(result.refusal)];
            if (result.status == OrdinaryCombatTick::Status::Invalid) {
                ++counters.invalidTicks;
                return refuse(Refusal::NativeInvalid);
            }
            ++counters.unsupportedTicks;
            return refuse(Refusal::NativeUnsupported);
        }
        ++counters.advancedTicks;
        frame.damage.playSeconds += frame.seconds;
    }
    m_shown = picture(m_shownParticles);
    if (!m_shown) {
        discard();
        return refuse(Refusal::Picture);
    }
    m_executed = std::move(effective);
    m_nextFrame = frame;
    m_revision = m_prediction->replayRevision();
    return true;
}

bool OnlineCombatReplay::present(CombatSnapshot& shown, EnemyPresentation& presentation,
                                 f32 blend) {
    auto& counters = m_diagnostics.domains[static_cast<usize>(m_domain)];
    try {
        const bool accepted = presentPass(shown, presentation, blend);
        if (accepted) {
            ++counters.presents;
        } else {
            ++counters.presentRefusals;
        }
        return accepted;
    } catch (...) {
        // CPU allocation failures cannot leave a usable partial native pass.
        // Both presentation helpers stage output before committing it.
        discard();
        ++counters.presentRefusals;
        throw;
    }
}

bool OnlineCombatReplay::presentPass(CombatSnapshot& shown, EnemyPresentation& presentation,
                                     f32 blend) const {
    if (!active() || !m_authority || !m_previous || shown.motion.epoch != m_context.epoch ||
        shown.motion.tick > m_authority->motion.tick || !presentation.matches(shown) ||
        shown.motion.cameraContinuity != m_authority->motion.cameraContinuity ||
        !std::isfinite(blend)) {
        return false;
    }
    // A correction is a whole-party boundary. Do not publish newer enemies
    // against an older/replaced local player or across a camera/portal cut.
    bool local = false;
    for (usize seat = 0; seat < m_context.owners.size(); ++seat) {
        const bool occupied = m_context.owners[seat] != 0;
        if (shown.players[seat].has_value() != occupied ||
            shown.motion.players[seat].has_value() != occupied ||
            m_shown->players[seat].has_value() != occupied ||
            m_shown->motion.players[seat].has_value() != occupied) {
            return false;
        }
        if (!occupied) {
            continue;
        }
        if (shown.motion.players[seat]->continuity != m_shown->motion.players[seat]->continuity ||
            shown.motion.players[seat]->grant != m_context.grants[seat] ||
            shown.players[seat]->portalPhase) {
            return false;
        }
        local |= m_context.owners[seat] == m_local;
    }
    if (!local) {
        return false;
    }
    if (m_seed && m_seed->stage) {
        if (!m_previousParticles || !m_shownParticles ||
            !m_previousParticles->matches(m_previous->motion.epoch, m_previous->motion.tick) ||
            !m_shownParticles->matches(m_shown->motion.epoch, m_shown->motion.tick) ||
            !StageCombatPresentation::present(shown, presentation, *m_previous, *m_shown, blend,
                                              m_shownParticles->swarm(),
                                              m_shownParticles->party())) {
            return false;
        }
        // The current exact native field already contains its own previous-step
        // particle positions. Sampling another field would blend unrelated RNG.
        presentation.particles = m_shownParticles;
        presentation.particleTick = m_shown->motion.tick;
        presentation.particleBlend = blend;
        return true;
    }
    // Stage all allocating copies before changing either presentation owner.
    auto players = shown.players;
    auto motions = shown.motion.players;
    auto enemies = m_shown->enemies;
    EnemyPresentation nextPresentation;
    nextPresentation.epoch = presentation.epoch;
    nextPresentation.tick = presentation.tick;
    nextPresentation.enemies.reserve(enemies.size());
    blend = std::clamp(blend, 0.0f, 1.0f);
    for (usize seat = 0; seat < m_context.owners.size(); ++seat) {
        if (m_context.owners[seat] != m_local) {
            continue;
        }
        auto pose = *m_shown->motion.players[seat];
        auto state = *m_shown->players[seat];
        if (m_previous && m_previous->motion.players[seat] && m_previous->players[seat]) {
            const auto& before = *m_previous->motion.players[seat];
            const auto& animation = m_previous->players[seat]->animation;
            if (glm::distance(before.position, pose.position) < 32 &&
                animation.generation == state.animation.generation &&
                animation.action == state.animation.action) {
                pose.position = glm::mix(before.position, pose.position, blend);
                pose.yaw = before.yaw + std::remainder(pose.yaw - before.yaw, kTwoPi) * blend;
                if (state.animation.frame >= animation.frame) {
                    state.animation.frame =
                        std::lerp(animation.frame, state.animation.frame, blend);
                }
            }
        }
        motions[seat] = pose;
        auto& target = *players[seat];
        target.animation = state.animation;
        target.rollback = state.rollback;
        target.transitionPose = state.transitionPose;
        target.shadow = state.shadow;
        target.hitFlash = state.hitFlash;
        target.reaction = state.reaction;
        target.knockback = state.knockback;
        target.knockbackTicks = 0;
        // Mixed-age drawing state is not a new simulation/replay certificate.
        target.rollbackInput.reset();
        target.movement.enabled = false;
        target.movement.current = false;
        target.movement.ranged = false;
        target.movement.platform = false;
        target.floor = state.floor;
        target.locomotion.reset();
        target.potion.reset();
        target.melee.reset();
        target.turbo.reset();
        target.turboGeometry.reset();
        target.turboEffects.reset();
        target.shove = false;
        target.weapon.reset();
        target.modifiers.reset();
        target.companions = state.companions;
        target.weaponHidden = state.weaponHidden;
        if (target.shadow) {
            target.shadow->ground += pose.position - m_shown->motion.players[seat]->position;
        }
        // HUD totals and external one-shot feedback remain host-confirmed.
    }
    for (auto& enemy : enemies) {
        const auto previous =
            std::ranges::find(m_previous->enemies, enemy.instance, &EnemyCombatState::instance);
        if (previous != m_previous->enemies.end() && previous->kind == enemy.kind &&
            previous->life == enemy.life &&
            previous->animation.generation == enemy.animation.generation) {
            enemy.position = Enemies::supportedPresentation(
                &m_prediction->world()->collision(), previous->position, enemy.position, blend,
                static_cast<EnemyAction>(enemy.animation.action),
                enemy.life == ReplicaEnemyLife::Active);
            enemy.yaw = previous->yaw + std::remainder(enemy.yaw - previous->yaw, kTwoPi) * blend;
            if (enemy.animation.frame >= previous->animation.frame) {
                enemy.animation.frame =
                    std::lerp(previous->animation.frame, enemy.animation.frame, blend);
            }
        }
        nextPresentation.enemies.push_back({enemy, enemy});
    }
    shown.players = std::move(players);
    shown.motion.players = std::move(motions);
    shown.enemies = std::move(enemies);
    shown.combatState.reset();
    shown.combatFrame.reset();
    shown.opponents.reset();
    shown.opponentFrame.reset();
    presentation = std::move(nextPresentation);
    return true;
}
} // namespace gdl::game
