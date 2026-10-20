#pragma once

#include "game/world/ItemFigureCheckpoint.h"
#include "game/world/StaticScenery.h"

namespace gdl::game {

/** The complete ordered placed-scenery owner. Floor collision is borrowed, not
 * rewound here; no native updates, resource binding or callbacks run on restore. */
class StaticSceneryCheckpoint {
public:
    struct Support {
        s32 object = -1;
        Mat4 local{1};
        Vec3 boxOffset{0};
        f32 boxYaw = 0;
        bool valid() const;
        bool operator==(const Support&) const = default;
    };
    struct Prop {
        s32 instance = -1;
        ItemInfo definition;
        ItemInstance placement;
        s32 minPlayers = 0;
        bool shown = true;
        ItemFigureCheckpoint::State figure;
        Support support;
        Obstacle box;
        bool operator==(const Prop& /*other*/) const;
    };
    struct State {
        static constexpr usize kMaxProps = 4096;
        static constexpr usize kMaxChoices = 65535;
        static constexpr usize kMaxCollisionTriangles = 65535;
        std::vector<Prop> props;
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
        friend class StaticSceneryCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    explicit StaticSceneryCheckpoint(StaticScenery& owner);
    ~StaticSceneryCheckpoint();
    StaticSceneryCheckpoint(const StaticSceneryCheckpoint&) = delete;
    StaticSceneryCheckpoint& operator=(const StaticSceneryCheckpoint&) = delete;
    StaticSceneryCheckpoint(StaticSceneryCheckpoint&&) = delete;
    StaticSceneryCheckpoint& operator=(StaticSceneryCheckpoint&&) = delete;

    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation/callback-free after joint preflight. Native updates may occur
     * between preparation and undo; clear/rebind and sibling restores invalidate
     * the plan. Borrowed layout, archives and collision must outlive both owners. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    StaticScenery& m_owner;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
