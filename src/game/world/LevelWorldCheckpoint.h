#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "engine/world/WorldStateCheckpoint.h"

#include "game/world/DestructibleWallsCheckpoint.h"
#include "game/world/FallingSceneryCheckpoint.h"
#include "game/world/LevelWorld.h"
#include "game/world/PlacedItemsCheckpoint.h"
#include "game/world/RotatorsCheckpoint.h"
#include "game/world/WorldDestructionCheckpoint.h"

namespace gdl::game {
/** All mutable state owned by an ordinary LevelWorld, not just passive motion.
 * Players, fixtures, portals, cameras and sound delivery remain separate owners.
 * A loaded/activated Skorne arena is unsupported and refuses the whole capture. */
class LevelWorldCheckpoint {
public:
    struct Cue {
        RotatorCue::Kind kind = RotatorCue::Kind::Turning;
        usize rotator = 0;
        Vec3 position{0};
        bool operator==(const Cue&) const = default;
    };
    struct State {
        static constexpr usize kMaxCues = 8192;
        static constexpr usize kMaxParticles = 1'000'000;
        LevelRef level;
        WorldStateCheckpoint::State core;
        LevelTriggers::Checkpoint triggers;
        WorldDestructionCheckpoint::State destruction;
        FallingSceneryCheckpoint::State falling;
        DestructibleWallsCheckpoint::State walls;
        PlacedItemsCheckpoint::State pickups;
        RotatorsCheckpoint::State rotators;
        TextureAnimator::Checkpoint textures;
        ParticleField::Checkpoint particles;
        std::vector<s32> particleSources; ///< ascending authored marker IDs, not draw order
        WorldStateCheckpoint::Light lighting, litNow;
        std::vector<Cue> cues;
        s32 playerCount = -1;
        f32 frameRemainder = 0, textureAdvance = 0, ambientOffset = 0;
        bool particlesAdvanced = false;
        bool valid() const;
        bool operator==(const State&) const = default;

    private:
        friend class LevelWorldCheckpoint;
        bool validAggregate() const;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class LevelWorldCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    explicit LevelWorldCheckpoint(LevelWorld& owner);
    ~LevelWorldCheckpoint();
    LevelWorldCheckpoint(const LevelWorldCheckpoint&) = delete;
    LevelWorldCheckpoint& operator=(const LevelWorldCheckpoint&) = delete;
    /** Explicit load-time catalog construction, including dormant pickup and
     * collection resources. No level or GPU resources are loaded by restore. */
    bool bindResources(RenderDevice& device);
    void clearResources();
    bool binds(const LevelWorld& owner) const;
    /** Native record birth through the persistent load-time pickup catalog.
     * Returns its actual appended index, never a synthesized checkpoint row.
     * Failure leaves the pickup owner unchanged; no loads/uploads or effects.
     * Ordinary successful births preserve an already-prepared whole-stage undo. */
    std::optional<usize> placeItemRecord(s32 record, const Vec3& position, s32 amount = 0);
    /** Ordered native collection using the persistent pickup owner. Absence
     * requires whole-transaction undo; collection bursts are not admitted. */
    std::optional<std::vector<Pickup>> collectItems(std::span<const Collector> collectors,
                                                    const StagedPickupJudge& judge);
    bool hasCollisionObject(s32 object) const;
    /** Sorted trusted identities for one synchronous support preflight. Borrowed
     * until resource rebind/clear; validates the catalog once for the batch. */
    std::span<const s32> collisionObjects() const;
    enum class CaptureOwner : usize {
        Core,
        Triggers,
        Destruction,
        Falling,
        Walls,
        Pickups,
        Rotators,
        Ambient,
        Aggregate,
        Count
    };
    static constexpr std::array<std::string_view, static_cast<usize>(CaptureOwner::Count)>
        kCaptureOwners{"core",    "triggers", "destruction", "falling",  "walls",
                       "pickups", "rotators", "ambient",     "aggregate"};
    using CaptureTimings = std::array<std::optional<u64>, static_cast<usize>(CaptureOwner::Count)>;
    /** Optional CPU nanoseconds for actual capture work, reset on every call.
     * Unreached operations remain absent, including an initial binding refusal. */
    std::optional<State> checkpoint(CaptureTimings* timings = nullptr) const;
    /** Consumes an earlier value image's pickup storage after exact catalog
     * authentication. Every live child and aggregate check still runs. Failure
     * may consume scratch storage, but cannot mutate any native participant. */
    std::optional<State> checkpoint(State&& reusable, CaptureTimings* timings = nullptr) const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Preflight every participant before mutation. Synchronous commit performs
     * no allocations, callbacks, cue dispatch or changes to borrowed asset owners.
     * Clear/reload or another successful aggregate/leaf restore invalidates a plan. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    struct Resources;
    bool bound() const;
    std::optional<State> capture(State* reusable, CaptureTimings* timings) const;
    LevelWorld& m_owner;
    std::shared_ptr<Resources> m_resources;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
