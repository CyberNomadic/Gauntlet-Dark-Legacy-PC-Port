#pragma once

#include "game/netplay/PartyBootstrap.h"
#include "game/screens/OrdinaryCombatTick.h"

namespace gdl::game {

/** Private native party and combat owners, separate from replica artwork and
 * persistent saves. Load only from the authenticated stage/party configuration.
 * Restoring or advancing never loads assets, plays audio, or dispatches events.
 * This owner does not authorize the interactions excluded by OrdinaryCombatTick. */
class CombatPrediction {
public:
    static constexpr u64 kMaxTicks = 120;
    CombatPrediction();
    ~CombatPrediction();
    CombatPrediction(const CombatPrediction&) = delete;
    CombatPrediction& operator=(const CombatPrediction&) = delete;

    /** All mutable level services and player figures are independently owned.
     * The render device outlives clear(). A failed load preserves the old stage. */
    bool open(RenderDevice& device, const std::filesystem::path& root, const LevelRef& level,
              const MatchContext& context, const PartyBootstrap::Party& party, f32 difficultyGain);
    bool resume(const MatchContext& context);
    void clear();
    bool loaded() const;

    /** Exact post-step authority, not interpolated presentation. Resource-free
     * state still resolves against trusted native bindings before any commit.
     * Unknown effect/projectile resources refuse the entire seed. */
    bool begin(u64 epoch, u64 tick, const OrdinaryCombatCheckpoint::State& authority);
    OrdinaryCombatTick::Result advance(u64 tick, std::span<const PlayInput> inputs,
                                       const OrdinaryCombatTick::Frame& frame);
    /** Rejecting any step invalidates the complete replay pass, not just its
     * last tick. A caller must buffer outputs until its whole pass commits. */
    void resetReplay();
    bool active() const;
    u64 tick() const;
    std::span<const PlayerRuntime> players() const;
    const LevelWorld* world() const;
    const Enemies* enemies() const;
    std::optional<OrdinaryCombatCheckpoint::State> checkpoint() const;

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};

} // namespace gdl::game
