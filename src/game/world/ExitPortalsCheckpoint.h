#pragma once

#include <memory>

#include "game/world/ExitPortals.h"
#include "game/world/FixtureCheckpointDetail.h"
#include "game/world/ItemFigureCheckpoint.h"

namespace gdl::game {
/** Exact portal continuation only: checkpoint/restore never runs readiness,
 * consumes a portal, drains wait messages, or dispatches transportation.
 * The trusted layout, archives and collision owner must remain loaded and
 * immutable until this wrapper and every prepared restore have been released. */
class ExitPortalsCheckpoint {
public:
    struct Portal {
        fixture_checkpoint::Placement source;
        Vec3 position{0};
        Mat4 transform{1};
        s32 support = -1;
        Mat4 supportLocal{1};
        bool secret = false;
        s32 minPlayers = 1;
        f32 radius = 3;
        bool consumed = false, shut = false;
        f32 alpha = 1;
        std::optional<Vec3> departurePosition;
        s32 archive = -1; ///< -1 absent, 0 primary items, 1 realm items
        bool model = false;
        ItemFigureCheckpoint::State icon;
        std::string tag;
        std::optional<LevelRef> destination;
        s32 action = 0, ticksLeft = 0;
        std::optional<TreePose::Checkpoint> pose;
        AnimationPlayer::Checkpoint player;
        bool operator==(const Portal&) const = default;
    };
    struct Visitor {
        s32 party = -1, ticks = 0;
        bool operator==(const Visitor&) const = default;
    };
    struct State {
        static constexpr usize kMaxPortals = 4096;
        static constexpr usize kMaxWaiting = 16384;
        static constexpr usize kMaxTriangles = 65535;
        static constexpr usize kMaxParticles = 1'000'000;
        std::vector<Portal> portals;
        std::string tree;
        std::array<s32, ExitPortals::kSequences.size()> sequences{-1, -1, -1, -1, -1};
        std::vector<s32> waiting;
        std::vector<Visitor> visitors;
        bool collision = false;
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
        friend class ExitPortalsCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    ExitPortalsCheckpoint(ExitPortals& owner, const WorldLayout& layout, ItemArchive& items,
                          ItemArchive* realm = nullptr);
    ~ExitPortalsCheckpoint();
    ExitPortalsCheckpoint(const ExitPortalsCheckpoint&) = delete;
    ExitPortalsCheckpoint& operator=(const ExitPortalsCheckpoint&) = delete;
    ExitPortalsCheckpoint(ExitPortalsCheckpoint&&) = delete;
    ExitPortalsCheckpoint& operator=(ExitPortalsCheckpoint&&) = delete;

    /** Explicitly records the already-bound native roster; no GPU allocation or
     * archive loading. Repeat after a genuine native clear/rebind, not per tick. */
    bool bindResources();
    bool binds(const ExitPortals& owner) const;
    bool binds(const WorldLayout& layout, const ItemArchive& items, const ItemArchive* realm,
               const WorldCollision& collision) const;
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation/callback-free after joint preflight. Native updates may
     * intervene before undo; clear/rebind or any sibling restore may not. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    struct Resources;
    bool bound() const;
    s32 archiveId(const ItemArchive* archive) const;
    ExitPortals& m_owner;
    const WorldLayout& m_layout;
    ItemArchive& m_items;
    ItemArchive* m_realm = nullptr;
    std::unique_ptr<Resources> m_resources;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
