#include "game/rollback/InputTimeline.h"

#include <algorithm>
#include <limits>

#include "engine/core/Assert.h"

namespace gdl::game {

bool InputTimeline::assign(usize seat, Peer peer) {
    if (seat >= m_seats.size() || m_seats[seat].grant == std::numeric_limits<u32>::max()) {
        return false;
    }
    auto& state = m_seats[seat];
    state.owner = peer;
    ++state.grant;
    state.pending.clear();
    state.previous.reset();
    state.diagnostics = {};
    std::erase_if(m_commandTrace,
                  [seat](const CommandTrace& trace) { return trace.command.seat == seat; });
    return true;
}

void InputTimeline::disconnect(Peer peer) {
    if (peer == 0) {
        return;
    }
    std::erase_if(m_commandTrace,
                  [peer](const CommandTrace& trace) { return trace.sender == peer; });
    for (auto& state : m_seats) {
        if (state.owner == peer) {
            state.owner = 0;
            state.pending.clear();
            state.previous.reset();
        }
    }
}

InputTimeline::Peer InputTimeline::owner(usize seat) const {
    return seat < m_seats.size() ? m_seats[seat].owner : 0;
}

u32 InputTimeline::grant(usize seat) const {
    return seat < m_seats.size() ? m_seats[seat].grant : 0;
}

void InputTimeline::beginEpoch() {
    GDL_VERIFY(m_epoch < std::numeric_limits<u64>::max(), "Input epoch exhausted");
    ++m_epoch;
    m_tick = 0;
    m_commandTrace.clear();
    m_droppedCommandTraces = 0;
    for (auto& state : m_seats) {
        state.pending.clear();
        state.previous.reset();
        state.diagnostics = {};
    }
}

InputTimeline::Admission InputTimeline::submit(Peer sender, const InputCommand& command) {
    if (!command.valid()) {
        return Admission::Invalid;
    }
    if (command.epoch != m_epoch) {
        return Admission::WrongEpoch;
    }
    auto& state = m_seats[command.seat];
    if (sender == 0 || sender != state.owner) {
        return Admission::WrongOwner;
    }
    if (command.grant != state.grant) {
        return Admission::WrongGrant;
    }
    // Redundancy and reordering must not inflate late counts or retune the sender
    // using a packet that was already seen. This watermark is diagnostic only.
    auto& diagnostics = state.diagnostics;
    if ((!diagnostics.received || command.tick > *diagnostics.received) &&
        (command.tick <= m_tick || command.tick - m_tick <= kMaxAhead)) {
        diagnostics.received = command.tick;
        diagnostics.arrivalMargin =
            command.tick >= m_tick ? static_cast<s32>(command.tick - m_tick)
                                   : -static_cast<s32>(std::min(m_tick - command.tick, kMaxAhead));
        if (command.tick < m_tick && diagnostics.late < std::numeric_limits<u32>::max()) {
            ++diagnostics.late;
        }
    }
    if (command.tick < m_tick) {
        traceAdmission(sender, command, Admission::TooLate);
        return Admission::TooLate;
    }
    if (command.tick - m_tick > kMaxAhead) {
        return Admission::TooEarly;
    }
    // First accepted command wins: redundant packets cannot change a queued tick.
    const auto [queued, inserted] = state.pending.emplace(command.tick, command);
    const auto result = inserted ? Admission::Accepted : Admission::Duplicate;
    traceAdmission(sender, queued->second, result);
    return result;
}

void InputTimeline::traceAdmission(Peer sender, const InputCommand& command, Admission admission) {
    if (command.heldButtons == 0 && command.pressedButtons == 0 && command.hintTopic == 0) {
        return;
    }
    const auto found = std::ranges::find_if(m_commandTrace, [&](const CommandTrace& trace) {
        return trace.sender == sender && trace.command.epoch == command.epoch &&
               trace.command.seat == command.seat && trace.command.grant == command.grant &&
               trace.command.tick == command.tick;
    });
    if (found != m_commandTrace.end()) {
        // Keep the first submitted payload/result, including after its deadline.
        // Redundant or conflicting packets cannot rewrite execution evidence.
        if (found->repeats < std::numeric_limits<u32>::max()) {
            ++found->repeats;
        }
        return;
    }
    if (m_commandTrace.size() == kCommandTraces) {
        m_commandTrace.pop_front();
        ++m_droppedCommandTraces;
    }
    m_commandTrace.push_back({command, sender, m_tick, admission});
}

void InputTimeline::traceExecution(Peer sender, const InputCommand& command, bool concealed) {
    if (command.heldButtons == 0 && command.pressedButtons == 0 && command.hintTopic == 0) {
        return;
    }
    const auto found = std::ranges::find_if(m_commandTrace, [&](const CommandTrace& trace) {
        return trace.sender == sender && trace.command.epoch == command.epoch &&
               trace.command.seat == command.seat && trace.command.grant == command.grant &&
               trace.command.tick == command.tick;
    });
    if (found == m_commandTrace.end()) {
        return;
    }
    if (concealed) {
        ++found->concealedTicks;
    } else {
        found->executedTick = m_tick;
    }
}

InputCommand InputTimeline::heldOrNeutral(const std::optional<InputCommand>& previous, u64 epoch,
                                          u64 tick, u32 grant, u8 seat) {
    InputCommand command;
    if (previous && previous->epoch == epoch && previous->grant == grant &&
        previous->seat == seat && previous->tick <= tick && tick - previous->tick <= kHoldTicks) {
        command = *previous;
        // A potion toss charges continuously during its wind-up. Retain its
        // held level like Attack; UsePotion and other one-shot presses do not repeat.
        command.pressedButtons = 0;
        command.hintTopic = 0;
        command.hintRevision = 0;
        command.console.reset();
    }
    command.epoch = epoch;
    command.tick = tick;
    command.grant = grant;
    command.seat = seat;
    return command;
}

InputTimeline::Frame InputTimeline::advance() {
    GDL_VERIFY(m_tick < std::numeric_limits<u64>::max(), "Input tick exhausted");
    Frame result;
    for (usize seat = 0; seat < m_seats.size(); ++seat) {
        auto& state = m_seats[seat];
        auto& command = result[seat];
        const auto found = state.pending.find(m_tick);
        if (found != state.pending.end()) {
            state.previous = found->second;
            command = found->second;
            traceExecution(state.owner, found->second, false);
            state.pending.erase(found);
        } else {
            command =
                heldOrNeutral(state.previous, m_epoch, m_tick, state.grant, static_cast<u8>(seat));
            if (state.previous && m_tick - state.previous->tick <= kHoldTicks) {
                ++state.diagnostics.concealed;
                traceExecution(state.owner, *state.previous, true);
            } else if (state.owner != 0) {
                ++state.diagnostics.neutral;
            }
        }
        command.epoch = m_epoch;
        command.tick = m_tick;
        command.grant = state.grant;
        command.seat = static_cast<u8>(seat);
    }
    ++m_tick;
    return result;
}

} // namespace gdl::game
