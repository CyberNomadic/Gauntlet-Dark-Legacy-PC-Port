#pragma once

#include <array>
#include <memory>

#include "game/level/FixtureCheckpointDetail.h"
#include "game/level/LevelTransporters.h"

namespace gdl::game {
/** Exact pad continuation, not player transportation. Archives, layout and collision
 * bindings must remain loaded and immutable through every prepared restore. The
 * catalog is built explicitly from native bound resources, never from a packet. */
class LevelTransportersCheckpoint {
public:
    struct Pad {
        fixture_checkpoint::Placement source;
        s32 id = 0, destinationId = 0;
        std::optional<usize> destination;
        Vec3 position{0};
        f32 radius = 0, height = 0;
        Mat4 transform{1};
        s32 support = -1;
        Mat4 supportLocal{1};
        Vec3 anchorLocal{0};
        bool model = false;
        std::optional<TreePose::Checkpoint> pose;
        AnimationPlayer::Checkpoint animation;
        f32 previousFrame = 0;
        u64 previousGeneration = 0;
        bool operator==(const Pad&) const = default;
    };
    struct State {
        static constexpr usize kMaxPads = 1024;
        static constexpr usize kMaxDetails = 65535;
        std::array<u8, 32> catalog{};
        std::string tree;
        s32 players = 1;
        bool collision = false;
        std::vector<Pad> pads;
        TextureAnimator::Checkpoint textures;
        bool presentationAdvanced = false;
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
        friend class LevelTransportersCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    explicit LevelTransportersCheckpoint(LevelTransporters& owner);
    ~LevelTransportersCheckpoint();
    LevelTransportersCheckpoint(const LevelTransportersCheckpoint&) = delete;
    LevelTransportersCheckpoint& operator=(const LevelTransportersCheckpoint&) = delete;
    /** Load-time only: CPU catalog construction, with no render-device calls. */
    bool bindResources();
    bool binds(const LevelTransporters& owner) const;
    bool binds(const WorldLayout& layout, const WorldCollision* collision) const;
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation/callback-free after preflight. Animate/sync/draw may intervene;
     * clear/rebind/catalog replacement or another successful restore may not. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    struct Resources;
    bool bound() const;
    LevelTransporters& m_owner;
    std::shared_ptr<const Resources> m_resources;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
