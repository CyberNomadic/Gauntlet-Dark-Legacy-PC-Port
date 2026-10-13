#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "game/enemies/OpponentState.h"

namespace gdl::game {

/** The native generator, swarm and missile phase, without player damage,
 * rewards, sounds or effect dispatch. Resource owners and the captured world
 * collision must outlive this borrowed view and every prepared restore. */
class OpponentSimulation {
public:
    static constexpr s32 kCritterIds = 3000;
    static constexpr s32 kBossIds = 4000;

    OpponentSimulation(Generators& generators, Enemies& enemies, EnemyMissiles& missiles)
        : m_generators(generators), m_enemies(enemies), m_missiles(missiles) {}

    using Checkpoint = OpponentState;

    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
        friend class OpponentSimulation;
    };

    std::optional<Checkpoint>
    checkpoint(std::span<const EnemyMissiles::Resource> resources = {}) const;
    std::optional<Prepared>
    prepareRestore(const Checkpoint& state,
                   std::span<const EnemyMissiles::Resource> resources = {}) const;
    bool canCommit(const Prepared& prepared) const;
    /** All three preparations are checked before any pool changes. Synchronous,
     * with no callbacks or resource changes between those checks and commits. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const Checkpoint& state, std::span<const EnemyMissiles::Resource> resources = {});

    struct Frame {
        s32 ticks = 1;
        f32 seconds = 1.0f / 60;
        std::span<const EnemyView> players;
        std::span<const Obstacle> fixtures;
        std::span<const MissileStop> stops;
        std::span<const MissileTarget> critters;
        std::span<const MissileTarget> bosses;
        const WorldCollision* collision = nullptr;
        f32 missileSpeed = 1;
        bool timeStopped = false;
    };

    /** Call after native player/fixture motion, before processing opponent
     * outputs. The caller supplies current views, scales and combatant bodies.
     * Generator obstacles are sampled again after births, just as in native play. */
    /** Returns the exact stop roster used by this phase; the native combatant
     * projectile phase adjusts its rock health using the swarm's queued hits. */
    std::vector<MissileStop> advance(const Frame& frame,
                                     const EnemyMissiles::SceneryBlast& scenery = {});

    static std::vector<MissileTarget> targets(const Enemies& enemies,
                                              std::span<const MissileTarget> critters = {},
                                              std::span<const MissileTarget> bosses = {});

    enum class Result { Advanced, Invalid, Unsupported, Interaction };
    /** Private continuation only. A step requiring uncaptured outside gameplay
     * is rejected and all pools return to their preceding values. Generator
     * accounting and harmless presentation queues are never discarded. Private
     * births/shot ids may be reused after correction: do not publish them as
     * authoritative lifetimes. Static collision, view and fixture state are
     * supplied observations, not predictions of the rest of the level. Shooting
     * minds/broods remain authoritative until their initial launch sweep can be
     * bounded; already captured flights continue within a finite work budget. */
    Result speculate(const Frame& frame, std::span<const EnemyMissiles::Resource> resources = {});

private:
    Generators& m_generators;
    Enemies& m_enemies;
    EnemyMissiles& m_missiles;
};

} // namespace gdl::game
