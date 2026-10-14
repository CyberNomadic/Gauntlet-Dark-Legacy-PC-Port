#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "game/enemies/CombatantCheckpoint.h"
#include "game/world/CombatantProjectiles.h"

namespace gdl::game {
/** Complete local continuation of launched combatant attacks, including attacks
 * whose originating actor has already been recycled. Immutable stocks and effect
 * prototypes must outlive this owner. This is not speculative-combat admission.
 * In particular, native SFXX creation still needs a separately prepared emission
 * path before it may execute inside rollback without resource uploads. */
class CombatantProjectilesCheckpoint {
public:
    using Resource = CombatantCheckpoint::Resource;
    using Definition = CombatantCheckpoint::Definition;
    using Shot = CombatantCheckpoint::Shot;
    static constexpr usize kMaxResources = 64;
    static constexpr usize kMaxFlights = 4096;
    static constexpr usize kMaxOutputs = 16384;

    struct Catalog {
        u32 id = 0;
        CombatantKind kind = CombatantKind::Unknown;
        std::string name;
        std::vector<std::string> children;
        bool operator==(const Catalog&) const = default;
    };
    struct Flight {
        Shot shot;
        u32 archive = 0; ///< Trusted stock ID, independent of the originating slot.
        Vec3 position{0}, velocity{0}, spin{0}, rotation{0};
        u32 effect = 0;
        bool morphed = false, stuck = false, planted = false;
        f32 phaseSeconds = 0;
        bool settled = false;
        f32 impactRadius = 0;
        std::optional<CritterArea> impactArea;
        bool leavesGenerator = false, summonsEnemies = false;
        f32 contactSeconds = 0;
        s32 piercedPlayer = -1;
        bool reflected = false;
        bool valid() const;
        bool operator==(const Flight&) const;
    };
    struct State {
        std::vector<Catalog> catalog;
        std::vector<Flight> flying;
        std::vector<u32> emittedEffects;
        std::vector<CombatantProjectileHit> hits;
        std::vector<CombatantWorldHit> worldHits;
        std::vector<RockHit> rockHits;
        std::vector<Mat4> generators, summons;
        /** Portable engine words and cursor; native launch consumes one high-24
         * binary32 draw for spread and two more only for spinning artwork. */
        Mt19937::Checkpoint random = Mt19937{}.checkpoint();
        f32 ricochetIn = 0;
        bool valid() const;
        bool operator==(const State&) const;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class CombatantProjectilesCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data>);
        std::unique_ptr<Data> m_data;
    };

    CombatantProjectilesCheckpoint(CombatantProjectiles&, EffectTrees&, std::span<const Resource>,
                                   std::span<const EffectTrees::PoolResource>);
    ~CombatantProjectilesCheckpoint();
    CombatantProjectilesCheckpoint(const CombatantProjectilesCheckpoint&) = delete;
    CombatantProjectilesCheckpoint& operator=(const CombatantProjectilesCheckpoint&) = delete;
    CombatantProjectilesCheckpoint(CombatantProjectilesCheckpoint&&) = delete;
    CombatantProjectilesCheckpoint& operator=(CombatantProjectilesCheckpoint&&) = delete;

    /** Refuses unregistered live/trailing effect topology; never silently drops
     * particles, links, queues or an effect whose shooter no longer exists. */
    std::optional<State> checkpoint() const;
    /** Aggregate capture reuses its already captured shared pool instead of
     * cloning that potentially large particle state a second time. */
    std::optional<State> checkpoint(const EffectTrees::PoolCheckpoint& effects) const;
    /** The enclosing transaction must prepare/install this exact shared effect
     * checkpoint once. This leaf validates its topology and handle references but
     * deliberately does not restore, stop or otherwise mutate that shared pool. */
    std::optional<Prepared> prepareRestore(const State&, const EffectTrees::PoolCheckpoint&) const;
    bool canCommit(const Prepared&) const;
    bool commitRestore(Prepared&&);
    /** Leaf-only convenience: the caller still installs the supplied shared
     * effect state exactly once as part of the same preflighted transaction. */
    bool restore(const State&, const EffectTrees::PoolCheckpoint&);

private:
    bool bound() const;
    const Resource* resource(u32) const;
    std::optional<Definition> definition(const CritterData*) const;
    const CritterData* definition(Definition) const;
    bool references(const State&, const EffectTrees::PoolCheckpoint&) const;
    CombatantProjectiles& m_owner;
    EffectTrees& m_effects;
    std::vector<Resource> m_resources;
    std::vector<Catalog> m_catalog;
    std::vector<std::shared_ptr<const u8>> m_resourceBindings;
    std::vector<const CritterData*> m_children;
    std::vector<EffectTrees::PoolResource> m_effectResources;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
