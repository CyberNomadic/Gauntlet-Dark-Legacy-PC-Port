#pragma once

#include <chrono>
#include <deque>
#include <set>

#include "engine/net/PacketTransport.h"

#include "game/netplay/CombatSnapshot.h"
#include "game/netplay/InputCommand.h"

namespace gdl::game {
/** Local monotonic-clock diagnostics. No timestamps cross machines. Resolution
 * means the host passed an input's deadline, not that the input was executed.
 * Frame observations stop at draw submission, not GPU completion or scanout. */
class LatencyTelemetry {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    struct Summary {
        u64 count = 0;
        usize window = 0;
        f64 median = 0;
        f64 p95 = 0;
        f64 p99 = 0;
        f64 maximum = 0;
    };
    class Distribution {
    public:
        void add(f64 value);
        Summary summary() const;

    private:
        std::deque<f64> m_samples;
        u64 m_count = 0;
    };
    void begin(u64 epoch);
    void sampled(std::span<const InputCommand> commands, Time now = Clock::now());
    void sent(std::span<const InputCommand> commands, Time now = Clock::now());
    void checkpoint(const CombatSnapshot& state, Time now = Clock::now());
    /** Measure a pure movement edge against its last submitted visual origin.
     * Eligibility belongs to the latest authenticated checkpoint already passed
     * to checkpoint(), not the certificate-free drawing snapshot. Refused edges
     * count as misses; this observation does not prove accepted input. */
    void motionRequest(std::span<const InputCommand> commands, const CombatSnapshot* shown,
                       const CombatSnapshot* authority, Time now = Clock::now());
    /** Opt-in probe for an ordinary weapon throw from idle, not arbitrary attacks.
     * First-frame timing includes locally replayed animation. Independent host
     * action observation survives that presentation result. Call after sampling
     * the exact command, retaining the original press time and idle display.
     * The probe must also start from an idle latest host checkpoint. Target-tick
     * and consumed-input guards exclude earlier throws, but the latest consumed
     * command is not an action-origin ACK; neither metric proves this exact input
     * was accepted, or that a projectile spawned or hit. */
    void throwRequest(const InputCommand& input, const CombatSnapshot& shown,
                      Time now = Clock::now());
    /** Observe a locally owned native replay release, before drawing it. Its exact
     * shot identity times release-to-projectile submission and host observation;
     * it does not infer which sampled input originated the action. Replayed older
     * releases never start another sample, even after an observation expires. */
    void localRelease(const ShotIdentity& origin, Time now = Clock::now());
    void presented(const CombatSnapshot& shown, Time now = Clock::now());
    void network(const PacketTransport::Statistics& stats);
    void update(Time now = Clock::now());
    /** Detailed release records are opt-in for probes, not periodic runtime logs. */
    std::string report(bool includeReleases = false) const;
    const Distribution& firstMotion() const { return m_motionFrames; }
    const Distribution& resolved() const { return m_resolved; }
    const Distribution& sendQueue() const { return m_sendQueue; }
    u64 missedMotion() const { return m_missedMotion; }

private:
    struct Pending {
        u64 tick = 0;
        u8 seat = 0;
        Time sampled;
        bool sent = false;
        bool resolved = false;
    };
    struct Motion {
        Time sampled;
        SeatMotion origin;
        u64 shownTick = 0;
    };
    struct Throw {
        Time sampled;
        SeatMotion origin;
        u64 generation = 0;
        u64 tick = 0;
    };
    struct AuthoritativeAction {
        SeatMotion origin;
        u64 generation = 0;
        u64 tick = 0;
        u32 action = 0;
        bool movementEnabled = false;
    };
    struct Release {
        ShotIdentity shot;
        SeatMotion owner;
        Time observed;
        bool presented = false;
        bool authoritative = false;
        u64 sample = 0;
    };
    enum class ReleaseOutcome : u8 { Pending, Complete, Expired, Interrupted, Unobserved };
    struct ReleaseSample {
        ShotIdentity shot;
        Time observed;
        std::optional<f64> firstFrameMs;
        std::optional<f64> hostMs;
        ReleaseOutcome outcome = ReleaseOutcome::Pending;
    };
    ReleaseSample* releaseSample(const Release& release);
    void observeProjectiles(const CombatSnapshot& state, Time now, bool authoritative);
    u64 m_epoch = 0;
    std::deque<Pending> m_pending;
    std::array<bool, InputCommand::kSeats> m_moving{};
    std::array<std::optional<Motion>, InputCommand::kSeats> m_motion;
    std::array<std::optional<Throw>, InputCommand::kSeats> m_throws;
    std::array<std::optional<Throw>, InputCommand::kSeats> m_throwAuthority;
    std::array<std::optional<AuthoritativeAction>, InputCommand::kSeats> m_authoritativeActions;
    std::array<std::optional<SeatMotion>, InputCommand::kSeats> m_projectileOwners;
    std::array<std::optional<u64>, InputCommand::kSeats> m_releaseTicks;
    // Only the latest sampled tick is retained. Older rewinds remain behind its
    // fence; distinct same-tick turbo volleys retain complete identities.
    std::array<std::set<ShotIdentity>, InputCommand::kSeats> m_releaseIdentities;
    std::array<bool, InputCommand::kSeats> m_releaseSaturated{};
    std::optional<u64> m_projectileTick;
    std::deque<Release> m_releases;
    // Diagnostic window only: dropping an old sample never cancels its pending
    // observation or changes aggregate results. Indices provide constant-time pairing.
    std::deque<ReleaseSample> m_releaseSamples;
    u64 m_releaseCount = 0;
    Distribution m_sendQueue;
    Distribution m_resolved;
    Distribution m_confirmedFrames;
    Distribution m_motionFrames;
    Distribution m_throwFrames;
    Distribution m_throwConfirmed;
    Distribution m_projectileFrames;
    Distribution m_projectileConfirmed;
    Distribution m_rtt;
    Distribution m_updateIntervals;
    Distribution m_renderIntervals;
    std::optional<Time> m_updated;
    std::optional<Time> m_rendered;
    std::optional<PacketTransport::Statistics> m_network;
    u64 m_expiredInputs = 0;
    u64 m_missedMotion = 0;
    u64 m_missedThrows = 0;
    u64 m_expiredThrowAuthority = 0;
    u64 m_interruptedThrowAuthority = 0;
    u64 m_supersededThrowAuthority = 0;
    u64 m_expiredReleases = 0;
    u64 m_interruptedReleases = 0;
    u64 m_unobservedProjectiles = 0;
    u64 m_pathChanges = 0;
};
} // namespace gdl::game
