#pragma once

#include "game/world/FixtureCheckpointDetail.h"
#include "game/world/ItemFigureCheckpoint.h"
#include "game/world/Rotators.h"

namespace gdl::game {
/** Complete fixed-roster rotator state. The scene transforms and the caller's
 * pending RotatorCue queue are separate owners and must be rewound together. */
class RotatorsCheckpoint {
public:
    struct Rotator {
        fixture_checkpoint::Placement source;
        WorldObject target;
        Rotators::Cursor cursor;
        s32 subtype = 0;
        f32 speed = 0, limit = 0;
        Vec3 origin{0}, spot{0};
        f32 radius = 0, height = 0;
        std::optional<ItemFigureCheckpoint::State> pad;
        bool operator==(const Rotator&) const;
    };
    struct State {
        static constexpr usize kMaxRotators = Rotators::kMaxCursors;
        static constexpr usize kMaxDetails = 65535;
        std::vector<Rotator> rotators;
        bool bound = false;
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
        friend class RotatorsCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    explicit RotatorsCheckpoint(Rotators& owner) : m_owner(owner) {}
    RotatorsCheckpoint(const RotatorsCheckpoint&) = delete;
    RotatorsCheckpoint& operator=(const RotatorsCheckpoint&) = delete;
    RotatorsCheckpoint(RotatorsCheckpoint&&) = delete;
    RotatorsCheckpoint& operator=(RotatorsCheckpoint&&) = delete;

    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State&) const;
    bool canCommit(const Prepared&) const;
    /** No allocation, resource binding, geometry changes or cue dispatch. Plans
     * survive native updates, but not a rebind or another successful restore. */
    bool commitRestore(Prepared&&);
    bool restore(const State&);

private:
    Rotators& m_owner;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
