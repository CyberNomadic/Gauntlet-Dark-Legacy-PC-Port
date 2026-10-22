#pragma once

#include <memory>
#include <optional>
#include <span>

#include "game/enemies/OpponentState.h"

namespace gdl::game {
/** Test-only capture and atomic restoration of the native opponent pools. */
class OpponentCheckpoint {
public:
    OpponentCheckpoint(Generators& generators, Enemies& enemies, EnemyMissiles& missiles,
                       PlacedEnemies* placements = nullptr)
        : m_generators(generators), m_enemies(enemies), m_missiles(missiles),
          m_placements(placements) {}

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
        friend class OpponentCheckpoint;
    };

    std::optional<Checkpoint>
    checkpoint(std::span<const EnemyMissiles::Resource> resources = {}) const;
    std::optional<Prepared>
    prepareRestore(const Checkpoint& state,
                   std::span<const EnemyMissiles::Resource> resources = {}) const;
    bool canCommit(const Prepared& prepared) const;
    /** All preparations are checked before any owner changes. Synchronous,
     * with no callbacks or resource changes between those checks and commits. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const Checkpoint& state, std::span<const EnemyMissiles::Resource> resources = {});

private:
    Generators& m_generators;
    Enemies& m_enemies;
    EnemyMissiles& m_missiles;
    PlacedEnemies* m_placements = nullptr;
};
} // namespace gdl::game
