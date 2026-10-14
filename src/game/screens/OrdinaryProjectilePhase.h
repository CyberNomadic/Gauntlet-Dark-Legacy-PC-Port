#pragma once

#include <array>
#include <string_view>

#include "game/screens/CombatRollbackCheckpoint.h"

namespace gdl::game {

class LevelWorldCheckpoint;

/** The native player-projectile phase inside an already prepared ordinary-combat
 * transaction. This is not a world checkpoint: the caller must undo the complete
 * tick when advance refuses, including the missile queue it has consumed.
 * Accepted swarm/generator hits retain their native loss/feedback/reward queues for the NEXT
 * opponent phase. Native world and generator hit effects use trusted prebound resources; sound
 * requests are buffered. No reward, sound, lesson, or external callback is dispatched. */
class OrdinaryProjectilePhase {
public:
    static constexpr usize kMaxFlightSteps = 256;
    static constexpr usize kMaxTotalSteps = 4096;
    static constexpr usize kMaxImpacts = 4096;
    using Cue = PlayerArsenal::ImpactCue;
    enum class Refusal {
        None,
        Binding,
        Multiplayer,
        StrongState,
        TargetCapacity,
        Owner,
        Capture,
        State,
        Scenery,
        Impact,
        Target,
        WorldExplosion,
        Enemy,
        Generator,
        Resource,
        Effect,
        GeneratorEmission,
        BarrelBlasts,
        BarrelContents,
        BarrelExplosion,
        BarrelGas,
        BarrelEmission,
        Count
    };
    static constexpr std::array<std::string_view, static_cast<usize>(Refusal::Count)> kRefusals{
        "none",
        "binding",
        "multiplayer",
        "strong_state",
        "target_capacity",
        "owner",
        "capture",
        "state",
        "scenery",
        "impact",
        "target",
        "world_explosion",
        "enemy",
        "generator",
        "resource",
        "effect",
        "generator_emission",
        "barrel_blasts",
        "barrel_contents",
        "barrel_explosion",
        "barrel_gas",
        "barrel_emission"};

    struct Result {
        bool advanced = false;
        usize impacts = 0;
        std::vector<Cue> cues;
        Refusal refusal = Refusal::None;
        /** Native impact target when one was actually selected; diagnostic only. */
        std::optional<s32> refusedTarget;
    };

    /** Pure admission/work check against complete native state and a trusted
     * immutable resource manifest. Plain normal-multiplayer class weapons only;
     * potion, piercing, reflected, and special-weapon gameplay remain excluded.
     * Retained visual banks and particle tails are not silently discarded. */
    static bool supported(const PlayerArsenalCheckpoint::State& state,
                          std::span<const PlayerMissiles::Resource> resources, f32 seconds);
    /** Trusted native ordinary weapon-impact artwork may remain after its shot
     * retires. This does not admit potion, elemental, or world-explosion effects. */
    static bool supportsEffect(const PlayerArsenal& arsenal,
                               const EffectTrees::PoolResource& resource);

    /** Uses the bound arsenal and the exact native target, collision, and impact
     * ordering. All queued and new impacts are checked BEFORE the first impact
     * is dispatched. Surviving shot endpoints use the same scenery contact
     * predicate as native play; distant or already-started scenery is harmless.
     * Barrel hits reuse the native rounded damage/choice outcome. Nonbreaking
     * hits and Plain/empty-Holding breaks need no additional owner. Passing the
     * persistent Stage world enables prepared Holding pickup births with native
     * suppressed lessons; a null world retains the earlier no-drop domain.
     * Hazards remain unsupported. Any prior blast queue forbids barrel dispatch.
     * A refusal after flight changes only owners covered by the
     * caller's prepared OrdinaryCombatCheckpoint undo. The caller also verifies
     * the resulting complete state, including newly queued lessons/rewards. */
    static Result advance(PlayerAttacks& attacks, std::span<PlayerRuntime> players,
                          const PlayerAttacks::Targets& targets, f32 seconds,
                          const CombatRollbackCheckpoint::Resources& resources,
                          LevelWorldCheckpoint* world = nullptr);

private:
    friend class OrdinaryWeaponEmission;
    friend class OrdinaryCombatTick;
    static std::optional<EffectTrees::Prepared>
    impactResource(const PlayerArsenal::ImpactPresentation& emission,
                   std::span<const EffectTrees::PoolResource> resources,
                   std::span<TextureSet* const> lenders, const RenderDevice& device);
    static bool ordinaryTarget(s32 target);
    static bool ordinaryGenerator(const Generators& generators, s32 slot);
    static std::optional<u32> generatorPresentation(
        const GeneratorFeedbackEmission& emission, EffectTrees& effects, const RenderDevice& device,
        std::span<const EffectTrees::PoolResource> resources, std::vector<Cue>& cues);
};

} // namespace gdl::game
