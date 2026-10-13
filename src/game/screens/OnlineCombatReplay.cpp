#include "game/screens/OnlineCombatReplay.h"

#include <algorithm>
#include <cmath>

#include "game/netplay/InputTimeline.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/SessionInputs.h"

namespace gdl::game {
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
}
void OnlineCombatReplay::clear() {
    discard();
    *this = OnlineCombatReplay{};
}
bool OnlineCombatReplay::active() const {
    return m_prediction != nullptr && m_prediction->active() && m_shown != nullptr;
}
u64 OnlineCombatReplay::tick() const {
    return active() ? m_prediction->tick() : 0;
}

std::unique_ptr<CombatSnapshot> OnlineCombatReplay::picture() const {
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
    auto state = CombatCapture::capture(motion, m_prediction->players(), *m_prediction->enemies(),
                                        nullptr, m_prediction->world());
    return state ? std::make_unique<CombatSnapshot>(std::move(*state)) : nullptr;
}

bool OnlineCombatReplay::update(const CombatSnapshot* authority,
                                std::span<const InputCommand> sent) {
    try {
        return updatePass(authority, sent);
    } catch (...) {
        // Presentation allocation or a refused native restore must never leave
        // a partly replayed party available to the next rendered frame.
        discard();
        throw;
    }
}

bool OnlineCombatReplay::updatePass(const CombatSnapshot* authority,
                                    std::span<const InputCommand> sent) {
    ++m_clock;
    discard();
    if (m_prediction == nullptr) {
        return false;
    }
    if (authority != nullptr && authority->motion.epoch == m_context.epoch &&
        (!m_authority || authority->motion.tick > m_authority->motion.tick)) {
        if (!authority->valid()) {
            m_authority.reset();
            return false;
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
        m_receivedAt = m_clock;
    }
    for (const auto& input : sent) {
        if (!input.valid() || input.epoch != m_context.epoch ||
            m_context.owners[input.seat] != m_local ||
            input.grant != m_context.grants[input.seat]) {
            return false;
        }
        if ((!m_authority || input.tick > m_authority->motion.tick) &&
            !m_history.record(std::span{&input, 1}, m_clock)) {
            return false;
        }
    }
    if (!m_authority || !m_authority->opponents || !m_authority->opponentFrame ||
        !m_authority->combatState || !m_authority->combatFrame ||
        m_authority->opponentFrame->held || m_authority->opponentFrame->timeStopped ||
        m_authority->combatFrame->damage.cutscene || m_history.resyncRequired() ||
        m_clock - m_receivedAt > 30) {
        return false;
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
            return false;
        }
        if (m_context.owners[seat] == m_local) {
            commands[seat] = m_history.replay(seat, m_authority->players[seat]->rollbackInput);
            if (commands[seat].empty()) {
                return false;
            }
            if (end != baseline && end != commands[seat].back().input.tick) {
                return false;
            }
            end = commands[seat].back().input.tick;
        }
    }
    if (end == baseline || end - baseline > CombatPrediction::kMaxTicks) {
        return false;
    }
    const OrdinaryCombatCheckpoint::State seed{m_authority->combatState->combat,
                                               *m_authority->opponents,
                                               m_authority->combatState->adapter};
    if (!m_prediction->begin(m_context.epoch, baseline, seed)) {
        return false;
    }
    auto frame = *m_authority->combatFrame;
    for (u64 offset = 1; offset <= end - baseline; ++offset) {
        const auto tick = baseline + offset;
        SessionInputs::Frame inputs{};
        for (usize seat = 0; seat < commands.size(); ++seat) {
            if (m_context.owners[seat] == 0) {
                continue;
            }
            InputCommand input;
            if (m_context.owners[seat] == m_local) {
                if (offset > commands[seat].size() ||
                    commands[seat][offset - 1].input.tick != tick) {
                    discard();
                    return false;
                }
                input = commands[seat][offset - 1].input;
            } else {
                input = InputTimeline::heldOrNeutral(m_authority->players[seat]->rollbackInput,
                                                     m_context.epoch, tick, m_context.grants[seat],
                                                     static_cast<u8>(seat));
            }
            inputs[seat] = SessionInputs::playInput(input);
        }
        if (tick == end) {
            m_previous = picture();
            if (!m_previous) {
                discard();
                return false;
            }
        }
        if (m_prediction->advance(tick, inputs, frame).status !=
            OrdinaryCombatTick::Status::Advanced) {
            discard();
            return false;
        }
        frame.damage.playSeconds += frame.seconds;
    }
    m_shown = picture();
    if (!m_shown) {
        discard();
        return false;
    }
    return true;
}

bool OnlineCombatReplay::present(CombatSnapshot& shown, EnemyPresentation& presentation,
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
