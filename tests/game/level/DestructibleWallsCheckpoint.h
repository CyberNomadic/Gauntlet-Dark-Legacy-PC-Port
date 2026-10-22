#pragma once

#include "game/level/DestructibleWalls.h"
#include "game/level/FixtureCheckpointDetail.h"

namespace gdl::game {
/** Complete fixed-roster wall state. The caller must also rewind WorldCollision's
 * solid mask; a wall restore never modifies that external owner or appends surfaces. */
class DestructibleWallsCheckpoint {
public:
    struct Acquisition {
        Vec3 point{0};
        f32 radius = 0, distanceScale = 1, maxHeight = 0;
        bool enabled = true;
        bool operator==(const Acquisition&) const = default;
    };
    struct Bounds {
        s32 id = -1;
        Vec3 base{0};
        f32 radius = 0, height = 0;
        s32 node = -1;
        f32 targetScoreScale = 1, maxTargetDistance = 0;
        bool potionBottle = false;
        std::optional<Acquisition> acquisition;
        bool valid() const;
        bool operator==(const Bounds&) const = default;
    };
    struct Wall {
        fixture_checkpoint::Placement source;
        Mat4 transform{1};
        Vec3 collisionCentre{0};
        std::vector<CollisionTriangle> surface;
        Bounds bounds;
        s32 health = 0, armor = 0, minPlayers = 0, object = -1;
        bool shown = true;
        f32 flash = 0;
        s32 blinks = 0;
        f32 blinkTicks = 0;
        bool white = false;
        bool model = false;
        bool operator==(const Wall& /*other*/) const;
    };
    struct State {
        static constexpr usize kMaxWalls = 4096;
        static constexpr usize kMaxDetails = 65535;
        std::vector<Wall> walls;
        bool bound = false;
        bool whiteTexture = false;
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
        friend class DestructibleWallsCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    explicit DestructibleWallsCheckpoint(DestructibleWalls& owner) : m_owner(owner) {}
    DestructibleWallsCheckpoint(const DestructibleWallsCheckpoint&) = delete;
    DestructibleWallsCheckpoint& operator=(const DestructibleWallsCheckpoint&) = delete;
    ~DestructibleWallsCheckpoint() = default;
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation/callback-free and preserves every Wall/model/surface address.
     * Borrowed layout, models, textures and collision must remain alive and immutable
     * except for the enclosing transaction's owned collision-mask updates. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    DestructibleWalls& m_owner;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
