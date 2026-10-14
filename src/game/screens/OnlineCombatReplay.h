#pragma once

#include <array>
#include <string_view>

#include "engine/core/Types.h"

#include "game/netplay/CombatPlayback.h"
#include "game/netplay/RollbackInputHistory.h"
#include "game/screens/CombatPrediction.h"

namespace gdl::game {
class StageParticlePresentation;
/** Whole-party native continuation from a completed host checkpoint. Local
 * input is retained verbatim; unknown remote ticks use the same bounded held
 * input policy as the host, never repeated button edges. This lane replaces
 * independent local movement only when every native step commits. Unsupported
 * interactions discard the entire pass and leave the existing fallback usable.
 * Predicted outputs never write saves or dispatch sound/rumble/reward events. */
class OnlineCombatReplay {
public:
    enum class Domain : usize { Unseeded, Legacy, Stage, Count };
    enum class PresentRefusal : usize {
        Inactive,
        Snapshot,
        Roster,
        ParticleInterval,
        StageSampling,
        Exception,
        Count
    };
    enum class Refusal : usize {
        Unbound,
        InvalidAuthority,
        InvalidInput,
        InputHistory,
        MissingSeed,
        Held,
        Cutscene,
        Resync,
        StaleAuthority,
        SeatMismatch,
        NoLocalInput,
        DivergentHorizon,
        Horizon,
        StageDecode,
        SeedRestore,
        InputGap,
        Picture,
        NativeInvalid,
        NativeUnsupported,
        Exception,
        Count
    };
    static constexpr std::array<std::string_view, static_cast<usize>(Domain::Count)> kDomains{
        "unseeded", "legacy", "stage"};
    static constexpr std::array<std::string_view, static_cast<usize>(PresentRefusal::Count)>
        kPresentRefusals{"inactive",          "snapshot",       "roster",
                         "particle_interval", "stage_sampling", "exception"};
    static constexpr std::array<std::string_view, static_cast<usize>(Refusal::Count)> kRefusals{
        "unbound",
        "invalid_authority",
        "invalid_input",
        "input_history",
        "missing_seed",
        "held",
        "cutscene",
        "resync",
        "stale_authority",
        "seat_mismatch",
        "no_local_input",
        "divergent_horizon",
        "horizon",
        "stage_decode",
        "seed_restore",
        "input_gap",
        "picture",
        "native_invalid",
        "native_unsupported",
        "exception"};
    /** Cumulative since begin(). An advanced prefix can still be refused; only
     * passes and presents establish committed replay and presentation. Legacy
     * means combat without Stage, not the separate local movement predictor.
     * Timings surround the existing calls, never another decode or simulation. */
    struct Timing {
        u64 count = 0;
        u64 nanoseconds = 0;
        u64 maximumNanoseconds = 0;
        void add(u64 elapsed);
        bool operator==(const Timing&) const = default;
    };
    struct Counters {
        u64 updates = 0;
        u64 passes = 0;
        u64 refusals = 0;
        u64 advancedTicks = 0;
        u64 invalidTicks = 0;
        u64 unsupportedTicks = 0;
        u64 presents = 0;
        u64 presentRefusals = 0;
        u64 decodeAttempts = 0;
        u64 decodeSuccesses = 0;
        u64 decodeFailures = 0;
        u64 decodeNanoseconds = 0;
        u64 advanceNanoseconds = 0;
        u64 restorePasses = 0;
        u64 appendPasses = 0;
        u64 reusedPasses = 0;
        u64 attemptedTicks = 0;
        u64 maximumTicksPerPass = 0;
        Timing passCost;
        Timing seedCost;
        Timing decodeCost;
        Timing restoreCost;
        Timing advanceCost;
        Timing captureCost;
        Timing prepareCost;
        Timing postconditionCost;
        std::array<Timing, static_cast<usize>(OrdinaryCombatCheckpoint::CaptureOwner::Count)>
            postconditionOwners{};
        Timing pictureCost;
        std::array<u64, static_cast<usize>(Refusal::Count)> reasons{};
        // Rendering also asks after a refused update. Inactive fallback is not
        // a failure to sample an accepted private picture.
        std::array<u64, static_cast<usize>(PresentRefusal::Count)> presentReasons{};
        std::array<u64, static_cast<usize>(OrdinaryCombatCheckpoint::RestoreIssue::Count)>
            seedIssues{};
        std::array<u64, static_cast<usize>(OrdinaryCombatTick::Refusal::Count)> tickRefusals{};
        std::array<u64, static_cast<usize>(OrdinaryCombatTick::PartyRefusal::Count)>
            partyRefusals{};
        std::array<u64, OrdinaryCombatTick::kActionCount> refusedActions{};
        std::array<u64, static_cast<usize>(OrdinaryCombatTick::TargetFamily::Count)>
            refusedTargets{};
        bool operator==(const Counters&) const = default;
    };
    struct Diagnostics {
        std::array<Counters, static_cast<usize>(Domain::Count)> domains{};
        bool operator==(const Diagnostics&) const = default;
    };
    bool begin(const MatchContext& context, u8 localPeer, CombatPrediction& prediction);
    void clear();
    bool update(const CombatSnapshot* authority, std::span<const InputCommand> sent);
    bool active() const;
    bool staged() const;
    bool present(CombatSnapshot& shown, EnemyPresentation& presentation, f32 blend);
    u64 tick() const;
    const Diagnostics& diagnostics() const { return *m_diagnostics; }

private:
    static Domain domainOf(const CombatSnapshot* state);
    bool refuse(Refusal reason);
    bool presentPass(CombatSnapshot& shown, EnemyPresentation& presentation, f32 blend,
                     PresentRefusal& refusal) const;
    void discard();
    bool updatePass(const CombatSnapshot* authority, std::span<const InputCommand> sent);
    std::unique_ptr<CombatSnapshot>
    picture(std::shared_ptr<const StageParticlePresentation>& particles) const;
    MatchContext m_context;
    u8 m_local = 0;
    CombatPrediction* m_prediction = nullptr;
    RollbackInputHistory m_history;
    // Native enemy pools make a full snapshot large even when its optional
    // supplement is absent. Keep retained snapshots off session/caller stacks.
    std::unique_ptr<CombatSnapshot> m_authority;
    std::unique_ptr<CombatSnapshot> m_previous;
    std::unique_ptr<CombatSnapshot> m_shown;
    std::shared_ptr<const StageParticlePresentation> m_previousParticles;
    std::shared_ptr<const StageParticlePresentation> m_shownParticles;
    // Resolve an immutable authority once. Restarts validate native bindings;
    // append-only continuation keeps that owner's exact accepted native state.
    std::unique_ptr<OrdinaryCombatCheckpoint::State> m_seed;
    std::weak_ptr<const u8> m_seedLifetime;
    using Commands = std::array<InputCommand, InputCommand::kSeats>;
    // Heap-backed, bounded by kMaxTicks, including remote held-input concealment.
    // Late real inputs can replace a previously concealed local tick.
    std::vector<Commands> m_executed;
    std::optional<OrdinaryCombatTick::Frame> m_nextFrame;
    u64 m_revision = 0;
    u64 m_clock = 0;
    u64 m_receivedAt = 0;
    Domain m_domain = Domain::Unseeded;
    // Diagnostic detail must not grow the session's stack footprint.
    std::unique_ptr<Diagnostics> m_diagnostics = std::make_unique<Diagnostics>();
};
} // namespace gdl::game
