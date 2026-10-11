#pragma once

#include <array>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/config/ControlProfiles.h"

namespace gdl::game {

/** A host-authored cue for one room seat. Device selection, capabilities and motor
 * strengths remain local: the ordinary GameContext callback resolves them. */
struct FeedbackEvent {
    u8 seat = 0;
    s32 frames = 0;
    ControlFeedback feedback = ControlFeedback::Damage;
    bool operator==(const FeedbackEvent&) const = default;
};

/** Small reliable feedback stream, independent of simulation snapshots. Only the
 * host emits cues; a guest can change only its own seats' feedback gates.
 * Each gate change advances a generation, so even in-flight cues from before a
 * local pause cannot replay after resume. Fresh match epochs reset everything.
 * Feedback is ephemeral: congestion drops the oldest not-yet-sent/consumed cue,
 * never grows a backlog or stalls gameplay. Gate updates coalesce per seat. */
class FeedbackSync {
public:
    enum class Admission : u8 { Accepted, Stale, WrongPeer, Invalid };
    struct Delivery {
        u8 peer = 0;
        std::vector<u8> bytes;
    };
    static constexpr usize kBudget = 64;
    static constexpr usize kPacketBytes = 40;
    static bool recognizes(std::span<const u8> bytes);
    void begin(u64 epoch, u8 localPeer, const std::array<u8, 4>& owners,
               const std::array<u32, 4>& grants);
    void clear();
    /** Host only. Muted cues are successfully discarded; false means invalid
     * arguments, no active epoch, a guest caller, or exhausted sequence space. */
    bool emit(s32 seat, s32 frames, ControlFeedback feedback);
    /** Local seats only; the caller also stops the physical motor immediately. */
    bool setEnabled(s32 seat, bool enabled);
    Admission receive(u8 peer, std::span<const u8> bytes);
    std::optional<FeedbackEvent> take();
    const std::deque<Delivery>& outgoing() const { return m_outgoing; }
    void sent();

private:
    void discardSeat(u8 seat);
    void retain(const FeedbackEvent& event);
    u64 m_epoch = 0;
    u8 m_local = 0;
    std::array<u8, 4> m_owners{};
    std::array<u32, 4> m_grants{};
    std::array<u64, 4> m_generations{};
    std::array<u64, 4> m_sequences{};
    std::array<bool, 4> m_enabled{};
    std::deque<FeedbackEvent> m_pending;
    std::deque<Delivery> m_outgoing;
};

} // namespace gdl::game
