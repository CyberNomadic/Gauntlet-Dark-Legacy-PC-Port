#include "game/screens/NativePlayDriver.h"

#include <algorithm>

namespace gdl::game {
bool NativePlayDriver::bind(PlayScene& scene, const MatchSession& match) {
    if (!match.nativeInputs() || match.phase() != MatchSession::Phase::Loading ||
        !match.context().valid() || match.tick() != 1 || match.context().epoch <= m_context.epoch ||
        match.context().transition == MatchTransition::Shop ||
        scene.simulationTickRate() != match.context().tickRate) {
        return false;
    }
    NativePlayDriver candidate;
    candidate.m_scene = &scene;
    candidate.m_context = match.context();
    candidate.m_nextTick = match.tick();
    if (!candidate.rosterMatches()) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}
void NativePlayDriver::clear() {
    *this = NativePlayDriver{};
}
bool NativePlayDriver::rosterMatches(bool allowDeparted) const {
    if (m_scene == nullptr || !m_scene->isOpen()) {
        return false;
    }
    std::array<bool, InputCommand::kSeats> seen{};
    for (const auto& member : m_scene->participants()) {
        const s32 seat = member.actor.player();
        if ((!allowDeparted && member.departed) || seat < 0 ||
            seat >= static_cast<s32>(seen.size()) || seen[static_cast<usize>(seat)] ||
            m_context.owners[static_cast<usize>(seat)] == 0) {
            return false;
        }
        seen[static_cast<usize>(seat)] = true;
    }
    for (usize seat = 0; seat < seen.size(); ++seat) {
        if (seen[seat] != (m_context.owners[seat] != 0)) {
            return false;
        }
    }
    return true;
}
bool NativePlayDriver::agrees(const MatchSession& match) const {
    return !m_failed && match.nativeInputs() && match.context() == m_context &&
           match.tick() == m_nextTick && rosterMatches(m_finished);
}
NativePlayDriver::Result NativePlayDriver::fail() {
    if (!m_failed && m_scene != nullptr) {
        m_scene->pauseGameplaySounds();
    }
    m_failed = true;
    return Result::Failed;
}
bool NativePlayDriver::sample(MatchSession& match, const SessionInputs::Frame& local) {
    if (!agrees(match)) {
        fail();
        return false;
    }
    if (m_finished || match.nativeDraining()) {
        return true;
    }
    if (match.phase() != MatchSession::Phase::Running) {
        return match.phase() == MatchSession::Phase::Loading;
    }
    auto captured = local;
    usize localIndex = 0;
    for (usize seat = 0; seat < m_context.owners.size(); ++seat) {
        if (m_context.owners[seat] == match.localPeer()) {
            captured[localIndex] =
                m_scene->captureNativeInput(static_cast<s32>(seat), local[localIndex]);
            ++localIndex;
        }
    }
    auto commands = MatchInputs::commands(match, captured);
    if (commands.empty() || (!m_pending.empty() && m_pending.size() != commands.size())) {
        fail();
        return false;
    }
    for (usize index = 0; index < commands.size(); ++index) {
        if (!commands[index].valid()) {
            fail();
            return false;
        }
        if (!m_pending.empty()) {
            commands[index].pressedButtons |= m_pending[index].pressedButtons;
            const auto& pending = m_pending[index];
            if (m_scene->hints().acceptsTopic(pending.hintTopic, pending.hintRevision)) {
                // Preserve the first click until it can be sampled. A later
                // page/back/open invalidates it rather than retargeting it.
                commands[index].hintTopic = pending.hintTopic;
                commands[index].hintRevision = pending.hintRevision;
                commands[index].pressedButtons &= ~static_cast<u32>(CommandPress::MenuAny);
            }
            if (m_pending[index].pressed(CommandPress::Attack)) {
                // Ordinary native attacks consume the held level. A complete
                // tap during a stall still needs one attack-bearing frame.
                commands[index].heldButtons |= static_cast<u32>(CommandHeld::Attack);
            }
        }
    }
    m_pending = std::move(commands);
    if (!match.inputReady()) {
        return true;
    }
    if (!match.sample(m_pending)) {
        fail();
        return false;
    }
    m_pending.clear();
    return true;
}
bool NativePlayDriver::complete(MatchSession& match) {
    if (const auto end = match.nativeEndTick();
        end && *end == completedTick() && m_outcome != PlayOutcome::Travel) {
        // A level change cannot discard a still-running native scene. The
        // application additionally validates destination and secret-parent state.
        fail();
        return false;
    }
    if (!match.completedNativeTick(completedTick())) {
        fail();
        return false;
    }
    return true;
}
void NativePlayDriver::filterArrivalSkip(SessionInputs::Frame& frame) {
    const auto& camera = m_scene->arrival().camera();
    if (!camera.active()) {
        m_arrivalVotes = {};
        return;
    }
    const bool eligible = camera.mode() == StartCamera::Mode::Legacy &&
                          camera.phase() == StartCamera::Phase::Hold &&
                          camera.ticksLeft() <= StartCamera::kSkipBelow;
    bool unanimous = eligible;
    for (usize seat = 0; seat < frame.size(); ++seat) {
        auto& menu = frame[seat].menu;
        if (eligible && m_context.owners[seat] != 0) {
            m_arrivalVotes[seat] = m_arrivalVotes[seat] || menu.start || menu.select || menu.back;
            unanimous = unanimous && m_arrivalVotes[seat];
        }
        menu.start = menu.select = menu.back = false;
    }
    if (unanimous) {
        for (usize seat = 0; seat < frame.size(); ++seat) {
            frame[seat].menu.select = m_context.owners[seat] != 0;
        }
    }
}
NativePlayDriver::Result NativePlayDriver::advance(MatchSession& match) {
    if (m_failed) {
        return Result::Failed;
    }
    if (!agrees(match)) {
        return fail();
    }
    if (m_finished) {
        if (match.nativeDraining() && !complete(match)) {
            return Result::Failed;
        }
        return Result::Held;
    }
    if (match.phase() != MatchSession::Phase::Running) {
        return match.phase() == MatchSession::Phase::Loading ? Result::Held : fail();
    }
    auto frame = MatchInputs::advance(match);
    if (!frame) {
        return match.phase() == MatchSession::Phase::Running ? Result::Held : fail();
    }
    filterArrivalSkip(*frame);
    m_outcome = m_scene->update(1.0 / m_context.tickRate, *frame);
    ++m_nextTick;
    if (!rosterMatches(true)) {
        return fail();
    }
    if (m_outcome != PlayOutcome::GameOver &&
        std::ranges::any_of(m_scene->participants(), &PlayerRuntime::departed)) {
        // Native Quit is a legitimate action, not a changed/malformed input
        // roster. This transport keeps a fixed party for the whole match, so
        // finish it on every peer without discarding the tick just executed.
        m_outcome = PlayOutcome::Leave;
    }
    m_finished = m_outcome != PlayOutcome::Running;
    if (!complete(match)) {
        return Result::Failed;
    }
    switch (m_outcome) {
    case PlayOutcome::Running: return Result::Advanced;
    case PlayOutcome::Travel: return Result::Travel;
    case PlayOutcome::GameOver: return Result::GameOver;
    case PlayOutcome::Leave: return Result::Leave;
    }
    return fail();
}
} // namespace gdl::game
