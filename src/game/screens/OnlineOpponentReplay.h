#pragma once

#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/OpponentPrediction.h"

namespace gdl::game {
/** Couples local player steps to independent native ordinary-opponent pools.
 * Remote players and outside fixtures are held observations, corrected by the
 * next complete host checkpoint. This is a bounded continuation, not authority
 * over hits, births, inventory, world triggers or any one-shot side effect. */
class OnlineOpponentReplay {
public:
    void begin(const MatchContext& context, u8 localPeer, OpponentPrediction& prediction,
               const WorldScene& scene, const WorldLayout& layout, const WorldCollision& collision);
    void clear();
    /** Empty callbacks preserve the existing independent movement path when no
     * complete compatible seed exists. Once a coupled pass begins, a rejected
     * step invalidates both opponent and local actor predictions atomically. */
    LocalMovementPrediction::ReplayCallbacks callbacks(const CombatSnapshot* authority,
                                                       std::span<const InputCommand> sent = {});
    void present(CombatSnapshot& shown, EnemyPresentation& presentation, f32 blend) const;
    bool active() const;

private:
    bool eligible(const CombatSnapshot& authority) const;
    bool restore(const CombatSnapshot& authority);
    bool before(const LocalMovementPrediction::ReplayTick& tick,
                LocalMovementPrediction::ContactOverlay& contacts);
    bool after(const LocalMovementPrediction::ReplayTick& tick);
    void finish(bool committed);
    bool stationaryGeometry() const;

    MatchContext m_context;
    u8 m_local = 0;
    OpponentPrediction* m_prediction = nullptr;
    const WorldLayout* m_layout = nullptr;
    std::unique_ptr<WorldScene> m_scene;
    WorldCollision m_collision;
    WorldMotion m_worldMotion;
    std::optional<OpponentFrame> m_frame;
    std::array<std::optional<PlayerLocomotionState>, InputCommand::kSeats> m_contacts;
    std::optional<WorldMotion::State> m_worldState;
    std::vector<Mat4> m_transforms;
    std::vector<EnemyCombatState> m_shown;
    std::vector<EnemyCombatState> m_previous;
    std::array<u64, Enemies::kMost> m_instances{};
    u64 m_baseline = 0;
    u64 m_end = 0;
    std::array<u64, InputCommand::kSeats> m_latestInput{};
    u64 m_independentThrough = 0;
    bool m_committed = false;
};
} // namespace gdl::game
