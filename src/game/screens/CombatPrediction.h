#pragma once

#include "game/netplay/PartyBootstrap.h"
#include "game/screens/OrdinaryCombatTick.h"

namespace gdl::game {
struct CombatSnapshot;
struct MotionSnapshot;
class StageParticlePresentation;

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
    /** A catalog lifetime, distinct for each successful load; failed loads and
     * ordinary replay preserve it. Cached decoded values may never cross it. */
    std::weak_ptr<const u8> resourceLifetime() const;

    /** Exact post-step authority, not interpolated presentation. Resource-free
     * state still resolves against trusted native bindings before any commit.
     * Unknown effect/projectile resources refuse the entire seed. */
    bool begin(u64 epoch, u64 tick, const OrdinaryCombatCheckpoint::State& authority,
               OrdinaryCombatCheckpoint::RestoreIssue* issue = nullptr);
    /** The first advance prepares one baseline undo. Later advances retain it,
     * reusing the last exact native postcondition as their admission image,
     * never restoring a child independently or publishing irreversible outputs. */
    OrdinaryCombatTick::Result advance(u64 tick, std::span<const PlayInput> inputs,
                                       const OrdinaryCombatTick::Frame& frame);
    /** Rejecting any step invalidates the complete replay pass, not just its
     * last tick. A caller must buffer outputs until its whole pass commits. */
    void resetReplay();
    bool active() const;
    u64 tick() const;
    /** Changes whenever this private owner is restored, reset or advanced.
     * Pair with resourceLifetime(): equal ticks alone do not prove continuity. */
    u64 replayRevision() const;
    std::span<const PlayerRuntime> players() const;
    const LevelWorld* world() const;
    const Enemies* enemies() const;
    /** Captures the same owner domain as the most recently accepted seed. Before
     * the first seed, preserves the legacy combat-only inspection boundary. */
    std::optional<OrdinaryCombatCheckpoint::State> checkpoint() const;
    /** Explicit inspection of one prepared domain. Requesting an unavailable
     * Stage fails instead of returning a partial combat-only checkpoint. */
    std::optional<OrdinaryCombatCheckpoint::State> checkpoint(bool includeStage) const;
    /** Resolves an opaque wire supplement against this level's load-time native
     * catalog. Failure never mutates the private owners or strips Stage data. */
    std::optional<OrdinaryCombatCheckpoint::Stage> decodeStage(std::span<const u8> bytes) const;
    std::unique_ptr<CombatSnapshot>
    stagePicture(const CombatSnapshot& authority, const MotionSnapshot& motion,
                 std::shared_ptr<const StageParticlePresentation>* particles = nullptr) const;

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};

} // namespace gdl::game
