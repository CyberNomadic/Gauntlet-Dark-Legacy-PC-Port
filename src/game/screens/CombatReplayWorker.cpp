#include "game/screens/CombatReplayWorker.h"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace gdl::game {
namespace {
bool sameInput(const InputCommand& a, const InputCommand& b) {
    return a.epoch == b.epoch && a.tick == b.tick && a.grant == b.grant && a.seat == b.seat &&
           a.direction == b.direction && a.magnitude == b.magnitude &&
           a.aimDirection == b.aimDirection && a.heldButtons == b.heldButtons &&
           a.pressedButtons == b.pressedButtons;
}
bool inputBefore(const InputCommand& a, const InputCommand& b) {
    return a.tick < b.tick || (a.tick == b.tick && a.seat < b.seat);
}
bool stageAuthority(const CombatSnapshot& state, const MatchContext& context) {
    if (state.motion.epoch != context.epoch || !state.combatState || !state.combatState->stage ||
        !state.combatFrame || !state.opponents || !state.opponentFrame) {
        return false;
    }
    for (usize seat = 0; seat < context.owners.size(); ++seat) {
        const bool occupied = context.owners[seat] != 0;
        if (state.players[seat].has_value() != occupied ||
            state.motion.players[seat].has_value() != occupied ||
            (occupied && state.motion.players[seat]->grant != context.grants[seat])) {
            return false;
        }
    }
    return true;
}
} // namespace

struct CombatReplayWorker::Data {
    MatchContext context;
    u8 local = 0;
    std::weak_ptr<const void> resources;
    Executor executor;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::thread thread;
    QueueState state;
    bool stopping = false;
    bool failed = false;
    u64 revision = 0;
    std::shared_ptr<const CombatSnapshot> authority;
    std::vector<InputCommand> journal;
    std::shared_ptr<const Completion> completed;

    void run() noexcept {
        for (;;) {
            Request request;
            std::shared_ptr<Completion> result;
            try {
                {
                    std::unique_lock lock{mutex};
                    ready.wait(lock, [&] { return stopping || state.pending; });
                    if (stopping) {
                        break;
                    }
                    request.revision = revision;
                    request.context = context;
                    request.localPeer = local;
                    request.authority = authority;
                    request.commands = journal;
                    state.pending = false;
                    state.inFlight = true;
                }
                result = std::make_shared<Completion>();
                result->request = request.revision;
                result->context = context;
                result->authorityTick = request.authority->motion.tick;
                result->inputThroughTick =
                    request.commands.empty() ? result->authorityTick : request.commands.back().tick;
                result->resources = resources;
                if (resources.expired()) {
                    result->status = Status::Resources;
                } else {
                    static_cast<Output&>(*result) = executor.run(request);
                    if (resources.expired()) {
                        result->status = Status::Resources;
                        result->publication.reset();
                    }
                }
            } catch (...) {
                // Inability to allocate even a completion is visible through
                // running=false. Never let an exception escape std::thread.
                if (result) {
                    result->status = Status::Exception;
                    result->publication.reset();
                }
                std::lock_guard lock{mutex};
                failed = true;
            }
            bool terminal = false;
            {
                std::lock_guard lock{mutex};
                state.inFlight = false;
                ++state.executed;
                terminal = failed || !result || result->status == Status::Exception ||
                           result->status == Status::Resources;
                if (result && !terminal && authority != request.authority) {
                    // Never hand off an older authority's picture after a newer
                    // checkpoint was submitted while this native pass ran.
                    result->status = Status::Superseded;
                    result->publication.reset();
                    ++state.superseded;
                }
                if (result && !stopping) {
                    completed = std::move(result);
                    state.completed = true;
                }
                if (terminal) {
                    failed = true;
                    state.failed = true;
                    state.pending = false;
                }
            }
            if (terminal) {
                break;
            }
        }
        try {
            if (executor.finish) {
                executor.finish();
            }
        } catch (...) {
            std::lock_guard lock{mutex};
            failed = true;
            state.failed = true;
            // A failed undo is fatal; never leave a successful publication as
            // evidence that this producer can continue safely.
            completed.reset();
            state.completed = false;
        }
        std::lock_guard lock{mutex};
        state.running = false;
        state.inFlight = false;
        state.pending = false;
    }
};

CombatReplayWorker::CombatReplayWorker() = default;
CombatReplayWorker::~CombatReplayWorker() {
    stop();
}

bool CombatReplayWorker::start(const MatchContext& context, u8 localPeer,
                               CombatPrediction& prediction) {
    stop();
    auto replay = std::make_shared<OnlineCombatReplay>();
    if (!replay->begin(context, localPeer, prediction)) {
        return false;
    }
    Executor executor;
    executor.run = [replay](const Request& request) {
        Output result;
        try {
            if (replay->update(request.authority.get(), request.commands)) {
                result.publication = replay->publication();
                result.status = result.publication ? Status::Published : Status::Unsupported;
            }
        } catch (...) {
            // update rewinds on failure; publication allocation may fail after
            // update accepted. The scheduler always invokes native finish before
            // terminating this producer and returning ownership to main.
            result.status = Status::Exception;
            result.publication.reset();
        }
        result.diagnostics =
            std::make_shared<const OnlineCombatReplay::Diagnostics>(replay->diagnostics());
        return result;
    };
    // This CPU-only reset needs no fresh diagnostic allocation. OnlineCombatReplay
    // itself is destroyed on main after the joined executor is released.
    executor.finish = [&prediction] { prediction.resetReplay(); };
    return start(context, localPeer, prediction.resourceLifetime(), std::move(executor));
}

bool CombatReplayWorker::start(const MatchContext& context, u8 localPeer,
                               std::weak_ptr<const void> resources, Executor executor) {
    stop();
    if (!context.valid() || localPeer == 0 || resources.expired() || !executor.run ||
        std::ranges::find(context.owners, localPeer) == context.owners.end()) {
        return false;
    }
    auto data = std::make_unique<Data>();
    data->context = context;
    data->local = localPeer;
    data->resources = std::move(resources);
    data->executor = std::move(executor);
    data->state.running = true;
    try {
        data->thread = std::thread{[owner = data.get()] { owner->run(); }};
    } catch (...) {
        return false;
    }
    m_data = std::move(data);
    return true;
}

CombatReplayWorker::Submission CombatReplayWorker::submit(const CombatSnapshot* incoming,
                                                          std::span<const InputCommand> commands) {
    if (!m_data) {
        return Submission::Stopped;
    }
    auto& data = *m_data;
    std::lock_guard lock{data.mutex};
    if (!data.state.running || data.stopping || data.failed) {
        return Submission::Stopped;
    }
    if (data.resources.expired()) {
        return Submission::Resources;
    }
    if (commands.size() > kMaxCommands) {
        return Submission::Capacity;
    }
    if (incoming && (!stageAuthority(*incoming, data.context) ||
                     (data.authority && incoming->motion.tick < data.authority->motion.tick))) {
        return Submission::Authority;
    }
    const bool newer =
        incoming && (!data.authority || incoming->motion.tick > data.authority->motion.tick);
    const auto* baseline = newer ? incoming : data.authority.get();
    if (baseline == nullptr) {
        return Submission::Authority;
    }
    try {
        auto merged = data.journal;
        std::erase_if(merged,
                      [&](const auto& input) { return input.tick <= baseline->motion.tick; });
        bool changed = newer;
        for (const auto& input : commands) {
            if (!input.valid() || input.epoch != data.context.epoch ||
                data.context.owners[input.seat] != data.local ||
                input.grant != data.context.grants[input.seat]) {
                return Submission::Input;
            }
            if (input.tick <= baseline->motion.tick) {
                continue;
            }
            if (input.tick - baseline->motion.tick > RollbackInputHistory::kHistory) {
                return Submission::Capacity;
            }
            const auto at = std::lower_bound(merged.begin(), merged.end(), input, inputBefore);
            if (at != merged.end() && at->tick == input.tick && at->seat == input.seat) {
                if (!sameInput(*at, input)) {
                    return Submission::Conflict;
                }
                continue;
            }
            if (merged.size() == kMaxCommands) {
                return Submission::Capacity;
            }
            merged.insert(at, input);
            changed = true;
        }
        if (!changed) {
            return Submission::Unchanged;
        }
        if (data.revision == std::numeric_limits<u64>::max()) {
            return Submission::Capacity;
        }
        // Full authority is copied only for a genuinely newer checkpoint. The
        // consumer validates all native supplement fields before using it.
        auto authority = newer ? std::make_shared<const CombatSnapshot>(*incoming) : data.authority;
        data.authority = std::move(authority);
        data.journal = std::move(merged);
        data.state.retainedCommands = data.journal.size();
        ++data.state.submitted;
        data.state.coalesced += data.state.pending ? 1 : 0;
        ++data.revision;
        data.state.pending = true;
        // New authority immediately revokes a completion still in the mailbox.
        // An already taken publication is checked by its detached presenter.
        if (newer) {
            data.completed.reset();
            data.state.completed = false;
        }
        data.ready.notify_one();
        return Submission::Queued;
    } catch (...) {
        return Submission::Exception;
    }
}

std::shared_ptr<const CombatReplayWorker::Completion> CombatReplayWorker::take() {
    if (!m_data) {
        return nullptr;
    }
    std::lock_guard lock{m_data->mutex};
    m_data->state.completed = false;
    return std::exchange(m_data->completed, {});
}

CombatReplayWorker::QueueState CombatReplayWorker::queueState() const {
    if (!m_data) {
        return {};
    }
    std::lock_guard lock{m_data->mutex};
    return m_data->state;
}

void CombatReplayWorker::stop() {
    if (!m_data) {
        return;
    }
    {
        std::lock_guard lock{m_data->mutex};
        m_data->stopping = true;
        m_data->state.pending = false;
        m_data->ready.notify_one();
    }
    if (m_data->thread.joinable()) {
        m_data->thread.join();
    }
    m_data.reset();
}
} // namespace gdl::game
