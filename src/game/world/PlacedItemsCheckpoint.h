#pragma once

#include "game/world/PlacedItems.h"

namespace gdl::game {

/** Value checkpoint of the complete native pickup owner. Resource creation is
 * confined to explicit level-load binding; restore only copies trusted CPU state. */
class PlacedItemsCheckpoint {
public:
    struct Resource {
        std::string name;
        u32 archive = 0;
        bool operator==(const Resource&) const = default;
    };
    struct Pose {
        u32 resource = 0;
        TreePose::Checkpoint pose;
        bool operator==(const Pose&) const = default;
    };
    struct Floor {
        s32 object = -1;
        Mat4 local{1};
        bool operator==(const Floor&) const = default;
    };
    struct Item {
        u32 resource = 0;
        std::optional<Floor> floor;
        s32 instance = -1, info = -1, subtype = 0, value = 0, health = 0;
        u32 flags = 0;
        f32 strength = 0;
        s32 minPlayers = 0;
        f32 radius = 0, height = 0;
        Vec3 position{0};
        Mat4 transform{1};
        TreeParticles::Checkpoint particles;
        TreePose::Checkpoint pose;
        AnimationPlayer::Checkpoint animation;
        f32 alpha = 1;
        bool visible = false, taken = false, contained = false;
        Vec3 velocity{0};
        bool thrown = false, carried = false;
        s32 opener = -1;
        f32 noGrabSeconds = 0;
        Mat4 previousTransform{1};
        f32 previousFrame = 0, previousAlpha = 1;
        u64 previousGeneration = 0;
        bool presentationCaptured = false;
        u32 continuity = 1;
        std::optional<Pose> presentationPose;
        bool operator==(const Item&) const = default;
    };
    struct Effect {
        u32 resource = 0;
        Vec3 position{0};
        AnimationPlayer::Checkpoint animation;
        TreePose::Checkpoint pose;
        std::vector<std::pair<u32, u32>> emitters;
        f32 secondsLeft = 0;
        bool emitting = true;
        bool operator==(const Effect&) const = default;
    };
    struct State {
        static constexpr usize kMaxItems = 4096;
        static constexpr usize kMaxResources = 4096;
        static constexpr usize kMaxArchives = 64;
        static constexpr usize kMaxEffects = 256;
        static constexpr usize kMaxParticles = 1'000'000;
        static constexpr usize kMaxTriangles = 65535;
        std::vector<ItemInfo> definitions;
        std::vector<ItemInstance> placements;
        std::vector<bool> archives;
        std::vector<Resource> figures, bursts;
        std::vector<Item> items;
        std::vector<Effect> effects;
        ParticleField::Checkpoint particles;
        /** Catalog emitter index for every field entry, including retired entries
         * retained while another collection effect is still alive. */
        std::vector<u32> particleResources;
        std::vector<TextureAnimator::Checkpoint> textures;
        s32 players = 0;
        f32 revealTime = 0;
        bool revealing = false, capturePending = false, burstsAdvanced = false;
        bool layout = false, collision = false;
        bool valid() const;
        bool operator==(const State&) const;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class PlacedItemsCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    explicit PlacedItemsCheckpoint(PlacedItems& owner);
    ~PlacedItemsCheckpoint();
    PlacedItemsCheckpoint(const PlacedItemsCheckpoint&) = delete;
    PlacedItemsCheckpoint& operator=(const PlacedItemsCheckpoint&) = delete;
    /** Call once after native bind, with all ordered immutable archives available.
     * Preloads authored names, native replacement figures and collection bursts,
     * including resources not currently instantiated. Failure leaves no catalog. */
    bool bindResources(RenderDevice& device);
    void clearResources();
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation/callback-free commit. Ordinary owner mutations are undoable;
     * clear/rebind and a successful sibling restore invalidate prepared plans.
     * Immutable layout, collision, archives and device-owned fallback texture must
     * outlive the owner, catalog and every preparation. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    struct Catalog;
    bool ready() const;
    PlacedItems& m_owner;
    std::shared_ptr<const Catalog> m_catalog;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
