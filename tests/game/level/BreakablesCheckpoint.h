#pragma once

#include "game/level/Breakables.h"
#include "game/level/ItemFigureCheckpoint.h"

namespace gdl::game {

/** Complete barrel owner state. Strike drops and retirement events belong to the
 * caller; preparing or committing this state never dispatches either output. */
class BreakablesCheckpoint {
public:
    struct Support {
        s32 object = -1;
        Mat4 local{1};
        Vec3 boxOffset{0};
        f32 boxYaw = 0;
        bool valid() const;
        bool operator==(const Support&) const = default;
    };
    struct Barrel {
        s32 instance = -1;
        ItemInstance placement;
        BreakableStrike::Kind kind = BreakableStrike::Kind::Plain;
        s32 health = 0;
        s32 armor = 0;
        s32 contents = -1;
        s32 count = 0;
        s32 minPlayers = 0;
        s32 state = Breakables::kWhole;
        f32 breakingSeconds = 0;
        bool shown = true;
        bool gone = false;
        f32 radius = 1;
        f32 height = 3;
        Vec3 collisionOffset{0};
        ItemFigureCheckpoint::State figure;
        Obstacle box;
        Support support;
        bool operator==(const Barrel& /*other*/) const;
    };
    struct State {
        static constexpr usize kMaxBarrels = 4096;
        static constexpr usize kMaxInfos = 65535;
        static constexpr usize kMaxChoices = 65535;
        static constexpr usize kMaxCollisionTriangles = 65535;
        static constexpr usize kMaxName = 64;
        std::vector<ItemInfo> infos;
        std::vector<Barrel> barrels;
        u32 seed = Breakables::kSeedStart;
        bool collision = false;
        bool layout = false;
        bool valid() const;
        bool operator==(const State& /*other*/) const;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class BreakablesCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    explicit BreakablesCheckpoint(Breakables& owner);
    ~BreakablesCheckpoint();
    BreakablesCheckpoint(const BreakablesCheckpoint&) = delete;
    BreakablesCheckpoint& operator=(const BreakablesCheckpoint&) = delete;
    BreakablesCheckpoint(BreakablesCheckpoint&&) = delete;
    BreakablesCheckpoint& operator=(BreakablesCheckpoint&&) = delete;

    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation/callback-free after joint preflight. Native barrel steps may
     * be undone; clear/rebind and sibling restores invalidate prepared plans.
     * Bound layout, collision and immutable archives must outlive every plan. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    Breakables& m_owner;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
