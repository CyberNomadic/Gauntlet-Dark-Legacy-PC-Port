#pragma once

#include <array>
#include <deque>
#include <map>
#include <optional>

#include "game/netplay/InputCommand.h"

namespace gdl::game {

/** Offline command admission and playout, independent of sockets and input devices.
 * SessionInputs assigns every seat locally and submits its controls before each
 * advance. Ownership and bounded missing-input handling remain available to its
 * diagnostic tests. Native online simulation uses RollbackInputs instead.
 * Call from the simulation thread; this object is not thread-safe. */
class InputTimeline {
public:
    using Peer = u64;
    using Frame = std::array<InputCommand, InputCommand::kSeats>;
    enum class Admission : u8 {
        Accepted,
        Invalid,
        WrongEpoch,
        WrongOwner,
        WrongGrant,
        TooLate,
        TooEarly,
        Duplicate,
    };
    // Bounds in simulation ticks. At the current 60 Hz update rate these allow
    // two seconds of queued inputs and 100 ms of held-input concealment.
    static constexpr u64 kMaxAhead = 120;
    static constexpr u64 kHoldTicks = 6;
    struct Diagnostics {
        std::optional<u64> received;
        s32 arrivalMargin = 0;
        u32 late = 0;
        u64 concealed = 0;
        u64 neutral = 0;
    };
    /** Local diagnostics only: authenticated, current-owner commands carrying an
     * action level or edge. Neutral/movement traffic cannot evict action evidence.
     * An execution is exact queued consumption, not an action or hit acknowledgement. */
    struct CommandTrace {
        InputCommand command;
        Peer sender = 0;
        u64 receivedTick = 0;
        Admission admission = Admission::Invalid;
        u32 repeats = 0;
        std::optional<u64> executedTick;
        u64 concealedTicks = 0;
    };
    static constexpr usize kCommandTraces = 128;
    const std::deque<CommandTrace>& commandTrace() const { return m_commandTrace; }
    u64 droppedCommandTraces() const { return m_droppedCommandTraces; }
    const Diagnostics& diagnostics(usize seat) const { return m_seats.at(seat).diagnostics; }
    /** Last actual input consumed, retaining its original tick for hold expiration. */
    const std::optional<InputCommand>& previous(usize seat) const {
        return m_seats.at(seat).previous;
    }
    /** Bounded missing-input policy for this timeline; not online prediction. */
    static InputCommand heldOrNeutral(const std::optional<InputCommand>& previous, u64 epoch,
                                      u64 tick, u32 grant, u8 seat);

    /** Zero denotes no owner. Reassigning, even to the same peer, revokes old packets. */
    bool assign(usize seat, Peer peer);
    void disconnect(Peer peer);
    Peer owner(usize seat) const;
    u32 grant(usize seat) const;
    u64 epoch() const { return m_epoch; }
    u64 tick() const { return m_tick; }
    /** Reject all old-context inputs, keeping the roster. Never reuse an epoch. */
    void beginEpoch();
    Admission submit(Peer sender, const InputCommand& command);
    /** Consume one tick, once. Missing inputs repeat only held state briefly;
     * one-shot actions are never inferred or repeated. Late edges are discarded. */
    Frame advance();

private:
    void traceAdmission(Peer sender, const InputCommand& command, Admission admission);
    void traceExecution(Peer sender, const InputCommand& command, bool concealed);
    struct Seat {
        Peer owner = 0;
        u32 grant = 0;
        std::map<u64, InputCommand> pending;
        std::optional<InputCommand> previous;
        Diagnostics diagnostics;
    };
    std::array<Seat, InputCommand::kSeats> m_seats;
    std::deque<CommandTrace> m_commandTrace;
    u64 m_droppedCommandTraces = 0;
    u64 m_epoch = 1;
    u64 m_tick = 0;
};

} // namespace gdl::game
