#pragma once

#include "game/screens/MatchInputs.h"
#include "game/screens/PlayScene.h"

namespace gdl::game {
/** The same fixed-step scene driver on every peer. It consumes complete agreed
 * input frames, never predicted controls or replicated scene state. Scene and
 * match loading/teardown remain the caller's responsibility.
 *
 * Sample once per application tick, flush the session, then advance. Splitting
 * these operations lets inputs leave before expensive native gameplay work.
 * A network stall holds gameplay clocks, not the UI or transport pump. */
class NativePlayDriver {
public:
    enum class Result : u8 { Held, Advanced, Travel, GameOver, Leave, Failed };
    bool bind(PlayScene& scene, const MatchSession& match);
    void clear();
    /** Latest held controls plus accumulated one-shot edges until the next
     * sampled frame. Local Sumner clicks become owner/revision-checked semantic
     * choices; no local pointer/text/settings state enters the shared scene. */
    bool sample(MatchSession& match, const SessionInputs::Frame& local);
    /** A native dead-player Quit ends this fixed-roster session cleanly after
     * acknowledging its executed frame. It does not remove transport members
     * or reinterpret Quit as Wait In Tower. Terminal scenes never advance again. */
    Result advance(MatchSession& match);
    bool failed() const { return m_failed; }
    u64 completedTick() const { return m_nextTick == 0 ? 0 : m_nextTick - 1; }

private:
    bool agrees(const MatchSession& match) const;
    bool rosterMatches(bool allowDeparted = false) const;
    Result fail();
    bool complete(MatchSession& match);
    void filterArrivalSkip(SessionInputs::Frame& frame);
    PlayScene* m_scene = nullptr;
    MatchContext m_context;
    std::vector<InputCommand> m_pending;
    std::array<bool, InputCommand::kSeats> m_arrivalVotes{};
    u64 m_nextTick = 0;
    bool m_finished = false;
    bool m_failed = false;
    PlayOutcome m_outcome = PlayOutcome::Running;
};
} // namespace gdl::game
