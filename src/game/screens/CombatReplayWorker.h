#pragma once

#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "game/screens/OnlineCombatReplay.h"

namespace gdl::game {
/** One CPU-only Stage producer. Loading, rendering, network/audio dispatch and
 * resource destruction stay on the application thread. Between start and stop,
 * no other caller may read or mutate the borrowed CombatPrediction, including
 * diagnostics, resume and clear. stop joins before returning. Then release all
 * detached publications/presenters before destroying artwork on the main thread:
 * the weak resource marker is an invalidation fence, not an asset lease. */
class CombatReplayWorker {
public:
    static constexpr usize kMaxCommands = RollbackInputHistory::kHistory * InputCommand::kSeats;
    enum class Submission {
        Queued,
        Unchanged,
        Stopped,
        Authority,
        Input,
        Conflict,
        Capacity,
        Resources,
        Exception
    };
    enum class Status { Published, Unsupported, Superseded, Resources, Exception };

    struct Request {
        u64 revision = 0;
        MatchContext context;
        u8 localPeer = 0;
        std::shared_ptr<const CombatSnapshot> authority;
        // Complete exact locally sampled suffix after authority->motion.tick.
        // Sparse ticks stay sparse: only the native replay fills held inputs.
        std::vector<InputCommand> commands;
    };
    struct Output {
        Status status = Status::Unsupported;
        std::shared_ptr<const StageReplayPresentation::Publication> publication;
        std::shared_ptr<const OnlineCombatReplay::Diagnostics> diagnostics;
    };
    struct Completion : Output {
        u64 request = 0;
        MatchContext context;
        u64 authorityTick = 0;
        u64 inputThroughTick = 0; ///< published endpoint, or requested horizon on refusal
        std::weak_ptr<const void> resources;
    };
    struct QueueState {
        bool running = false;
        bool inFlight = false;
        bool pending = false;
        bool completed = false;
        bool failed = false; ///< terminal resource/executor failure; stop before restarting
        usize retainedCommands = 0;
        u64 submitted = 0;
        u64 coalesced = 0;
        u64 executed = 0;
        u64 superseded = 0;
        u64 rebaseAttempts = 0;
        u64 rebased = 0; ///< successful proof; a still-newer authority may supersede it
    };

    CombatReplayWorker();
    ~CombatReplayWorker();
    CombatReplayWorker(const CombatReplayWorker&) = delete;
    CombatReplayWorker& operator=(const CombatReplayWorker&) = delete;
    CombatReplayWorker(CombatReplayWorker&&) = delete;
    CombatReplayWorker& operator=(CombatReplayWorker&&) = delete;

    /** Called on main after load and before any concurrent native access. A
     * failed start leaves no worker. Legacy combat must use a separate stopped
     * synchronous coordinator; this producer accepts Stage authorities only. */
    bool start(const MatchContext& context, u8 localPeer, CombatPrediction& prediction);

    /** Deterministic execution seam for the bounded scheduler. run and finish
     * execute exclusively on the producer thread, and must not load/draw or
     * dispatch irreversible outputs. After a successful start, finish runs once,
     * including after exceptions, while borrowed owners still live. Native start supplies the
     * real OnlineCombatReplay adapter; this overload also permits barrier-driven
     * scheduler tests without timing-dependent native asset work. */
    struct Executor {
        std::function<Output(const Request&)> run;
        std::function<void()> finish;
        // Optional exact-authority-only reconciliation at the completed native
        // endpoint. Called at most once per job, never under the mailbox mutex.
        // It must not advance through the request's newly appended inputs.
        std::function<Output(const Request&)> rebase;
    };
    bool start(const MatchContext& context, u8 localPeer, std::weak_ptr<const void> resources,
               Executor executor);

    /** Main-thread submission of a complete authenticated checkpoint and exact
     * already-sampled commands. Newer authority is copied once; null/repeated
     * authority reuses that immutable copy. Same-tick authority must be the same
     * accepted checkpoint. No packet timestamps or input edges are rewritten.
     * Journal entries retire only in a request containing authority at/after
     * their tick, never merely because a worker consumed them. Rejection leaves
     * the pending request unchanged; the caller must withdraw presentation on a
     * session/admission failure rather than treating an old result as current. */
    Submission submit(const CombatSnapshot* authority, std::span<const InputCommand> commands);
    /** Transfer the latest completion; intermediates may have been replaced.
     * The returned object never reads native owners and remains valid after
     * stop. Its borrowed artwork still requires the explicit teardown order. */
    std::shared_ptr<const Completion> take();
    QueueState queueState() const;
    /** No mid-transaction interruption: finish the in-flight pass, discard any
     * pending work and join. Native finish rewinds to its authority baseline.
     * Afterwards native owner access is again exclusive to the application. */
    void stop();

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};
} // namespace gdl::game
