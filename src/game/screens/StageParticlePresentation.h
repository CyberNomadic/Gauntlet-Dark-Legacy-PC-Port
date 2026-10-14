#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldLighting.h"

#include "game/screens/StageSnapshotSampling.h"

namespace gdl::game {
class EffectTrees;
class ExitPortals;
class ItemFigure;
class LevelFixtures;
class LevelOpponents;
class LevelWorld;

/** Owned, render-only particles from one accepted native post-step picture.
 * No emitter is advanced here. Mutable simulation owners may be rewound or
 * destroyed immediately after capture; immutable archives/textures must remain
 * alive until the supplied resource lifetime token is invalidated. */
class StageParticlePresentation {
public:
    enum class Phase : u8 {
        WorldScenery,  // falling pieces and breakable walls before world fixture figures
        WorldFixtures, // switch and rotator figures in native world opaque draw
        SolidFixtures, // chest/gate/barrel/statue figures before deferred world draw
        Pickups,
        World,
        Fixtures, // secret portals, then trap/scenery effects after the deferred world
        Projectiles,
        Effects
    };
    struct Figure {
        const ItemFigure* figure = nullptr;
        /** Native fixture draws usually use their discrete pose. Only callers
         * whose actual native draw supplies a fraction may enable this. */
        bool interpolated = false;
        Phase phase = Phase::Fixtures;
        /** Native null-camera ItemFigure draws use fixed default billboard
         * axes rather than the scene camera. */
        bool cameraFacing = true;
        /** Only missing decorative mesh owners: rotator pads and scenery.
         * Their complete copied native draw also owns their particles. */
        bool retainMesh = false;
    };
    struct Sources {
        const LevelWorld& world;
        const EffectTrees& projectiles;
        const EffectTrees& effects;
        const ExitPortals* portals = nullptr;
        /** Already filtered, in native draw order. Secret portals are captured
         * separately and must not also occur in this list. */
        std::span<const Figure> fixtures;
        bool projectilesAdvanced = true;
        /** Optional native 30 Hz swarm anchors, copied under the same resource
         * lifetime as this picture. No live enemy owner is retained. */
        const Enemies* enemies = nullptr;
        /** Native player endpoint poses under the same picture lifetime. */
        std::span<const PlayerRuntime> party;
        /** Complete current native draw-time list, already in native priority
         * order and capped by WorldLighting::kMostPoints. Copied during capture. */
        std::span<const PointLight> lights;
    };
    static constexpr usize kMaxRows = 16384;
    static constexpr usize kMaxParticles = 1'000'000;

    /** Exactly the native visible ItemFigure roster, in native order and pass,
     * excluding secret portals (captured separately). Mesh-only native owners
     * have no particle simulation to copy. Borrowed rows are for immediate
     * capture only; the returned sidecar never retains these owner pointers. */
    static std::optional<std::vector<Figure>> collectFigures(const LevelWorld& world,
                                                             const LevelFixtures& fixtures,
                                                             const LevelOpponents& opponents);

    StageParticlePresentation(StageParticlePresentation&&) noexcept;
    StageParticlePresentation& operator=(StageParticlePresentation&&) noexcept;
    ~StageParticlePresentation();
    StageParticlePresentation(const StageParticlePresentation&) = delete;
    StageParticlePresentation& operator=(const StageParticlePresentation&) = delete;

    /** Native sources only, never packet-selected pointers or resource loads.
     * Copies include all previous-step particle state and independent clocks.
     * Failure leaves every source untouched. The token is observed weakly: its
     * owner must invalidate it BEFORE releasing any borrowed artwork. */
    static std::unique_ptr<StageParticlePresentation>
    capture(u64 epoch, u64 tick, const std::shared_ptr<const void>& resourceLifetime,
            const Sources& sources);

    bool matches(u64 epoch, u64 tick) const;
    /** Also verifies the exact loaded resource owner, not just matching stamps. */
    bool matches(u64 epoch, u64 tick, const std::weak_ptr<const void>& resources) const;
    usize particleCount() const;
    const StageSnapshotSampling::Swarm* swarm() const;
    const StageSnapshotSampling::Party* party() const;
    /** Native lighting uses the current endpoint at every render fraction, not
     * interpolated array indices. Empty after resource-lifetime invalidation. */
    std::span<const PointLight> lights() const;
    /** Draw each phase exactly once in native ordering. While this complete
     * sidecar is selected, the replica's independently reconstructed weapon
     * particles must stand down, including retiring tails. Negative alpha is
     * the native held/options presentation; otherwise supply the SAME fraction
     * as this picture's geometry. No interpolation between unrelated fields. */
    bool draw(Phase phase, RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
              f32 alpha) const;
    /** Supplemental meshes not present in FixtureCapture. Invoke WorldScenery
     * after world opaque geometry (falling pieces, then breakable walls),
     * WorldFixtures next (pads: native All), SolidFixtures
     * with fixture depth geometry (scenery: DepthWriting), then Fixtures after
     * deferred world geometry (scenery: Effects, including its particles).
     * draw() deliberately excludes these figures' particles. */
    bool drawDecorations(Phase phase, RenderDevice& device, const Mat4& clip,
                         const WorldLighting& lighting, const CameraFrame& camera, f32 alpha) const;

private:
    struct Data;
    explicit StageParticlePresentation(std::unique_ptr<Data> data);
    std::unique_ptr<Data> m_data;
};
} // namespace gdl::game
