#pragma once

#include "game/netplay/CombatPlayback.h"
#include "game/netplay/RollbackInputHistory.h"
#include "game/screens/CombatPrediction.h"

namespace gdl::game {
/** Whole-party native continuation from a completed host checkpoint. Local
 * input is retained verbatim; unknown remote ticks use the same bounded held
 * input policy as the host, never repeated button edges. This lane replaces
 * independent local movement only when every native step commits. Unsupported
 * interactions discard the entire pass and leave the existing fallback usable.
 * Predicted outputs never write saves or dispatch sound/rumble/reward events. */
class OnlineCombatReplay {
public:
    bool begin(const MatchContext& context, u8 localPeer, CombatPrediction& prediction);
    void clear();
    bool update(const CombatSnapshot* authority, std::span<const InputCommand> sent);
    bool active() const;
    bool present(CombatSnapshot& shown, EnemyPresentation& presentation, f32 blend) const;
    u64 tick() const;

private:
    void discard();
    bool updatePass(const CombatSnapshot* authority, std::span<const InputCommand> sent);
    std::unique_ptr<CombatSnapshot> picture() const;
    MatchContext m_context;
    u8 m_local = 0;
    CombatPrediction* m_prediction = nullptr;
    RollbackInputHistory m_history;
    // Native enemy pools make a full snapshot large even when its optional
    // supplement is absent. Keep retained snapshots off session/caller stacks.
    std::unique_ptr<CombatSnapshot> m_authority;
    std::unique_ptr<CombatSnapshot> m_previous;
    std::unique_ptr<CombatSnapshot> m_shown;
    u64 m_clock = 0;
    u64 m_receivedAt = 0;
};
} // namespace gdl::game
