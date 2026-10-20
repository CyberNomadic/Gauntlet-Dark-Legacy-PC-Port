#pragma once

#include <array>
#include <memory>
#include <optional>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {

/** In-process states immediately BEFORE their numbered simulation tick. This store
 * knows nothing about packets, asset loading, input prediction or confirmation.
 * The scene assembles its value-state components in the supplied capture callback.
 * Borrowed immutable resources must outlive the retained states; destroy/reset
 * the history on an epoch change before replacing those resources. clear() only
 * invalidates tick tags and keeps the stored values and their buffer capacity.
 *
 * Slots are heap-owned to keep full scene values off the thread stack. Reusing a
 * slot retains its component vector capacities. A slot's exact tick tag prevents
 * ring wrap from returning another tick's state. */
template <class State> class LocalStateHistory {
public:
    static constexpr usize kCapacity = 16;

    LocalStateHistory() : m_slots(std::make_unique<std::array<Slot, kCapacity>>()) {}
    LocalStateHistory(const LocalStateHistory&) = delete;
    LocalStateHistory& operator=(const LocalStateHistory&) = delete;
    ~LocalStateHistory() = default;

    /** Capture returns true only after every component has been copied. A failed
     * or throwing copy cannot leave a partially overwritten state addressable. */
    template <class Capture> bool capture(u64 tick, Capture&& copy) {
        auto& slot = (*m_slots)[tick % kCapacity];
        slot.tick.reset();
        if (!std::forward<Capture>(copy)(slot.state)) {
            return false;
        }
        slot.tick = tick;
        return true;
    }

    /** Valid until this slot is captured again, discarded, or the history clears. */
    const State* find(u64 tick) const {
        const auto& slot = (*m_slots)[tick % kCapacity];
        return slot.tick == tick ? &slot.state : nullptr;
    }

    /** After loading tick N, the old branch's N+1 and later states are obsolete. */
    void discardAfter(u64 tick) {
        for (auto& slot : *m_slots) {
            if (slot.tick && *slot.tick > tick) {
                slot.tick.reset();
            }
        }
    }

    /** Invalidate tags within the same loaded resource lifetime, retaining buffers. */
    void clear() {
        for (auto& slot : *m_slots) {
            slot.tick.reset();
        }
    }

private:
    struct Slot {
        State state{};
        std::optional<u64> tick;
    };
    std::unique_ptr<std::array<Slot, kCapacity>> m_slots;
};

} // namespace gdl::game
