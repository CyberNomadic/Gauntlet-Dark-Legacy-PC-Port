#pragma once

#include <array>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "game/netplay/InputCommand.h"

namespace gdl::game {

/** Input history for one independently simulated peer. Actual commands are
 * immutable evidence; used predictions are disposable. Gameplay snapshots,
 * transport, device polling, output confirmation and scene loading are callers'
 * responsibilities. All methods run on the simulation owner thread.
 *
 * tick() names the next pre-update state. On a mismatch, restore that state's
 * native snapshot before rewind(); replay never samples local devices again.
 * Only confirmedTick() can authorize irreversible effects or scene changes. */
class RollbackInputs {
public:
    using Owners = std::array<u8, InputCommand::kSeats>;
    using Grants = std::array<u32, InputCommand::kSeats>;
    using Commands = std::array<InputCommand, InputCommand::kSeats>;
    struct Frame {
        Commands commands{};
        u8 predictedSeats = 0;
    };
    enum class Admission : u8 {
        Accepted,
        Duplicate,
        Stale,
        Invalid,
        WrongPeer,
        WrongEpoch,
        WrongGrant,
        TooEarly,
        Conflict
    };
    static constexpr u64 kPredictionTicks = 8;
    static constexpr u64 kRedundantTicks = 8;
    static constexpr u64 kHistoryTicks = 128;
    static constexpr u64 kMaxAhead = 120;
    static constexpr u64 kPeerSkew = 2;
    static constexpr u8 kMaxDelay = 2;
    /** Round trips under which a startup delay of zero and of one tick still leave the
     * eight-tick prediction window enough room; longer trips take the longest delay. */
    static constexpr s32 kNoDelayPingMs = 20;
    static constexpr s32 kOneTickPingMs = 60;
    /** The local input delay a measured round trip calls for; an unknown ping keeps the
     * one-tick default. */
    static u8 leadForPing(s32 pingMs);

    /** Remote progress initially has unknown transit age. Use the conservative
     * eight-tick pacing allowance until its first report, without extending the
     * separate eight-unconfirmed-tick prediction window. */
    bool open(u8 localPeer, u64 epoch, const Owners& owners, const Grants& grants, u8 delay = 1);
    bool sampleReady() const;
    /** Exactly one complete local frame at sampleTick(). Edges may be accumulated
     * by the input adapter while this is backpressured, never merged after admission. */
    bool sample(std::span<const InputCommand> commands);
    /** Authenticate peer outside this class. Each represented tick must include
     * all that peer's occupied seats; up to eight ticks may arrive out of order.
     * Invalid/conflicting batches change no history. An actual/predicted mismatch
     * is Accepted and schedules correction, not a protocol conflict. */
    Admission submit(u8 authenticatedPeer, std::span<const InputCommand> commands);
    bool ready() const;
    std::optional<Frame> advance();
    /** Accept only the earliest pending correction after the caller restores its
     * native pre-tick snapshot. Actual inputs and unsent device edges are untouched. */
    bool rewind(u64 tick);
    /** Finish an active replay at the current next tick after executing at least
     * one corrected frame, when the native scene reached an earlier terminal.
     * No future simulation is fabricated. Actual inputs, the local sample cursor
     * and redundancy remain intact in case another correction removes the exit. */
    bool finishReplayAt(u64 nextTick);

    u64 tick() const { return m_next; }
    u64 sampleTick() const { return m_sample; }
    u64 confirmedTick() const;
    std::optional<u64> mismatchTick() const { return m_mismatch; }
    bool replaying() const { return m_next < m_replayEnd; }
    std::span<const InputCommand> redundantInputs() const { return m_redundancy; }
    /** Up to eight retained local frames beginning at firstTick. A transport
     * whose original sends are unreliable must also resend from each receiver's
     * acknowledged input watermark: latest-eight alone can drop its missing
     * oldest frame when prediction stalls with a local input buffer. Empty means
     * the requested beginning is unavailable, not that a gap may be filled. */
    std::vector<InputCommand> resendInputs(u64 firstTick) const;

    /** Current simulated progress, separate from future input timestamps. The
     * authenticated origin must survive relaying. Stale progress never regresses
     * the watermark; advertised jumps outside the bounded window are refused.
     * transitTicks is a bounded estimate of one-way delivery age supplied by the
     * transport, not by simulation or input lead. It affects pacing only. */
    Admission observePeerTick(u8 authenticatedPeer, u64 epoch, u64 nextTick, u8 transitTicks = 0);
    /** Forward-pacing advice only: continue pumping/sending during an idle tick.
     * Replaying old ticks never waits for peer pacing. advance() enforces the
     * eight-tick prediction bound independently of this scheduling advice. */
    bool timeSyncIdle() const;

private:
    using Actual = std::array<std::optional<InputCommand>, InputCommand::kSeats>;
    bool occupied(u8 peer) const;
    Admission admit(u8 peer, std::span<const InputCommand> commands);
    bool complete(const Actual& frame) const;
    InputCommand prediction(u8 seat, u64 tick) const;
    void trim();
    bool m_open = false;
    u8 m_local = 0;
    u8 m_delay = 0;
    u64 m_epoch = 0;
    u64 m_next = 1;
    u64 m_sample = 1;
    u64 m_actualThrough = 0;
    u64 m_replayEnd = 0;
    u64 m_replayStart = 0;
    Owners m_owners{};
    Grants m_grants{};
    std::array<u64, 5> m_peerTicks{};
    std::array<u8, 5> m_peerTransitTicks{};
    Actual m_predictionSeed;
    std::map<u64, Actual> m_actual;
    std::map<u64, Frame> m_used;
    std::optional<u64> m_mismatch;
    std::vector<InputCommand> m_redundancy;
};

} // namespace gdl::game
