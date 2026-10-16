#pragma once

#include <algorithm>
#include <compare>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

/** Stable native identity, never an address or a compacted pool index. The
 * namespace distinguishes owners; generation distinguishes reused entity IDs. */
struct FrameEventEmitter {
    static constexpr u8 kNoSeat = 4;
    u32 nameSpace = 0;
    u64 entity = 0;
    u64 generation = 0;
    u8 seat = kNoSeat;
    bool valid() const;
    auto operator<=>(const FrameEventEmitter&) const = default;
    template <class D> friend void visitNativeState(D& digest, const FrameEventEmitter& value) {
        digest.template fields<4>(value);
    }
};

struct FrameEventIdentity {
    u64 epoch = 0;
    u64 tick = 0; ///< completed native tick, not a render frame or input arrival
    FrameEventEmitter emitter;
    u32 cue = 0;
    u32 occurrence = 0; ///< emitter-local deterministic occurrence within this tick
    bool valid() const;
    auto operator<=>(const FrameEventIdentity&) const = default;
    template <class D> friend void visitNativeState(D& digest, const FrameEventIdentity& value) {
        digest.template fields<5>(value);
    }
};

/** A desired loop survives later tick publications. Change instance for a new
 * intentional lifetime; a repeated start of this key updates its parameters. */
struct FrameLoopKey {
    FrameEventEmitter emitter;
    u32 channel = 0;
    u32 instance = 0;
    bool valid() const;
    auto operator<=>(const FrameLoopKey&) const = default;
    template <class D> friend void visitNativeState(D& digest, const FrameLoopKey& value) {
        digest.template fields<3>(value);
    }
};

/** In-memory presentation boundary, with no simulation, transport or resources.
 * Payload is an owned, copyable, equality-comparable value. It may describe a
 * sound/rumble or a confirmed save intent; it must not hold mutable borrowed
 * state. A digest alone is not payload equality.
 *
 * Submit complete tick journals, including empty ticks. A correction replaces
 * the entire suffix through the existing head, so loop retractions are evaluated
 * only after replay, never between its intermediate ticks. One-shots already
 * heard cannot be undone: their removal/change is reported, not replayed.
 *
 * Confirmation MUST be the contiguous actual-input frontier of EVERY occupied
 * seat, computed by the session. This class cannot infer it from received packet
 * maxima or the simulation head. Epoch changes cancel loops and clear history.
 * A desired finite audio loop's repeat policy belongs to Payload/the dispatcher,
 * not this clock. No callbacks run here: the caller dispatches each successful result once.
 * Dispatch failure requires a caller-owned retry queue, not another confirm(). */
template <class Payload> class FrameEventLedger {
public:
    static constexpr usize kHistory = 16;
    static constexpr usize kEventsPerTick = 256;
    static constexpr usize kPresentedPerTick = 1024;
    static constexpr usize kActiveLoops = 256;
    enum class Policy : u8 { OneShot, LoopStart, LoopStop, Confirmed };
    struct Event {
        FrameEventIdentity id;
        Policy policy = Policy::OneShot;
        Payload payload;
        std::optional<FrameLoopKey> loop;
        bool operator==(const Event&) const = default;
    };
    struct Journal {
        std::vector<Event> events;
        bool operator==(const Journal&) const = default;
    };
    enum class ActionKind : u8 { Present, StartLoop, UpdateLoop, StopLoop, Commit };
    struct Action {
        ActionKind kind;
        Event event;
    };
    enum class Error : u8 {
        None,
        Closed,
        WrongEpoch,
        RepeatedEpoch,
        InvalidRange,
        Gap,
        TooOld,
        IncompleteReplay,
        InvalidEvent,
        DuplicateIdentity,
        ConflictingIdentity,
        ConfirmedConflict,
        HistoryFull,
        EventLimit,
        ConfirmationRegression,
        UnsimulatedConfirmation
    };
    struct Result {
        Error error = Error::None;
        std::vector<Action> actions;
        std::vector<FrameEventIdentity> unretractable;
        explicit operator bool() const { return error == Error::None; }
    };

    Result reset(u64 epoch, u64 firstTick = 1) {
        if (epoch == 0) {
            return failure(Error::WrongEpoch);
        }
        if (firstTick == 0) {
            return failure(Error::InvalidRange);
        }
        if (epoch == m_epoch) {
            return failure(Error::RepeatedEpoch);
        }
        Result result;
        for (const auto& [key, event] : m_active) {
            result.actions.push_back({ActionKind::StopLoop, event});
        }
        m_history.clear();
        m_baseLoops.clear();
        m_active.clear();
        m_epoch = epoch;
        m_firstTick = firstTick;
        m_head.reset();
        m_confirmed.reset();
        return result;
    }

    /** All validation and output allocation precedes mutation. Invalid journals
     * leave both the retained branch and pending presentation unchanged. */
    Result replace(u64 epoch, u64 firstTick, std::span<const Journal> journals) {
        return replaceImpl(epoch, firstTick, journals, std::nullopt);
    }

    /** Replace a corrected suffix whose terminal tick may precede the old head.
     * newHead is the LAST completed tick. An empty suffix can abandon everything
     * from firstTick, with newHead == firstTick - 1. Confirmed ticks cannot be
     * shortened. Discarded journals lose their intents, but retain already-heard
     * one-shot tombstones: a later correction can remove this tentative terminal
     * and simulate those ticks again without repeating their sounds/rumble. */
    Result replaceSuffix(u64 epoch, u64 firstTick, std::span<const Journal> journals,
                         u64 newHead) {
        return replaceImpl(epoch, firstTick, journals, newHead);
    }

private:
    Result replaceImpl(u64 epoch, u64 firstTick, std::span<const Journal> journals,
                       std::optional<u64> newHead) {
        if (const auto error = checkEpoch(epoch); error != Error::None) {
            return failure(error);
        }
        if (journals.size() > kHistory ||
            (journals.empty() && (!newHead || firstTick == 0)) ||
            (!journals.empty() &&
             firstTick > std::numeric_limits<u64>::max() - (journals.size() - 1))) {
            return failure(Error::InvalidRange);
        }
        const u64 lastTick = journals.empty() ? firstTick - 1 : firstTick + journals.size() - 1;
        if (newHead && *newHead != lastTick) {
            return failure(Error::InvalidRange);
        }
        if (!m_head) {
            if (firstTick != m_firstTick) {
                return failure(Error::Gap);
            }
            if (journals.empty()) {
                return failure(Error::InvalidRange);
            }
        } else {
            if (firstTick < m_history.begin()->first) {
                return failure(Error::TooOld);
            }
            if (firstTick > *m_head && firstTick - *m_head != 1) {
                return failure(Error::Gap);
            }
            if (lastTick < *m_head && !newHead) {
                return failure(Error::IncompleteReplay);
            }
            if (lastTick < *m_head && m_confirmed &&
                (lastTick < *m_confirmed || firstTick <= *m_confirmed)) {
                return failure(Error::ConfirmedConflict);
            }
        }
        auto history = m_history;
        auto base = m_baseLoops;
        Result result;
        // Keep these records bounded by the same history cap. Their empty
        // journals no longer affect loops or confirmation; presented identities
        // remain until ordinary confirmed-prefix retirement (or epoch teardown).
        for (auto row = history.upper_bound(lastTick); row != history.end(); ++row) {
            for (const auto& old : row->second.journal.events) {
                if (old.policy == Policy::OneShot) {
                    result.unretractable.push_back(old.id);
                }
            }
            row->second.journal.events.clear();
        }
        for (usize index = 0; index < journals.size(); ++index) {
            const u64 tick = firstTick + index;
            const Journal& journal = journals[index];
            if (journal.events.size() > kEventsPerTick) {
                return failure(Error::EventLimit);
            }
            std::set<FrameEventIdentity> identities;
            for (const auto& event : journal.events) {
                if (!valid(event, epoch, tick)) {
                    return failure(Error::InvalidEvent);
                }
                if (!identities.insert(event.id).second) {
                    return failure(Error::DuplicateIdentity);
                }
            }
            auto& record = history[tick];
            if (m_confirmed && tick <= *m_confirmed && record.journal != journal) {
                return failure(Error::ConfirmedConflict);
            }
            for (const auto& old : record.journal.events) {
                const auto found = find(journal.events, old.id);
                if (found != journal.events.end() && found->policy != old.policy) {
                    return failure(Error::ConflictingIdentity);
                }
                if (old.policy == Policy::OneShot &&
                    (found == journal.events.end() || found->payload != old.payload)) {
                    result.unretractable.push_back(old.id);
                }
            }
            for (const auto& event : journal.events) {
                const auto seen = find(record.presented, event.id);
                if (seen != record.presented.end() && event.policy != Policy::OneShot) {
                    return failure(Error::ConflictingIdentity);
                }
                const auto previous = find(record.journal.events, event.id);
                if (seen != record.presented.end() && seen->payload != event.payload &&
                    (previous == record.journal.events.end() ||
                     previous->payload != event.payload) &&
                    std::ranges::find(result.unretractable, event.id) ==
                        result.unretractable.end()) {
                    result.unretractable.push_back(event.id);
                }
                if (event.policy == Policy::OneShot && seen == record.presented.end()) {
                    if (record.presented.size() == kPresentedPerTick) {
                        return failure(Error::EventLimit);
                    }
                    record.presented.push_back(event);
                    result.actions.push_back({ActionKind::Present, event});
                }
            }
            record.journal = journal;
        }
        while (history.size() > kHistory) {
            const auto oldest = history.begin();
            if (!m_confirmed || oldest->first > *m_confirmed) {
                return failure(Error::HistoryFull);
            }
            applyLoops(base, oldest->second.journal);
            history.erase(oldest);
        }
        auto active = base;
        for (const auto& [tick, record] : history) {
            applyLoops(active, record.journal);
        }
        if (active.size() > kActiveLoops || base.size() > kActiveLoops) {
            return failure(Error::EventLimit);
        }
        for (const auto& [key, old] : m_active) {
            if (!active.contains(key)) {
                result.actions.push_back({ActionKind::StopLoop, old});
            }
        }
        for (const auto& [key, event] : active) {
            const auto old = m_active.find(key);
            if (old == m_active.end()) {
                result.actions.push_back({ActionKind::StartLoop, event});
            } else if (old->second.payload != event.payload) {
                result.actions.push_back({ActionKind::UpdateLoop, event});
            }
        }
        m_history.swap(history);
        m_baseLoops.swap(base);
        m_active.swap(active);
        m_head = lastTick < m_firstTick ? std::nullopt : std::optional{lastTick};
        return result;
    }

public:
    /** Call only once at least firstTick is confirmed. */
    Result confirm(u64 epoch, u64 allSeatsThrough) {
        if (const auto error = checkEpoch(epoch); error != Error::None) {
            return failure(error);
        }
        if (m_confirmed && allSeatsThrough < *m_confirmed) {
            return failure(Error::ConfirmationRegression);
        }
        if (!m_head || allSeatsThrough > *m_head || allSeatsThrough < m_firstTick) {
            return failure(Error::UnsimulatedConfirmation);
        }
        Result result;
        for (const auto& [tick, record] : m_history) {
            if ((!m_confirmed || tick > *m_confirmed) && tick <= allSeatsThrough) {
                for (const auto& event : record.journal.events) {
                    if (event.policy == Policy::Confirmed) {
                        result.actions.push_back({ActionKind::Commit, event});
                    }
                }
            }
        }
        m_confirmed = allSeatsThrough;
        return result;
    }

    u64 epoch() const { return m_epoch; }
    std::optional<u64> head() const { return m_head; }
    std::optional<u64> confirmed() const { return m_confirmed; }
    usize retainedTicks() const { return m_history.size(); }
    usize activeLoops() const { return m_active.size(); }

private:
    struct Record {
        Journal journal;
        std::vector<Event> presented; ///< includes withdrawn one-shots until history retires
    };
    using Loops = std::map<FrameLoopKey, Event>;
    static Result failure(Error error) { return {error, {}, {}}; }
    Error checkEpoch(u64 epoch) const {
        return m_epoch == 0 ? Error::Closed : epoch != m_epoch ? Error::WrongEpoch : Error::None;
    }
    static typename std::vector<Event>::const_iterator find(const std::vector<Event>& events,
                                                            const FrameEventIdentity& identity) {
        return std::ranges::find(events, identity, &Event::id);
    }
    static bool valid(const Event& event, u64 epoch, u64 tick) {
        if (!event.id.valid() || event.id.epoch != epoch || event.id.tick != tick) {
            return false;
        }
        switch (event.policy) {
        case Policy::OneShot:
        case Policy::Confirmed: return !event.loop;
        case Policy::LoopStart:
        case Policy::LoopStop:
            return event.loop && event.loop->valid() && event.loop->emitter == event.id.emitter;
        }
        return false;
    }
    static void applyLoops(Loops& loops, const Journal& journal) {
        for (const auto& event : journal.events) {
            if (event.policy == Policy::LoopStart) {
                loops.insert_or_assign(*event.loop, event);
            } else if (event.policy == Policy::LoopStop) {
                loops.erase(*event.loop);
            }
        }
    }

    u64 m_epoch = 0;
    u64 m_firstTick = 1;
    std::optional<u64> m_head;
    std::optional<u64> m_confirmed;
    std::map<u64, Record> m_history;
    Loops m_baseLoops;
    Loops m_active;
};
} // namespace gdl::game
