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

    private:
        friend class PlacedItemsCheckpoint;
        bool validIdentity() const;
        bool validMutable() const;
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
    /** Consumes value storage, not restore authority. The immutable identity
     * must exactly match the current bound catalog; all mutable values are
     * discarded, freshly captured and validated. Failure may consume storage
     * but never changes the native owner. Identical independent catalogs work. */
    std::optional<State> checkpoint(State&& reusable) const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation/callback-free commit. Ordinary owner mutations are undoable;
     * clear/rebind and a successful sibling restore invalidate prepared plans.
     * Immutable layout, collision, archives and device-owned fallback texture must
     * outlive the owner, catalog and every preparation. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

    /** Native pickup births using the untouched load-time figures. These are
     * synchronous CPU-only mutations, not deserialized item rows. The result is
     * the actual appended index; refusal (including missing art/capacity) leaves
     * the owner unchanged. Collision must be null or this owner's bound world.
     * Amount overrides one pickup's value, never the number of spawned items.
     * Existing prepared undo remains valid across successful births/releases. */
    std::optional<usize> place(std::string_view name, const Vec3& position,
                               const WorldCollision* collision);
    std::optional<usize> placeRecord(s32 record, const Vec3& position,
                                     const WorldCollision* collision, s32 amount = 0);
    std::optional<usize> throwItem(std::string_view name, const Vec3& position,
                                   const Vec3& velocity, const WorldCollision* collision,
                                   f32 noGrabSeconds, std::optional<f32> strength = std::nullopt);
    /** False is the native missing/not-carried no-op; absence is an unsupported
     * request or binding. This releases pickups only, never a Death actor. */
    std::optional<bool> release(usize index, const Vec3& position, const Vec3& velocity,
                                const WorldCollision* collision, f32 noGrabSeconds);

    /** Same ordered native offers and partial amounts, without creating bursts.
     * A refused judge/burst returns absence; earlier mutations require the
     * caller's enclosing rewind. No resource loading or presentation callbacks. */
    std::optional<std::vector<Pickup>> collect(std::span<const Collector> collectors,
                                               const StagedPickupJudge& judge);

private:
    struct Catalog;
    bool ready() const;
    std::optional<State> capture(State state) const;
    std::optional<PlacedItems::Item> birth(s32 record, const Vec3& position,
                                           const WorldCollision* collision, s32 amount) const;
    bool placementValid(const PlacedItems::Item& item) const;
    std::optional<usize> append(PlacedItems::Item item);
    PlacedItems& m_owner;
    std::shared_ptr<const Catalog> m_catalog;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
