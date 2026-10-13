#pragma once

#include "game/screens/CombatRollbackCheckpoint.h"
#include "game/screens/OpponentCombatCheckpoint.h"
#include "game/screens/OpponentSimulation.h"

namespace gdl::game {

/** Atomic value boundary for the ordinary combat owners in one bound level.
 * Includes all ordered party members, attack pools, ordinary opponents, native
 * pending damage/rewards/lessons, and their one shared effect pool. Fixtures,
 * world motion, pickups, audio, and scene/application outputs remain outside:
 * this is not a whole-scene checkpoint or permission for network prediction. */
class OrdinaryCombatCheckpoint {
public:
    struct State {
        CombatRollbackCheckpoint::State combat;
        OpponentState opponents;
        OpponentCombatCheckpoint::State adapter;
        bool valid() const;
        bool operator==(const State&) const = default;
    };
    /** Validate split transport supplements without copying the native pools. */
    static bool valid(const CombatRollbackCheckpoint::State& combat, const OpponentState& opponents,
                      const OpponentCombatCheckpoint::State& adapter);
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class OrdinaryCombatCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    class Rewind {
    public:
        Rewind(Rewind&&) noexcept;
        Rewind& operator=(Rewind&&) noexcept;
        ~Rewind();
        Rewind(const Rewind&) = delete;
        Rewind& operator=(const Rewind&) = delete;

    private:
        friend class OrdinaryCombatCheckpoint;
        struct Data;
        explicit Rewind(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    /** Borrowed owners/artwork must outlive the wrapper and its preparations.
     * The population and adapter views are constructed here from exactly the
     * same LevelOpponents owner; callers cannot accidentally pair two levels.
     * Resource records are copied, not their immutable model data. */
    OrdinaryCombatCheckpoint(CombatRollbackCheckpoint& combat, LevelOpponents& opponents,
                             std::span<const EnemyMissiles::Resource> missiles = {});
    ~OrdinaryCombatCheckpoint();
    OrdinaryCombatCheckpoint(const OrdinaryCombatCheckpoint&) = delete;
    OrdinaryCombatCheckpoint& operator=(const OrdinaryCombatCheckpoint&) = delete;
    OrdinaryCombatCheckpoint(OrdinaryCombatCheckpoint&&) = delete;
    OrdinaryCombatCheckpoint& operator=(OrdinaryCombatCheckpoint&&) = delete;

    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** All child plans preflight before any live owner changes. The synchronous
     * commit allocates nothing and emits no callbacks. No native updates, owner
     * destruction or resource rebind may interleave prepare/preflight/commit. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);
    /** Preallocates exact current-state undo across an ordinary native tick,
     * including placement watch/activation, pain RNG and progression changes.
     * Outputs remain caller-owned and buffered; unsupported external owners
     * must be refused before mutation. No nested restore or resource change
     * may occur between preparation and rewind. */
    std::optional<Rewind> prepareRewind() const;
    bool canRewind(const Rewind& rewind) const;
    bool rewind(Rewind&& rewind);

private:
    friend class OrdinaryCombatTick;
    bool bindingsValid() const;
    CombatRollbackCheckpoint& m_combat;
    LevelOpponents& m_opponents;
    OpponentSimulation m_pools;
    OpponentCombatCheckpoint m_adapter;
    std::vector<EnemyMissiles::Resource> m_missiles;
    bool m_configurationValid = false;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
    u64 m_generation = 1;
};

} // namespace gdl::game
