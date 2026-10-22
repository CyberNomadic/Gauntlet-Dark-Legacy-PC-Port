#pragma once

#include <array>
#include <deque>
#include <map>
#include <optional>

#include "engine/net/PacketTransport.h"

#include "game/rollback/InputCommand.h"

namespace gdl::game {

/** Input-only playout for independently running native simulations. Missing inputs
 * stall; they are never guessed, dropped, or replaced by host state. The caller
 * supplies an authenticated, frozen roster after a shared load barrier. Peer 1
 * relays inputs immediately, not after its simulation/render completes.
 *
 * This owns no gameplay, clocks, device polling, or provider. Call on one thread.
 * Reliable deliveries are bounded per destination; congestion must not discard
 * one-shot actions. Disconnects end the epoch (no silent neutral substitution).
 * A new level needs a new epoch and a fresh instance after its load barrier. */
class LockstepInputs {
public:
    using Owners = std::array<u8, InputCommand::kSeats>;
    using Grants = std::array<u32, InputCommand::kSeats>;
    using Frame = std::array<InputCommand, InputCommand::kSeats>;
    struct Link {
        u8 peer = 0;
        PacketTransport::Connection connection = 0;
    };
    enum class Admission : u8 {
        Accepted,
        Duplicate,
        Stale,
        WrongPeer,
        Invalid,
        TooEarly,
        Conflict,
        Full
    };
    static constexpr u64 kWindow = 120;
    static constexpr usize kQueuedPackets = 128;
    static constexpr u8 kMaxDelay = 6;

    bool open(u8 localPeer, u64 epoch, const Owners& owners, const Grants& grants, u8 delay,
              std::span<const Link> links);
    bool sample(std::span<const InputCommand> commands);
    Admission receive(PacketTransport::Connection connection, std::span<const u8> bytes);
    bool flush(PacketTransport& transport);
    void disconnected(PacketTransport::Connection connection);
    bool sampleReady() const;
    bool ready() const;
    std::optional<Frame> advance();
    bool failed() const { return m_failed; }
    u64 tick() const { return m_tick; }         ///< next simulation tick, starting at one
    u64 sampleTick() const { return m_sample; } ///< next local input tick, including delay
    usize queuedPackets() const;

private:
    using Pending = std::array<std::optional<InputCommand>, InputCommand::kSeats>;
    Admission admit(u8 peer, std::span<const InputCommand> commands);
    bool queueReady(u8 source) const;
    void queue(u8 source, std::span<const InputCommand> commands);
    bool occupied(u8 peer) const;
    bool m_open = false;
    bool m_failed = false;
    u8 m_local = 0;
    u8 m_delay = 0;
    u64 m_epoch = 0;
    u64 m_tick = 1;
    u64 m_sample = 1;
    Owners m_owners{};
    Grants m_grants{};
    std::array<PacketTransport::Connection, 5> m_links{};
    std::array<std::deque<std::vector<u8>>, 5> m_outgoing;
    std::map<u64, Pending> m_pending;
    std::map<u64, Frame> m_history;
};
} // namespace gdl::game
