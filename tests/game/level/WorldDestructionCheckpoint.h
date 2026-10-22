#pragma once

#include <memory>
#include <optional>

#include "game/level/WorldDestruction.h"

namespace gdl::game {

/** Exact destruction ledger and pending blasts. Scene visibility, collision
 * masks and animator cycle events are separate owners: an enclosing world
 * transaction must restore them together before advancing native gameplay. */
class WorldDestructionCheckpoint {
public:
    struct Object {
        std::string name;
        Vec3 position{0};
        u32 flags = 0;
        u32 objectFlags = 0;
        s32 next = -1;
        s32 child = -1;
        s32 parent = -1;
        f32 radius = 0;
        bool noCollision = false;
        bool operator==(const Object&) const = default;
    };
    struct State {
        static constexpr usize kMaxObjects = 65535;
        static constexpr usize kMaxExplosions = 4096;
        std::vector<Object> objects;
        std::vector<bool> destroyed;
        std::vector<usize> controlled;
        std::vector<Vec3> explosions;
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
        friend class WorldDestructionCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    explicit WorldDestructionCheckpoint(WorldDestruction& owner) : m_owner(owner) {}
    WorldDestructionCheckpoint(const WorldDestructionCheckpoint&) = delete;
    WorldDestructionCheckpoint& operator=(const WorldDestructionCheckpoint&) = delete;
    ~WorldDestructionCheckpoint() = default;

    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Preflight all participating owners before calling commit. Native ledger
     * changes may intervene for undo, but clear/rebind or another restore
     * invalidates the plan. Commit allocates nothing and emits no explosions. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    bool sameRoster(const State& state) const;
    WorldDestruction& m_owner;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};

} // namespace gdl::game
