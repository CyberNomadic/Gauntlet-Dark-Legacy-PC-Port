#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "game/players/LevelWatch.h"
#include "game/screens/PlayerAttacksCheckpoint.h"
#include "game/screens/PlayerHealth.h"
#include "game/screens/PlayerRuntimeCheckpoint.h"
#include "game/world/PlayerArsenalCheckpoint.h"

namespace gdl::game {

/** One local restore transaction for the supplied player/combat owners, not a
 * complete scene checkpoint or permission to predict world interactions. The
 * opponent adapter, fixtures, camera, and external audio/output ledgers remain
 * separate participants of any enclosing simulation transaction. */
class CombatRollbackCheckpoint {
public:
    struct Resources {
        std::span<const PlayerMissiles::Resource> missiles;
        std::span<const EffectTrees::PoolResource> missileEffects;
        std::span<const EffectTrees::PoolResource> sharedEffects;
    };
    struct State {
        std::vector<PlayerRuntimeCheckpoint::State> players;
        PlayerHealth::Checkpoint health;
        LevelWatch::Checkpoint levels;
        PlayerAttacksCheckpoint::State attacks;
        PlayerArsenalCheckpoint::State arsenal;
        EffectTrees::PoolCheckpoint effects;
        bool valid() const;
        bool operator==(const State&) const = default;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class CombatRollbackCheckpoint;
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
        friend class CombatRollbackCheckpoint;
        explicit Rewind(Prepared&& state);
        Prepared m_state;
    };

    /** All owners and immutable artwork bindings outlive this object and its
     * preparations. Runtime order is native party order, not sorted input IDs.
     * Resource records are copied, but their artwork/lender pointers are borrowed.
     * Resource reload must clear the corresponding owners before replacement. */
    CombatRollbackCheckpoint(std::span<PlayerRuntimeCheckpoint* const> players,
                             PlayerHealth& health, LevelWatch& levels, PlayerAttacks& attacks,
                             PlayerArsenal& arsenal, EffectTrees& effects,
                             const Resources& resources);
    ~CombatRollbackCheckpoint();
    CombatRollbackCheckpoint(const CombatRollbackCheckpoint&) = delete;
    CombatRollbackCheckpoint& operator=(const CombatRollbackCheckpoint&) = delete;
    CombatRollbackCheckpoint(CombatRollbackCheckpoint&&) = delete;
    CombatRollbackCheckpoint& operator=(CombatRollbackCheckpoint&&) = delete;

    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Prepare/preflight/commit synchronously, with no intervening native update,
     * owner destruction, or resource teardown. Refusal changes nothing. Once all
     * owners pass preflight, commit allocates nothing and invokes no callbacks. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);
    /** Preallocates an undo of the current native state. Unlike Prepared, this
     * permits intervening owned gameplay updates, including pain RNG and level
     * marks. Resource rebinds, child restores, unsupported external owners, and
     * owner replacement still invalidate it. The caller buffers all outputs and
     * refuses uncaptured interactions BEFORE dispatching them. */
    std::optional<Rewind> prepareRewind() const;
    bool canRewind(const Rewind& rewind) const;
    /** Joint preflight, then allocation-free/callback-free single-use undo. */
    bool rewind(Rewind&& rewind);

private:
    friend class OrdinaryCombatCheckpoint;
    friend class OrdinaryCombatTick;
    bool bindingsValid() const;
    bool canCommit(const Prepared& prepared, bool unchangedValues) const;
    void commitReady(Prepared&& prepared);
    bool binds(const LevelWorld& world, const ItemArchive& weapons, const EffectTrees& effects,
               const RenderDevice& device) const;
    std::vector<PlayerRuntimeCheckpoint*> m_players;
    PlayerHealth& m_health;
    LevelWatch& m_levels;
    PlayerAttacks& m_attacks;
    PlayerArsenal& m_arsenal;
    EffectTrees& m_effects;
    std::vector<PlayerMissiles::Resource> m_missiles;
    std::vector<EffectTrees::PoolResource> m_missileEffects;
    std::vector<EffectTrees::PoolResource> m_sharedEffects;
    bool m_configurationValid = false;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
    u64 m_generation = 1;
};

} // namespace gdl::game
