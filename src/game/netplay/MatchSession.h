#pragma once

#include <deque>
#include <memory>

#include "game/netplay/AudioSync.h"
#include "game/netplay/CombatPlayback.h"
#include "game/netplay/FeedbackSync.h"
#include "game/netplay/InputTimeline.h"
#include "game/netplay/LatencyTelemetry.h"
#include "game/netplay/PartyCheckpoint.h"
#include "game/netplay/ShopSync.h"

namespace gdl::game {

/** Frozen, room-admitted roster. Peer 1 is the host; these small IDs are NOT socket
 * handles. In particular the first remote socket can be 1 without owning the host's seats. */
using MatchOwners = std::array<u8, InputCommand::kSeats>;
struct MatchLink {
    u8 peer = 0;
    PacketTransport::Connection connection = 0;
};
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
    u32 scene = 0; // trusted build's scene roster, never a path supplied by a peer
    u16 tickRate = 60;
    // Start one checkpoint interval ahead. Admission feedback raises this on
    // slower/jittery paths; a local connection need not pay a fixed 100 ms lead.
    u8 inputLead = 3;
    MatchTransition transition = MatchTransition::Start;
    MatchOwners owners{};
    std::array<u32, InputCommand::kSeats> grants{};
    bool challengeWon = false; ///< authenticated completion only on SecretReturn
    bool valid() const;
    bool operator==(const MatchContext&) const = default;
};
struct MatchControl {
    MatchControlKind kind = MatchControlKind::Prepare;
    MatchStop stop = MatchStop::None;
    MatchContext context;
    u8 movieSkipVotes = 0; // one bit per occupied seat, never one bit per machine
    bool valid() const;
};
class MatchControlPacket {
public:
    static constexpr usize kBytes = 48;
    static std::optional<std::vector<u8>> encode(const MatchControl& control);
    static std::optional<MatchControl> decode(std::span<const u8> bytes);
};

/** Simulation-thread match lifecycle and packet routing, independent of HTTP/UI/assets.
 * open() takes only an authenticated frozen room roster. The host prepares a scene;
 * each machine explicitly confirms it has loaded before any simulation may advance.
 * Reliable control is retained on congestion. Completed-checkpoint receipts bound
 * each peer's independent publication window; unsent captures coalesce in place.
 * No host migration, mid-match joins or save writes.
 * The scene adapter must call advance() before stepping gameplay; clients never get a frame. */
class MatchSession {
public:
    enum class Phase : u8 { Offline, Lobby, Loading, Running, Paused, Stopped };
    enum class Admission : u8 { Accepted, Stale, WrongPeer, WrongPhase, Invalid };
    // Includes the native route/preview and encounter movie before asset readiness.
    // Peers keep pumping transport throughout; an absent viewer still times out.
    static constexpr f64 kLoadTimeout = 120;
    static constexpr usize kControlBudget = 16;
    static constexpr usize kPublicationWindow = 2;
    static constexpr usize kPublicationBurst = 64;
    static constexpr f64 kPublicationTimeout = 1;
    // Two 20 Hz snapshot intervals of slack beyond the negotiated input lead.
    // A quiet/stalled host must never let client input numbering run away.
    static constexpr u64 kInputSlack = 6;
    static constexpr u8 kMaxInputLead = 30;

    bool open(u8 localPeer, const MatchOwners& owners, std::span<const MatchLink> links);
    /** A new transport/room lifetime is required before opening again after clear(). */
    void clear();
    /** Travel/Shop/SecretEnter/SecretReturn require a complete host checkpoint;
     * Shop also requires its setup, and only SecretReturn may carry challengeWon.
     * Start/Resume take neither. A shop cannot travel before every peer finishes.
     * Invalid or congested preparation leaves the current epoch unchanged. */
    bool prepare(u32 scene, MatchTransition transition = MatchTransition::Start,
                 const MatchParty* party = nullptr, const ShopSetup* shop = nullptr,
                 bool challengeWon = false);
    ShopSync& shop() { return m_shop; }
    const ShopSync& shop() const { return m_shop; }
    FeedbackSync& feedback() { return m_feedback; }
    AudioSync& audio() { return m_audio; }
    const FeedbackSync& feedback() const { return m_feedback; }
    /** Complete travel/shop profiles only, never a partially received roster. */
    const MatchParty* travelParty() const;
    bool loaded();
    /** Entry movies may skip only after the host acknowledges every occupied seat.
     * Votes are reliable, idempotent and scoped to this Start/Travel epoch. */
    bool requestMovieSkip(u8 seat);
    bool movieSkipReady() const;
    u8 movieSkipVotes() const { return m_movieSkipVotes; }
    bool requestPause();
    void leave();
    void disconnected(PacketTransport::Connection connection);
    void update(f64 seconds);
    /** Out-of-phase gameplay datagrams are discarded as Stale. They may overtake
     * reliable start/travel control or arrive late after pause; neither is fatal. */
    Admission receive(PacketTransport::Connection connection, std::span<const u8> bytes);
    void flush(PacketTransport& transport);

    /** One complete local frame per 60 Hz local update, without UI pointers.
     * Guests ahead of the latest host checkpoint coalesce into one pending frame:
     * latest held state/aim plus one pending edge per action, not a replay backlog.
     * Acceptance need not advance inputTick() when the host has fallen behind. */
    bool sample(std::span<const InputCommand> frame);
    /** Exact last accepted sample, including coalesced edges; empty while throttled. */
    std::span<const InputCommand> sampledInputs() const { return m_sampled; }
    bool inputReady() const;
    std::optional<InputTimeline::Frame> advance();
    /** Check before expensive capture/encoding. Full peers skip publications;
     * one ready peer is sufficient. Timeout credit comes only from update(). */
    bool publicationReady() const;
    bool publish(const CombatSnapshot& state);
    bool publish(const CombatPacket::Prepared& state);
    u64 inputTick() const { return host() ? m_timeline.tick() : m_nextInput; }
    u64 tick() const { return m_timeline.tick(); }
    bool host() const { return m_local == 1; }
    u8 localPeer() const { return m_local; }
    const MatchContext& context() const { return m_context; }
    Phase phase() const { return m_phase; }
    MatchStop stopReason() const { return m_stop; }
    const CombatPlayback& playback() const { return m_playback; }
    const InputTimeline::Diagnostics& inputDiagnostics(usize seat) const {
        return m_timeline.diagnostics(seat);
    }
    const std::optional<InputCommand>& resolvedInput(usize seat) const {
        return m_timeline.previous(seat);
    }
    u8 inputLead() const { return std::max(m_context.inputLead, m_adaptiveLead); }
    LatencyTelemetry& latency() { return m_latency; }
    const LatencyTelemetry& latency() const { return m_latency; }
    usize queuedControls() const { return m_controls.size(); }

private:
    bool canPublish(const MotionSnapshot& motion) const;
    struct ControlDelivery {
        PacketTransport::Connection connection;
        std::vector<u8> bytes;
    };
    struct StateBatch {
        std::shared_ptr<const CombatReplica::Packets> packets;
        CombatReplica::Receipt identity;
        usize next = 0;
        f64 remaining = kPublicationTimeout;
        bool acknowledged = false;
    };
    struct StateDelivery {
        std::deque<StateBatch> batches;
        bool ready() const {
            return batches.size() < kPublicationWindow || batches.back().next == 0;
        }
    };
    u8 peerOf(PacketTransport::Connection connection) const;
    bool queue(u8 peer, MatchControlKind kind, MatchStop reason = MatchStop::None, u8 votes = 0);
    bool broadcast(MatchControlKind kind, MatchStop reason = MatchStop::None, u8 votes = 0);
    void tryCommit();
    void stop(MatchStop reason, bool notify);
    void discardGameplay();
    Admission control(u8 peer, const MatchControl& message);
    Admission checkpoint(u8 peer, const PartyCheckpoint& message);
    Admission receipt(u8 peer, const CombatReplica::Receipt& message);
    void resetLeadRecovery();

    Phase m_phase = Phase::Offline;
    MatchStop m_stop = MatchStop::None;
    u8 m_local = 0;
    MatchOwners m_owners{};
    std::array<PacketTransport::Connection, 5> m_links{};
    std::array<bool, 5> m_loaded{};
    u8 m_movieSkipVotes = 0;
    u8 m_movieSkipRequests = 0;
    MatchContext m_context;
    MatchParty m_travelParty;
    ShopSync m_shop;
    FeedbackSync m_feedback;
    AudioSync m_audio;
    InputTimeline m_timeline;
    InputHistory m_history;
    std::vector<InputCommand> m_sampled;
    CombatPlayback m_playback;
    u64 m_nextInput = 0;
    u8 m_adaptiveLead = 0;
    u64 m_leadFeedbackFloor = 0;
    std::optional<u64> m_healthyLeadSince;
    u32 m_healthyLeadSamples = 0;
    s32 m_healthyLeadMargin = 0;
    u64 m_sampleClock = 0;
    struct SentInput {
        u64 tick;
        u64 sampled;
    };
    std::deque<SentInput> m_sentInputs;
    std::array<u64, InputCommand::kSeats> m_feedbackInput{};
    std::array<u32, InputCommand::kSeats> m_pendingPress{};
    std::optional<u64> m_published;
    f64 m_loadingSeconds = 0;
    std::deque<ControlDelivery> m_controls;
    std::vector<u8> m_input;
    std::array<StateDelivery, 5> m_states;
    std::optional<CombatReplica::Receipt> m_receipt;
    LatencyTelemetry m_latency;
    LatencyTelemetry::Time m_statisticsAt;
};
} // namespace gdl::game
