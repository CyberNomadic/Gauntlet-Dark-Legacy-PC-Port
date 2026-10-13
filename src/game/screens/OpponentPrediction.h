#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "game/screens/OpponentSimulation.h"

namespace gdl::game {
class LevelWorld;

/** Private native opponent continuation. Loading owns independent stock models
 * and clocks; replay never mutates replica presentation or dispatches gameplay
 * outputs. The trusted level and its hazard/item resources outlive clear(). */
class OpponentPrediction {
public:
    static constexpr u64 kMaxTicks = 120;
    OpponentPrediction();
    ~OpponentPrediction();
    OpponentPrediction(const OpponentPrediction&) = delete;
    OpponentPrediction& operator=(const OpponentPrediction&) = delete;

    /** Call at stage loading, after the ordinary presentation catalog is frozen.
     * Difficulty and initial party size are trusted room/level configuration,
     * not coefficients taken from an opponent packet. Failed loads preserve the
     * prior owner. No assets are loaded by begin(), advance() or resetReplay(). */
    bool open(RenderDevice& device, const std::filesystem::path& root, LevelWorld& world,
              const Enemies& trustedCatalog, s32 initialPlayers, f32 difficultyGain);
    void clear();
    bool loaded() const;

    /** Start both correction and presentation replay passes from this exact
     * authority boundary. Collision is a caller-owned trusted reconstruction of
     * the same level, copied here; it is never replaced by packet geometry. */
    bool begin(u64 epoch, u64 postStepTick, const OpponentState& authority,
               const WorldCollision& collision);
    /** One common world step AFTER all players have moved. A held phase retains
     * every native clock/queue. Any rejected step rewinds the entire pass to its
     * authority boundary and disables all output getters. */
    OpponentSimulation::Result advance(u64 tick, const OpponentSimulation::Frame& frame,
                                       bool held = false);
    void resetReplay();

    bool active() const;
    u64 epoch() const;
    u64 tick() const;
    const Enemies* enemies() const;
    const Generators* generators() const;
    const EnemyMissiles* missiles() const;
    const WorldCollision* collision() const;

    struct KnownEnemy {
        u32 slot = 0;
        Enemies::Checkpoint::Body state;
    };
    /** Only lifetimes present at the authority boundary may override a host
     * picture. Private births still affect private native collision, but their
     * reusable speculative IDs are never exported as authoritative actors. */
    std::vector<KnownEnemy> knownEnemies() const;
    /** Diagnostic value capture, including after a rejected pass. Does not
     * consume queues; callers must not publish it as new authority. */
    std::optional<OpponentState> checkpoint() const;

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};

} // namespace gdl::game
