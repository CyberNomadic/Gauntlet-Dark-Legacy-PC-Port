#pragma once

#include "game/screens/LevelFixtures.h"
#include "game/world/BreakablesCheckpoint.h"
#include "game/world/ChestsCheckpoint.h"
#include "game/world/LockedGatesCheckpoint.h"
#include "game/world/RubbleCheckpoint.h"
#include "game/world/SafeRocksCheckpoint.h"
#include "game/world/StaticSceneryCheckpoint.h"
#include "game/world/TrapsCheckpoint.h"

namespace gdl::game {
/** All seven fixture pools and delayed hazard chains. A caller must jointly
 * restore players, effects, pickups, world and opponents before advancing; this
 * owner alone does not roll back emitted sound/help or mutate those owners. */
class LevelFixturesCheckpoint {
public:
    struct Cloud {
        Vec3 position{0};
        f32 damage = 0, secondsLeft = 0;
        u32 effect = 0;
        bool operator==(const Cloud&) const = default;
    };
    struct Blast {
        Vec3 position{0};
        f32 radius = 0, damage = 0, seconds = 1, elapsed = 0;
        u32 flags = 0;
        bool started = false, done = false;
        std::vector<usize> players;
        std::vector<s32> reached, opponents;
        std::vector<std::pair<usize, f32>> playerReady;
        std::vector<std::pair<s32, f32>> opponentReady;
        bool operator==(const Blast&) const = default;
    };
    struct State {
        ChestsCheckpoint::State chests;
        LockedGatesCheckpoint::State gates;
        TrapsCheckpoint::State traps;
        BreakablesCheckpoint::State barrels;
        SafeRocksCheckpoint::State rocks;
        StaticSceneryCheckpoint::State scenery;
        RubbleCheckpoint::State rubble;
        std::vector<Cloud> clouds;
        std::vector<Blast> blasts;
        std::vector<usize> doomedChests;
        f32 difficultyGain = 1;
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
        friend class LevelFixturesCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    explicit LevelFixturesCheckpoint(LevelFixtures& owner);
    ~LevelFixturesCheckpoint();
    LevelFixturesCheckpoint(const LevelFixturesCheckpoint&) = delete;
    LevelFixturesCheckpoint& operator=(const LevelFixturesCheckpoint&) = delete;
    /** Explicit load-time prewarming; required even for an empty native roster. */
    bool bindResources();
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Joint preflight before the first owner changes; synchronous commit makes
     * no allocations, device calls, sound or gameplay callbacks. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    struct Resources;
    bool bound() const;
    LevelFixtures& m_owner;
    std::unique_ptr<Resources> m_resources;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
