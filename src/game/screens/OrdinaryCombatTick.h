#pragma once

#include <string>

#include "game/screens/OrdinaryCombatCheckpoint.h"
#include "game/screens/OrdinaryPickupPhase.h"
#include "game/screens/OrdinaryProjectilePhase.h"
#include "game/screens/OrdinaryStrongPhase.h"
#include "game/screens/OrdinaryWeaponEmission.h"
#include "game/screens/PartyDamage.h"
#include "game/screens/PartyMotion.h"
#include "game/world/ExitPortals.h"
#include "game/world/LevelTransporters.h"

namespace gdl::game {

/** Executable ordinary combat phase under one complete party/population undo.
 * Without stage owners, starts at PartyMotion after world/powerup preparation.
 * Jointly bound stage owners also advance native world/fixture/trigger phases;
 * players must still stand on static supports. Distant moving geometry is kept.
 * The caller supplies the gameplay projection. A jointly checkpointed camera
 * advances with the party; legacy callers retain held observation. Only static player supports and
 * plain walking, guard, close attacks, normal/strong throws on the ordinary swarm/generators, and
 * nonlethal incoming blows are admitted. Inert fixture and nonexplosive world stops retain native
 * impact presentation. Barrel hits and wood breaks can create prepared pickups;
 * ordinary Gold/Keys collection shares native judgment and mutation. Explosions and gas remain
 * unsupported. Other special attacks, pickups,
 * world events, lessons, promotions and
 * player death refuse the complete phase.
 *
 * Sound/rumble/footstep and BLOCKFX presentation requests are returned only on
 * acceptance. Swarm and generator hit effects use the trusted prebound native pool. Guard
 * gameplay is exact; BLOCKFX allocation/attachment and figure cosmetic clocks
 * are deliberately deferred, not a full visual-world replay.
 * No online packet/admission or application-side output dispatch lives here. */
class OrdinaryCombatTick {
public:
    struct Frame {
        s32 ticks = 1;
        f32 seconds = 1.0f / 60;
        WorldCamera camera;
        CameraView projection;
        Vec3 attention{0};
        /** Exact scene damage context at the start of this phase. The native
         * phase advances playSeconds before dispatching ordinary enemy blows. */
        PartyDamage::Context damage;
        /** Host scene option; the character's saved autoAttack preference is
         * checked independently by native PartyMotion. */
        bool autoMelee = false;
        bool valid() const;
    };
    struct Footstep {
        s32 player = -1;
        PartyMotion::Action foot = PartyMotion::Action::FirstFoot;
        Footing footing = Footing::Rock;
        bool operator==(const Footstep&) const = default;
    };
    struct FeedbackSound {
        std::string name;
        Vec3 position{0};
        f32 volume = 0;
        bool operator==(const FeedbackSound&) const = default;
    };
    enum class Status { Advanced, Invalid, Unsupported };
    enum class Refusal {
        None,
        Environment,
        Ambient,
        Admission,
        Placements,
        Statues,
        OpponentAdmission,
        Powerups,
        PlayerTimers,
        WorldMotion,
        PartyMotion,
        Fixtures,
        Surface,
        OpponentStep,
        EnemyCue,
        Promotion,
        Damage,
        Lessons,
        Feedback,
        RewardPromotion,
        Projectiles,
        Strikes,
        Pickups,
        Trigger,
        Portal,
        ExitWait,
        Postcondition,
        Count
    };
    static constexpr std::array<std::string_view, static_cast<usize>(Refusal::Count)> kRefusals{
        "none",         "Environment",  "Ambient",           "Admission",
        "Placements",   "Statues",      "OpponentAdmission", "Powerups",
        "PlayerTimers", "WorldMotion",  "PartyMotion",       "Fixtures",
        "Surface",      "OpponentStep", "EnemyCue",          "Promotion",
        "Damage",       "Lessons",      "Feedback",          "RewardPromotion",
        "Projectiles",  "Strikes",      "Pickups",           "Trigger",
        "Portal",       "ExitWait",     "Postcondition"};
    struct Timings {
        std::optional<u64> captureNanoseconds;
        std::optional<u64> prepareNanoseconds;
        std::optional<u64> postconditionNanoseconds;
        OrdinaryCombatCheckpoint::CaptureTimings postconditionOwners;
        LevelWorldCheckpoint::CaptureTimings postconditionWorld;
    };
    enum class PartyRefusal {
        None,
        UnsupportedAction,
        FamiliarShot,
        NoPotion,
        MeleeScenery,
        MeleeTarget,
        WeaponEmission,
        Selector,
        TurboAction,
        TurboEmission,
        TurboLesson,
        Statue,
        FloorSupport,
        GeneratorEmission,
        BarrelEmission,
        BarrelContents,
        BarrelExplosion,
        BarrelGas,
        Count
    };
    static constexpr std::array<std::string_view, static_cast<usize>(PartyRefusal::Count)>
        kPartyRefusals{"none",
                       "unsupported_action",
                       "familiar_shot",
                       "no_potion",
                       "melee_scenery",
                       "melee_target",
                       "weapon_emission",
                       "selector",
                       "turbo_action",
                       "turbo_emission",
                       "turbo_lesson",
                       "statue",
                       "floor_support",
                       "generator_emission",
                       "barrel_emission",
                       "barrel_contents",
                       "barrel_explosion",
                       "barrel_gas"};
    enum class TargetFamily {
        None,
        Barrel,
        Swarm,
        Generator,
        Critter,
        Boss,
        SafeRock,
        Wall,
        Switch,
        InertStop,
        Potion,
        Chest,
        Statue,
        Player,
        Unknown,
        Count
    };
    static constexpr std::array<std::string_view, static_cast<usize>(TargetFamily::Count)>
        kTargetFamilies{"none",   "barrel",    "swarm",  "generator", "critter",
                        "boss",   "safe_rock", "wall",   "switch",    "inert_stop",
                        "potion", "chest",     "statue", "player",    "unknown"};
    static constexpr usize kActionCount = static_cast<usize>(PlayerAnimator::Action::ComboJes) + 1;
    struct Result {
        Status status = Status::Invalid;
        Refusal refusal = Refusal::None;
        std::vector<CameraSubject> subjects;
        std::vector<Footstep> footsteps;
        std::vector<EnemyCue> enemyCues;
        std::optional<PlayerHealth::Heartbeat> heartbeat;
        std::vector<PartyDamage::Output> damage;
        std::vector<s32> meleeHits; ///< accepted contacts, in native order/input IDs
        std::vector<FeedbackSound> feedback;
        std::vector<LevelFixtures::NamedSound> fixtureSounds; ///< named, not distance-attenuated
        std::vector<OrdinaryWeaponEmission::Cue> throws;      ///< buffered once per native fan
        std::vector<OrdinaryStrongPhase::Cue> strong; ///< authored free-A cues, never played here
        std::vector<OrdinaryProjectilePhase::Cue> impacts;
        std::vector<OrdinaryPickupPhase::Output>
            pickups;     ///< accepted native order; never dispatched here
        Timings timings; ///< local CPU durations, including refused native attempts
        PartyRefusal partyRefusal = PartyRefusal::None; ///< detail when refusal is PartyMotion
        OrdinaryProjectilePhase::Refusal projectileRefusal = OrdinaryProjectilePhase::Refusal::None;
        /** Exact native action/winning target at the first action-related refusal.
         * Diagnostic only: an absent value never substitutes for admission. */
        std::optional<PlayerAnimator::Action> refusedAction;
        TargetFamily refusedTarget = TargetFamily::None;
    };

    /** Every owner and trusted archive must outlive this view. The supplied
     * classes and fixture binding must match the checkpoint's native resources;
     * step verifies exact runtime addresses/order, not just matching player IDs. */
    OrdinaryCombatTick(OrdinaryCombatCheckpoint& checkpoint, const ClassDataSet& classes,
                       LevelFixtures& fixtures);
    Result step(std::span<PlayerRuntime> party, std::span<const PlayInput> inputs,
                const Frame& frame);

private:
    friend class CombatPrediction;
    /** A native postcondition image, never a caller-supplied shortcut. Its
     * private owner additionally fences the exact replay tick and revision. */
    struct Boundary {
        std::unique_ptr<OrdinaryCombatCheckpoint::State> state;
        const OrdinaryCombatTick* owner = nullptr;
        const OrdinaryCombatCheckpoint::Rewind* rewind = nullptr;
    };
    /** Only the private owner may reuse its exact authority-interval undo.
     * It must rewind the entire interval on refusal or exception. */
    Result stepUnderRewind(std::span<PlayerRuntime> party, std::span<const PlayInput> inputs,
                           const Frame& frame, const OrdinaryCombatCheckpoint::Rewind& rewind,
                           Boundary& boundary);
    Result stepTimed(std::span<PlayerRuntime> party, std::span<const PlayInput> inputs,
                     const Frame& frame, Timings& timings,
                     const OrdinaryCombatCheckpoint::Rewind* intervalUndo, Boundary* boundary);
    bool bound(std::span<PlayerRuntime> party) const;
    bool environment() const;
    bool travelContact(std::span<const PlayerRuntime> party, bool portals) const;
    bool supported(const OrdinaryCombatCheckpoint::State& state,
                   std::span<const PlayerRuntime> party, f32 seconds) const;
    static TargetFamily targetFamily(s32 target);
    OrdinaryCombatCheckpoint& m_checkpoint;
    const ClassDataSet& m_classes;
    LevelFixtures& m_fixtures;
    std::vector<ExitPortals::Contact> m_exits;
    std::vector<LevelTransporters::Contact> m_transports;
};

} // namespace gdl::game
