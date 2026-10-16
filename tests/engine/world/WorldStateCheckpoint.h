#pragma once

#include <array>
#include <memory>
#include <span>

#include "engine/world/TextureAnimator.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"

namespace gdl {
/** Exact mutable scene/collision/animation state against immutable, already bound
 * assets. Unlike SceneGeometry, this retains the preceding display pose, every
 * collision object (including appended item surfaces), and pending cycle events. */
class WorldStateCheckpoint {
public:
    struct Resource {
        enum class Kind : u8 { Slot, Cycle };
        Kind kind = Kind::Slot;
        u32 index = 0, frame = 0;
        bool operator==(const Resource&) const = default;
    };
    struct Placement {
        Mat4 local{1}, previous{1};
        u32 continuity = 1;
        bool operator==(const Placement&) const = default;
    };
    struct Unit {
        u32 object = 0;
        f32 alpha = 1;
        bool visible = true;
        bool operator==(const Unit&) const = default;
    };
    struct Slot {
        u32 index = 0;
        std::optional<u32> texture, frame;
        std::vector<u32> cycle;
        f32 cyclePosition = 0, cycleSpeed = 0;
        Vec2 offset{0}, scrollPhase{0}, scrollVelocity{0};
        bool usable = false, thermal = false;
        bool operator==(const Slot&) const = default;
    };
    struct Point {
        Vec3 position{0}, color{1};
        f32 radius = 1, intensity = 2;
        bool operator==(const Point&) const = default;
    };
    struct Light {
        Vec3 ambient{0}, color{0}, direction{0};
        std::vector<Point> points;
        bool valid() const;
        bool operator==(const Light&) const = default;
        static Light capture(const WorldLighting& lighting);
        WorldLighting lighting() const;
    };
    struct Moving {
        s32 object = -1;
        Mat4 transform{1};
        bool operator==(const Moving&) const = default;
    };
    struct Cycle {
        s32 object = -1;
        bool wrapped = false;
        bool operator==(const Cycle&) const = default;
    };
    struct State {
        static constexpr usize kMaxObjects = 65535;
        static constexpr usize kMaxTriangles = 1'000'000;
        static constexpr usize kMaxTextures = 16384;
        static constexpr usize kMaxAnimations = 2048;
        static constexpr usize kMaxEvents = 4096;
        u64 layout = 0;
        std::array<u8, 32> catalog{}; ///< SHA-256 of trusted, immutable bound topology
        u32 resourceCount = 0;
        std::vector<Placement> placements;
        std::vector<Unit> units;
        std::vector<Slot> slots;
        Light light;
        f32 darken = 0;
        u32 textureFrame = 0;
        std::vector<Moving> moving;
        std::vector<s32> hidden, contactOnly, blockedFloorExits;
        std::vector<WorldAnimator::Cursor> animations;
        std::vector<Cycle> cycles;
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
        friend class WorldStateCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    /** The owning aggregate replaces binding whenever it clears/reloads the
     * three native owners. Its token field and immutable assets outlive this
     * wrapper and every preparation. No per-frame resource discovery is done. */
    WorldStateCheckpoint(WorldScene& scene, WorldCollision& collision, WorldAnimator& animator,
                         const std::shared_ptr<const u8>& binding);
    ~WorldStateCheckpoint();
    WorldStateCheckpoint(const WorldStateCheckpoint&) = delete;
    WorldStateCheckpoint& operator=(const WorldStateCheckpoint&) = delete;
    bool bind(const TextureAnimator& textures, const WorldLayout& layout);
    void clear();
    /** Trusted collision owners, not world-node indices: appended item surfaces
     * may have ids greater than the number of layout objects. */
    bool hasCollisionObject(s32 object) const;
    std::span<const s32> collisionObjects() const;
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    struct Catalog;
    bool ready() const;
    WorldScene& m_scene;
    WorldCollision& m_collision;
    WorldAnimator& m_animator;
    const std::shared_ptr<const u8>& m_binding;
    std::shared_ptr<const Catalog> m_catalog;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
    u64 m_generation = 0;
};
} // namespace gdl
