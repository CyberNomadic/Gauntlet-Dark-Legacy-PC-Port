#pragma once

#include "game/level/ItemFigureCheckpoint.h"
#include "game/level/LockedGates.h"

namespace gdl::game {

/** Exact gate phase owner; restoring cannot consume keys or emit GateEvents. */
class LockedGatesCheckpoint {
public:
    struct Support {
        s32 object = -1;
        Mat4 local{1};
        Vec3 boxOffset{0};
        f32 boxYaw = 0;
        bool valid() const;
        bool operator==(const Support&) const = default;
    };
    struct Gate {
        s32 instance = -1;
        ItemInfo definition;
        ItemInstance placement;
        s32 subtype = 0;
        s32 state = LockedGates::kShut;
        s32 openingTicks = 0;
        f32 refusalLeft = 0;
        s32 minPlayers = 0;
        bool shown = true;
        bool blocksPassage = true;
        ItemFigureCheckpoint::State figure;
        Obstacle box;
        Support support;
        bool operator==(const Gate& /*other*/) const;
    };
    struct State {
        static constexpr usize kMaxGates = 4096;
        static constexpr usize kMaxCollisionTriangles = 65535;
        std::vector<Gate> gates;
        bool collision = false;
        bool layout = false;
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
        friend class LockedGatesCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    explicit LockedGatesCheckpoint(LockedGates& owner);
    ~LockedGatesCheckpoint();
    LockedGatesCheckpoint(const LockedGatesCheckpoint&) = delete;
    LockedGatesCheckpoint& operator=(const LockedGatesCheckpoint&) = delete;
    LockedGatesCheckpoint(LockedGatesCheckpoint&&) = delete;
    LockedGatesCheckpoint& operator=(LockedGatesCheckpoint&&) = delete;

    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation/callback-free commit after joint preflight. Owned native gate
     * steps are undoable; rebind/clear and any sibling restore invalidate plans.
     * The bound layout, collision and immutable archives must outlive every plan. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    LockedGates& m_owner;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
