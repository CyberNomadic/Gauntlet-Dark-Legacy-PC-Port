#pragma once

#include <array>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/netplay/InputCommand.h"

namespace gdl::game {
/** Accepted local controls retained until an authoritative post-step checkpoint.
 * Receipt watermarks are not acknowledgements and never retire this history.
 * A bounded, contiguous replay uses the host's missing-input policy; this class
 * neither admits late host commands nor restores world simulation state. */
class RollbackInputHistory {
public:
    static constexpr u64 kHistory = 120;
    using Grants = std::array<u32, InputCommand::kSeats>;
    struct Entry {
        InputCommand input;
        u64 sampled = 0;        ///< local simulation clock, not a remote timestamp
        bool predicted = false; ///< missing tick filled by held/neutral playout
    };

    /** Zero grants are seats not owned locally. Reset on epoch/ownership changes. */
    bool begin(u64 epoch, const Grants& grants);
    void clear();
    /** Atomically record one sampled frame. Repeats must be identical; a future
     * lead adjustment may leave gaps, which replay fills without repeating edges.
     * Overflow latches resyncRequired instead of discarding unresolved commands. */
    bool record(std::span<const InputCommand> commands, u64 sampled = 0);
    /** State K already includes input K; only K+1 and later are replayed.
     * Reordered, repeated and foreign-epoch checkpoints leave history unchanged. */
    bool resolve(u64 epoch, u64 postStepTick);
    /** Discard a seat's speculation on a fresh hard correction/control revocation. */
    void invalidate(usize seat);
    /** The seed is the last actual input consumed by the host at the checkpoint,
     * with its original tick. It must not be the newest received future input or
     * a concealed frame stamped with the checkpoint's tick. Returned storage is
     * stable until this seat is replayed or the history is mutated. */
    std::span<const Entry> replay(usize index,
                                  const std::optional<InputCommand>& previous = std::nullopt);
    usize count(usize seat) const;
    usize pending() const;
    bool resyncRequired(usize seat) const;
    bool resyncRequired() const;
    std::optional<u64> resolved() const { return m_resolved; }
    u64 epoch() const { return m_epoch; }

private:
    struct Seat {
        std::map<u64, Entry> commands;
        std::vector<Entry> replay;
        bool exhausted = false;
    };
    std::array<Seat, InputCommand::kSeats> m_seats;
    Grants m_grants{};
    u64 m_epoch = 0;
    std::optional<u64> m_resolved;
};
} // namespace gdl::game
