#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/netplay/InputCommand.h"
#include "game/netplay/RollbackInputs.h"

namespace gdl::game {

/** Every seat's actual command for one confirmed tick, in seat order; an unoccupied seat
 * holds a default command. */
using JournalFrame = std::array<InputCommand, InputCommand::kSeats>;

/**
 * The confirmed input record of one epoch from its first tick. The simulation is
 * deterministic from the stage's load, so a peer that lost its way, or one arriving late,
 * reloads the stage and replays this record to stand where everyone else stands. Frames
 * are appended in tick order and never change.
 */
class EpochInputJournal {
public:
    static constexpr usize kMaxFrames = 60 * 60 * 30; ///< half an hour at the native rate

    bool open(u64 epoch, const RollbackInputs::Owners& owners);
    void clear();
    /** Appends the frame of the next tick; its commands must carry the epoch, tick, seat
     * and owner the journal expects. */
    bool append(u64 tick, const JournalFrame& frame);

    bool opened() const { return m_epoch != 0; }
    u64 epoch() const { return m_epoch; }
    const RollbackInputs::Owners& owners() const { return m_owners; }
    /** The last tick recorded; zero before the first. */
    u64 through() const { return m_frames.size(); }
    std::span<const JournalFrame> frames() const { return m_frames; }
    const JournalFrame* frame(u64 tick) const {
        return tick >= 1 && tick <= m_frames.size() ? &m_frames[tick - 1] : nullptr;
    }

private:
    u64 m_epoch = 0;
    RollbackInputs::Owners m_owners{};
    std::vector<JournalFrame> m_frames;
};

/**
 * One reliable, bounded piece of a journal transfer: `count` ticks from `firstTick` for the
 * seats in `seats`, each seat's commands packed as runs of identical ticks, since steadied
 * hands repeat themselves. A transfer ends with a chunk marked last. Partial frames (a
 * subset of seats) carry the unconfirmed tail a peer already sent.
 */
class JournalPacket {
public:
    struct Chunk {
        u64 epoch = 0;
        u64 firstTick = 0;
        u16 count = 0;
        u8 seats = 0; ///< bit per seat present
        bool last = false;
        /** For each present seat in seat order, `count` commands in tick order. */
        std::vector<InputCommand> commands;
        bool valid() const;
    };
    static constexpr u8 kVersion = 1;
    static constexpr usize kHeaderBytes = 28;
    static constexpr usize kRunBytes = 41;
    static constexpr usize kMaxBytes = 1200;
    static constexpr u16 kMaxTicks = 4096;

    static bool recognizes(std::span<const u8> bytes);
    static std::optional<std::vector<u8>> encode(const Chunk& chunk);
    static std::optional<Chunk> decode(std::span<const u8> bytes);

    /** Cuts the journal's ticks `firstTick`..`lastTick` into chunks that each fit the wire;
     * the final one is marked last when `markLast`. Empty when the range is not recorded. */
    static std::vector<Chunk> chunks(const EpochInputJournal& journal, u64 firstTick, u64 lastTick,
                                     bool markLast);
};

} // namespace gdl::game
