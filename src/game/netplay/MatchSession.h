#pragma once

#include <chrono>
#include <deque>
#include <memory>

#include "game/netplay/LockstepInputs.h"
#include "game/netplay/NativeAgreement.h"
#include "game/netplay/PartyCheckpoint.h"
#include "game/netplay/ShopSync.h"

namespace gdl::game {

/** Frozen, room-admitted roster. Peer 1 is the host; these IDs are not socket handles. */
using MatchOwners = std::array<u8, InputCommand::kSeats>;
struct MatchLink {
    u8 peer = 0;
    PacketTransport::Connection connection = 0;
};
// Resume and the pause controls retain their old numeric values only so malformed
// obsolete requests can be rejected explicitly. Local menus never pause the shared clock.
enum class MatchTransition : u8 { Start, Travel, Resume, Shop, SecretEnter, SecretReturn };
enum class MatchStop : u8 { None, Left, Disconnected, LoadTimeout, TransportFailure };
enum class MatchControlKind : u8 {
    Prepare,
    Ready,
    Commit,
    Pause,
    RequestPause,
    Stop,
    MovieSkipVote
};
struct MatchContext {
    u64 epoch = 0;
    u32 scene = 0; // trusted build's scene roster, never a peer-supplied path
    u16 tickRate = 60;
    u8 inputLead = 3; // fixed negotiated startup delay, never an adaptive clock
    MatchTransition transition = MatchTransition::Start;
    MatchOwners owners{};
    std::array<u32, InputCommand::kSeats> grants{};
    bool challengeWon = false;
    // Travel adopts the next context only after the actual source scene executes
    // this terminal boundary. Both are zero on the initial Start context.
    u64 sourceEpoch = 0;
    u64 sourceTick = 0;
    bool valid() const;
    bool operator==(const MatchContext&) const = default;
};
struct MatchControl {
    MatchControlKind kind = MatchControlKind::Prepare;
    MatchStop stop = MatchStop::None;
    MatchContext context;
    u8 movieSkipVotes = 0; // occupied seat bits, never machine bits
    bool valid() const;
};
class MatchControlPacket {
public:
    static constexpr u8 kVersion = 6;
    static constexpr usize kBytes = 64;
    static std::optional<std::vector<u8>> encode(const MatchControl& control);
    static std::optional<MatchControl> decode(std::span<const u8> bytes);
};

/** Native shared-simulation lifecycle and reliable input routing.
 * Every peer executes the same complete frame. No missing-input concealment,
 * snapshots, replicated audio, host migration, mid-match joins or save writes.
 * The scene adapter acknowledges each issued frame only after executing it.
 * Load, movie, shop and source-terminal barriers retain reliable messages under
 * congestion; wall time can time out a peer but never advances gameplay. */
class MatchSession {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    enum class Phase : u8 { Offline, Lobby, Loading, Running, Stopped };
    enum class Admission : u8 { Accepted, Stale, WrongPeer, WrongPhase, Invalid };
    static constexpr f64 kLoadTimeout = 120;
    static constexpr usize kControlBudget = 16;
    static constexpr u32 kNativeAgreementPeriod = 60;

    bool open(u8 localPeer, const MatchOwners& owners, std::span<const MatchLink> links,
              u8 startupDelay = 3);
    /** A new transport/room lifetime is required before opening again after clear(). */
    void clear();
    /** Checkpointed transitions require a complete host party; Shop also its setup.
     * Invalid or congested preparation leaves the current epoch unchanged. */
    bool prepare(u32 scene, MatchTransition transition = MatchTransition::Start,
                 const MatchParty* party = nullptr, const ShopSetup* shop = nullptr,
                 bool challengeWon = false);
    ShopSync& shop() { return m_shop; }
    const ShopSync& shop() const { return m_shop; }
    const MatchParty* travelParty() const;
    bool loaded();
    bool requestMovieSkip(u8 seat);
    bool movieSkipReady() const;
    u8 movieSkipVotes() const { return m_movieSkipVotes; }
    /** Global pause has no shared tick boundary and is deliberately unsupported. */
    bool requestPause() { return false; }
    void leave();
    void disconnected(PacketTransport::Connection connection);
    /** seconds is lifecycle elapsed time; now is independent monotonic watchdog time. */
    void update(f64 seconds, Time now = Clock::now());
    Admission receive(PacketTransport::Connection connection, std::span<const u8> bytes,
                      std::optional<Time> now = std::nullopt);
    void flush(PacketTransport& transport, Time now = Clock::now());

    /** Exact local frame, accepted once without coalescing. The input adapter must
     * retain edges while inputReady() reports backpressure. */
    bool sample(std::span<const InputCommand> frame);
    std::span<const InputCommand> sampledInputs() const { return m_sampled; }
    bool inputReady() const;
    std::optional<LockstepInputs::Frame> advance();
    /** Acknowledge actual execution, not dequeue. At a pending travel fence this
     * adopts the new Loading context after the adapter verifies its local outcome. */
    bool completedNativeTick(u64 tick);
    /** Optional observed-state diagnostic, not a repair stream or full-state proof. */
    bool observeNative(u64 tick, const NativeAgreement::Digest& digest, Time now = Clock::now());
    const NativeAgreement* nativeAgreement() const { return m_nativeAgreement.get(); }
    bool nativeDraining() const { return m_pendingNativeContext.has_value(); }
    const MatchContext* pendingNativeContext() const {
        return m_pendingNativeContext ? &*m_pendingNativeContext : nullptr;
    }
    std::optional<u64> nativeEndTick() const {
        return m_pendingNativeContext ? std::optional{m_pendingNativeContext->sourceTick}
                                      : std::nullopt;
    }
    u64 inputTick() const { return m_lockstep ? m_lockstep->sampleTick() : 0; }
    u64 tick() const { return m_lockstep ? m_lockstep->tick() : 0; }
    bool nativeInputs() const { return m_phase != Phase::Offline; }
    bool host() const { return m_local == 1; }
    u8 localPeer() const { return m_local; }
    const MatchContext& context() const { return m_context; }
    Phase phase() const { return m_phase; }
    MatchStop stopReason() const { return m_stop; }
    const std::optional<InputCommand>& resolvedInput(usize seat) const {
        return m_nativeResolved.at(seat);
    }
    u8 inputLead() const { return m_startupDelay; }
    usize queuedControls() const { return m_controls.size(); }

private:
    struct ControlDelivery {
        PacketTransport::Connection connection;
        std::vector<u8> bytes;
    };
    u8 peerOf(PacketTransport::Connection connection) const;
    bool queue(u8 peer, MatchControlKind kind, MatchStop reason = MatchStop::None, u8 votes = 0);
    bool broadcast(MatchControlKind kind, MatchStop reason = MatchStop::None, u8 votes = 0);
    void tryCommit();
    void stop(MatchStop reason, bool notify);
    void discardGameplay();
    bool beginNativeInputs();
    bool ensureNativeAgreement(Time now);
    void failNativeAgreement(NativeAgreement::Failure fallback, u64 tick = 0);
    bool acceptPreparedContext(const MatchContext& context, MatchParty party = {});
    bool finishNativeDrain();
    Admission control(u8 peer, const MatchControl& message);
    Admission checkpoint(u8 peer, const PartyCheckpoint& message);

    Phase m_phase = Phase::Offline;
    MatchStop m_stop = MatchStop::None;
    u8 m_local = 0;
    MatchOwners m_owners{};
    std::array<PacketTransport::Connection, 5> m_links{};
    std::array<bool, 5> m_loaded{};
    u8 m_movieSkipVotes = 0;
    u8 m_movieSkipRequests = 0;
    MatchContext m_context;
    u8 m_startupDelay = 3;
    std::unique_ptr<LockstepInputs> m_lockstep;
    std::unique_ptr<NativeAgreement> m_nativeAgreement;
    std::array<std::optional<InputCommand>, InputCommand::kSeats> m_nativeResolved;
    u64 m_nativeCompleted = 0;
    std::optional<u64> m_nativeInFlight;
    std::optional<MatchContext> m_pendingNativeContext;
    MatchParty m_pendingNativeParty;
    std::unique_ptr<ShopSync> m_pendingNativeShop;
    u8 m_pendingNativeMovieVotes = 0;
    // Only the old relay survives loading, not another scene. Ready acknowledges
    // execution through its fence; tryCommit retires this relay.
    std::unique_ptr<LockstepInputs> m_drainingInputs;
    std::optional<MatchContext> m_drainingContext;
    MatchParty m_travelParty;
    ShopSync m_shop;
    std::vector<InputCommand> m_sampled;
    f64 m_loadingSeconds = 0;
    std::deque<ControlDelivery> m_controls;
};
} // namespace gdl::game
