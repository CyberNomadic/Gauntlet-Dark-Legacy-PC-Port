#pragma once

#include <array>
#include <string_view>

#include "game/screens/CombatRollbackCheckpoint.h"
#include "game/screens/LevelFixturesCheckpoint.h"
#include "game/screens/OpponentCombatCheckpoint.h"
#include "game/screens/OpponentSimulation.h"
#include "game/world/ExitPortalsCheckpoint.h"
#include "game/world/LevelTransportersCheckpoint.h"
#include "game/world/LevelWorldCheckpoint.h"
#include "game/world/TowerCameraCheckpoint.h"

namespace gdl::game {

/** Atomic value boundary for the ordinary combat owners in one bound level.
 * Includes all ordered party members, attack pools, ordinary opponents, native
 * pending damage/rewards/lessons, and their one shared effect pool. Optional
 * jointly bound stage owners add fixtures, world motion, triggers and pickups.
 * Camera, portal and transporter-pad continuations can join that boundary. Audio and application
 * outputs remain outside: this is not a whole-scene checkpoint or permission for unrestricted
 * network prediction.
 */
class OrdinaryCombatCheckpoint {
public:
    /** First rejected value/resource owner; diagnostic only, never relaxes admission. */
    enum class RestoreIssue : usize {
        None,
        Resources,
        Context,
        State,
        Bindings,
        Supports,
        Combat,
        Pools,
        Adapter,
        World,
        Fixtures,
        Camera,
        Portals,
        Transporters,
        Commit,
        Count
    };
    static constexpr std::array<std::string_view, static_cast<usize>(RestoreIssue::Count)>
        kRestoreIssues{"none",     "resources", "context", "state",        "bindings",
                       "supports", "combat",    "pools",   "adapter",      "world",
                       "fixtures", "camera",    "portals", "transporters", "commit"};
    struct Stage {
        LevelWorldCheckpoint::State world;
        LevelFixturesCheckpoint::State fixtures;
        std::optional<TowerCameraCheckpoint::State> camera;
        std::optional<ExitPortalsCheckpoint::State> portals;
        std::optional<LevelTransportersCheckpoint::State> transporters;
        bool operator==(const Stage&) const = default;
    };
    struct State {
        CombatRollbackCheckpoint::State combat;
        OpponentState opponents;
        OpponentCombatCheckpoint::State adapter;
        std::optional<Stage> stage;
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
                             std::span<const EnemyMissiles::Resource> missiles = {},
                             LevelWorldCheckpoint* world = nullptr,
                             LevelFixturesCheckpoint* fixtures = nullptr,
                             TowerCameraCheckpoint* camera = nullptr,
                             ExitPortalsCheckpoint* portals = nullptr,
                             LevelTransportersCheckpoint* transporters = nullptr);
    ~OrdinaryCombatCheckpoint();
    OrdinaryCombatCheckpoint(const OrdinaryCombatCheckpoint&) = delete;
    OrdinaryCombatCheckpoint& operator=(const OrdinaryCombatCheckpoint&) = delete;
    OrdinaryCombatCheckpoint(OrdinaryCombatCheckpoint&&) = delete;
    OrdinaryCombatCheckpoint& operator=(OrdinaryCombatCheckpoint&&) = delete;

    enum class CaptureOwner : usize {
        Bindings,
        Combat,
        Pools,
        Adapter,
        World,
        Fixtures,
        Camera,
        Portals,
        Transporters,
        Links,
        Count
    };
    static constexpr std::array<std::string_view, static_cast<usize>(CaptureOwner::Count)>
        kCaptureOwners{"bindings", "combat", "pools",   "adapter",      "world",
                       "fixtures", "camera", "portals", "transporters", "links"};
    using CaptureTimings = std::array<std::optional<u64>, static_cast<usize>(CaptureOwner::Count)>;
    /** Optional CPU timing of the actual captures; never recaptures for diagnostics. */
    std::optional<State>
    checkpoint(CaptureTimings* timings = nullptr,
               LevelWorldCheckpoint::CaptureTimings* worldTimings = nullptr) const;
    bool stageBound() const;
    std::optional<Prepared> prepareRestore(const State& state, RestoreIssue* issue = nullptr) const;
    bool canCommit(const Prepared& prepared) const;
    /** All child plans preflight before any live owner changes. The synchronous
     * commit allocates nothing and emits no callbacks. No native updates, owner
     * destruction or resource rebind may interleave prepare/preflight/commit. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state, RestoreIssue* issue = nullptr);
    /** Preallocates exact current-state undo across an ordinary native tick,
     * including placement watch/activation, pain RNG and progression changes.
     * Outputs remain caller-owned and buffered; unsupported external owners
     * must be refused before mutation. No nested restore or resource change
     * may occur between preparation and rewind. */
    std::optional<Rewind> prepareRewind() const;
    /** Validates/preallocates a different baseline without restoring it. All
     * incoming, resource and cross-owner checks remain identical to restore. */
    std::optional<Rewind> prepareRewind(const State& target) const;
    bool canRewind(const Rewind& rewind) const;
    bool rewind(Rewind&& rewind);

private:
    friend class OrdinaryCombatTick;
    /** Recycles only storage from the preceding private native boundary. The
     * world authenticates immutable values and captures all live state anew. */
    std::optional<State> capture(CaptureTimings* timings,
                                 LevelWorldCheckpoint::CaptureTimings* worldTimings,
                                 LevelWorldCheckpoint::State* reusableWorld) const;
    bool bindingsValid() const;
    bool targetValid(const State& state, RestoreIssue* issue = nullptr) const;
    CombatRollbackCheckpoint& m_combat;
    LevelOpponents& m_opponents;
    OpponentSimulation m_pools;
    OpponentCombatCheckpoint m_adapter;
    std::vector<EnemyMissiles::Resource> m_missiles;
    LevelWorldCheckpoint* m_world = nullptr;
    LevelFixturesCheckpoint* m_fixtures = nullptr;
    TowerCameraCheckpoint* m_camera = nullptr;
    ExitPortalsCheckpoint* m_portals = nullptr;
    LevelTransportersCheckpoint* m_transporters = nullptr;
    bool m_configurationValid = false;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
    u64 m_generation = 1;
};

} // namespace gdl::game
