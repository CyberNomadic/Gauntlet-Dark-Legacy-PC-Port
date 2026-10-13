#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "game/enemies/CritterStatues.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/world/EffectTrees.h"

namespace gdl::game {

class Combatant;
class LevelOpponents;

/** The ordinary population adapter's gameplay state, outside the independently
 * restored enemy/generator/missile pools. Restoring never consumes rewards,
 * plays sounds, creates effects, or advances the native phase. */
class OpponentCombatCheckpoint {
public:
    struct Reward {
        s32 player = -1;
        s32 experience = 0;
        bool destroyed = false;
        bool operator==(const Reward&) const = default;
    };
    struct State {
        static constexpr usize kMaxRecords = 65535;
        static constexpr usize kMaxDormantBanks = 16;
        struct DormantBank {
            static constexpr usize kMaxName = 31;
            CombatantKind kind = CombatantKind::Unknown;
            std::string name;
            TextureAnimator::Checkpoint textures;
            bool valid() const;
            bool operator==(const DormantBank&) const = default;
        };
        std::array<s32, 4> hitStreak{};
        std::vector<std::pair<s32, s32>> lessons;
        std::vector<Reward> generatorRewards;
        std::vector<RockHit> rockHits;
        std::vector<Vec3> hearers;
        /** Expired handles remain here until the native cue cleanup phase. */
        std::vector<u32> cueEffects;
        f32 shrink = 1;
        u32 bagSeed = 1;
        /** Retained preloaded statues and inactive family banks. These values
         * preserve dormant owners; they do not admit great-creature simulation. */
        CritterStatues::Checkpoint statues;
        std::vector<DormantBank> dormantBanks;
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
        friend class OpponentCombatCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    /** The owner, its borrowed archives, and shared effect pool must outlive this
     * wrapper and every preparation. Close/rebind invalidates existing plans;
     * restoring the separate population pools does not. */
    explicit OpponentCombatCheckpoint(LevelOpponents& owner);
    OpponentCombatCheckpoint(const OpponentCombatCheckpoint&) = delete;
    OpponentCombatCheckpoint& operator=(const OpponentCombatCheckpoint&) = delete;
    OpponentCombatCheckpoint(OpponentCombatCheckpoint&&) = delete;
    OpponentCombatCheckpoint& operator=(OpponentCombatCheckpoint&&) = delete;
    ~OpponentCombatCheckpoint();

    /** Refuses uncaptured great-creature, boss, Death, carried-item and
     * following-effect owners, including their pending output queues. The
     * supplied shared pool must be captured/restored by the enclosing bundle.
     * Dormant statues and loaded-but-unused family texture clocks are retained
     * exactly; callers must refuse waking or activating a great creature. */
    std::optional<State> checkpoint(const EffectTrees::PoolCheckpoint& effects) const;
    std::optional<Prepared> prepareRestore(const State& state,
                                           const EffectTrees::PoolCheckpoint& effects) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation-free and callback-free after successful preflight. No other
     * participant may mutate the bound level between preflight and commit. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state, const EffectTrees::PoolCheckpoint& effects);

private:
    static bool unused(const Combatant& actor);
    bool supported() const;
    LevelOpponents& m_owner;
    u64 m_generation = 0;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};

} // namespace gdl::game
