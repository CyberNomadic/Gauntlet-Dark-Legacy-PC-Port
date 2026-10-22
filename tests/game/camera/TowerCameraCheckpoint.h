#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include "game/camera/TowerCamera.h"
#include "game/level/LevelCatalog.h"

namespace gdl::game {
class LevelWorld;

/** Complete normal-camera continuation, including the nine-sample smoothing
 * rings and partially completed marker turn. Boss/scripted cameras, camera
 * shake, the prior rendered camera and projection are separate scene owners.
 * The native camera and loaded world must outlive this non-rebindable wrapper.
 * This value checkpoint does not itself enable moving-platform prediction. */
class TowerCameraCheckpoint {
public:
    struct Marker {
        u32 delay = 0, next = 0;
        Vec3 position{0}, rotation{0};
        bool operator==(const Marker&) const = default;
    };
    struct State {
        static constexpr usize kMaxMarkers = 2048;
        LevelRef level;
        std::vector<Marker> markers;
        CameraRange range;
        WorldCamera camera;
        Vec3 attention{0};
        f32 distance = 0;
        s32 marker = -1, aimedMarker = -1;
        f32 yawRate = 0, pitchRate = 0;
        f32 turnStep = static_cast<f32>(TowerCamera::kTurnSteps);
        std::array<Vec3, TowerCamera::kRing> ringPositions{};
        std::array<f32, TowerCamera::kRing> ringDistances{};
        s32 ringIndex = 0;
        bool valid() const;
        bool operator==(const State& /*other*/) const;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class TowerCameraCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    TowerCameraCheckpoint(TowerCamera& owner, const LevelWorld& world);
    ~TowerCameraCheckpoint();
    TowerCameraCheckpoint(const TowerCameraCheckpoint&) = delete;
    TowerCameraCheckpoint& operator=(const TowerCameraCheckpoint&) = delete;
    TowerCameraCheckpoint(TowerCameraCheckpoint&&) = delete;
    TowerCameraCheckpoint& operator=(TowerCameraCheckpoint&&) = delete;

    bool binds(const TowerCamera& owner, const LevelWorld& world) const;
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** No allocations or callbacks. Preparations survive normal native updates
     * for enclosing tick undo, but not reset, world reload, or another restore.
     * Preflight every owner before any synchronous aggregate commit. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    bool bound() const;
    bool matches(const State& state) const;
    TowerCamera& m_owner;
    const LevelWorld& m_world;
    std::shared_ptr<const u8> m_worldBinding;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
