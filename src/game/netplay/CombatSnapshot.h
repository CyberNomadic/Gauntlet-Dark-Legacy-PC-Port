#pragma once

#include <utility>

#include "engine/world/SceneGeometry.h"

#include "game/enemies/OpponentState.h"
#include "game/netplay/AnimationCheckpoint.h"
#include "game/netplay/EffectIdentity.h"
#include "game/netplay/FighterSnapshot.h"
#include "game/netplay/HudSnapshot.h"
#include "game/netplay/MotionSnapshot.h"
#include "game/netplay/PlatformCheckpoint.h"
#include "game/netplay/PlayerActionCheckpoint.h"
#include "game/netplay/PlayerKnockbackCheckpoint.h"
#include "game/netplay/PlayerLocomotionCheckpoint.h"
#include "game/netplay/PlayerMeleeCheckpoint.h"
#include "game/netplay/PlayerPoseCheckpoint.h"
#include "game/netplay/PlayerPotionCheckpoint.h"
#include "game/netplay/PlayerTurboCheckpoint.h"
#include "game/netplay/PotionIdentity.h"
#include "game/netplay/ShotIdentity.h"
#include "game/netplay/TurboEffectCheckpoint.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {

/** Authored sequence cursor, never a request to run animation events. Sequence
 * and action indexes require the session's matching build/assets handshake.
 * Generation changes on restart or wrap, so readers cannot blend two swings.
 * Zero generation denotes an unavailable figure/animation. */
struct CombatAnimation {
    u32 action = 0;
    u32 sequence = 0;
    u64 generation = 0;
    f32 frame = 0;
    f32 transition = 1;
    bool valid() const;
};

enum class ReplicaPlayerLife : u8 { Standing, Dying, InTower };
enum class ReplicaEnemyLife : u8 { Active, Asleep, Dying };

/** A per-seat companion mesh: slot 0 is familiar tier 1/2; slot 1 is the
 * powerup companion kind 1..7. Assets are selected before admitting packets. */
struct CompanionState {
    u32 form = 0;
    Mat4 placement{1};
    CombatAnimation animation;
    f32 textureClock = 0;
    f32 alpha = 1;
    /** Exact fire-shield emitter clock for bounded local locomotion replay. */
    std::optional<AnimationPlayer::Checkpoint> rollback = std::nullopt;
    bool valid(usize slot) const;
};

/** Host floor contact for the costume's shadow, not guest-side physics. */
struct PlayerShadowState {
    Vec3 ground{0};
    Vec3 normal{0, 1, 0};
    f32 alpha = 1;
    bool valid() const;
};

/** Host-approved local player replay envelope. A snapshot resolves all input ticks
 * through motion.tick (including missed deadlines); later commands may be replayed.
 * Disabling this envelope revokes prediction immediately, including pending inputs. */
struct PlayerMovementState {
    bool enabled = false;
    bool cameraLimited = false;
    bool ranged = false; ///< ordinary throws are not substituted with melee or item attacks
    /** Latest confirmed local pose is safe even when an action cannot be replayed.
     * False during captures, transport, shared cutscenes and other world-clock locks. */
    bool current = false;
    bool platform = false; ///< certified world-motion locomotion; actions remain authoritative
    f32 speed = 5;
    f32 radius = 0.75f;
    f32 height = 5;
    f32 followHeight = 2.5f;
    f32 cameraYaw = 0;
    Vec3 attention{0};
    u64 receivedInput = 0; ///< latest input tick observed, zero before any command
    s32 arrivalMargin = 0; ///< receivedInput minus host tick at first arrival
    u32 lateInputs = 0;    ///< unique commands that missed their simulation deadline
    bool valid() const;
};

/** Host-derived ordinary weapon launch parameters. Resource IDs refer only to the
 * preloaded projectile roster; no inventory or damage authority is transferred. */
struct PlayerWeaponState {
    u32 character = 0;
    Vec3 hand{0};
    f32 speed = 20;
    u32 resource = 0;
    u32 streak = 0;
    Color tint = Color::white();
    f32 forward = 0;
    std::optional<Vec3> target;
    /** Shared native effect-bank clock. Absent until its first projectile starts it. */
    std::optional<f32> textureFrame;
    u8 shots = 1;  ///< native fan size, never a new damage authority
    u32 rider = 0; ///< optional authored elemental throw effect, in the same trusted roster
    std::optional<f32> riderTextureFrame;
    bool valid() const;
};

/** Post-step attack modifiers and a host-certified prefix with unchanged effects.
 * Guest replay never consumes, toggles or renews inventory. A zero-tick prefix
 * keeps the current pose but requires a fresh checkpoint before predicting.
 * The prefix also binds the next-step effective pace in movement.speed; Speed
 * slots can expire and change that pace without clearing the Speed flag. */
struct PlayerWeaponModifiers {
    static constexpr u32 kFlags = PlayerMissiles::kReplayModifiers;
    static constexpr u32 kMaxTicks = 120;
    static constexpr f32 kMaxInventoryStep = (1.0f / 30) * 3;
    u32 flags = 0;
    u32 ticks = 0;
    bool speed = false;    ///< animation Speed, distinct from the weapon flag namespace
    u32 shields = 0;       ///< current native armor shield mask; only locomotion is replayable
    f32 fireSeconds = 0;   ///< longest Fire Shield duration, -1 for permanent, 0 without it
    f32 inventoryStep = 0; ///< native per-tick drain for the Fire Shield companion's fade
    bool valid() const;
};

struct PlayerCombatState {
    static constexpr u32 kKnockbackTicks = 12;
    f32 health = 0;
    ReplicaPlayerLife life = ReplicaPlayerLife::Standing;
    bool hitFlash = false;
    bool damageable = false;
    CombatAnimation animation;
    std::array<std::optional<CompanionState>, 2> companions{};
    /** Present only for departing survivors: 0 starts the effect, 1 hides the body.
     * Separate from motion: the collision anchor does not sink with the artwork. */
    std::optional<f32> portalPhase = std::nullopt;
    std::optional<PlayerShadowState> shadow = std::nullopt;
    PlayerMovementState movement{};
    std::optional<PlayerAnimator::Checkpoint> rollback = std::nullopt;
    /** Last actual executed input, retaining its original tick for held-input expiry.
     * Unlike receivedInput, this can seed replay after the post-step checkpoint. */
    std::optional<InputCommand> rollbackInput = std::nullopt;
    std::optional<PlayerWeaponState> weapon = std::nullopt;
    std::optional<PlayerMeleeState> melee = std::nullopt;
    /** Plain turbo timeline plus an independent movement region. Keeping it
     * separate preserves ordinary ranged previews when no melee is nearby. */
    std::optional<PlayerTurboState> turbo = std::nullopt;
    std::optional<PlayerMeleeState> turboGeometry = std::nullopt;
    /** Host-confirmed impulse and next-step reaction; never a new damage event. */
    std::optional<Knockback::Checkpoint> knockback = std::nullopt;
    PlayerDeed reaction = PlayerDeed::None;
    /** Zero-input static-floor corridor checked against current host bodies/fixtures. */
    u32 knockbackTicks = 0;
    bool weaponHidden = false; ///< native turbo row's held-weapon visibility, even without replay
    /** The turbo movement region also excludes ram-only fixture contacts. */
    bool shove = false;
    std::optional<PlayerWeaponModifiers> modifiers = std::nullopt;
    /** Native arm attachment, independent of whether local actions may be replayed. */
    u32 armEquipment = 0;
    std::optional<PlayerFloorState> floor = std::nullopt;
    std::optional<PlayerLocomotionState> locomotion = std::nullopt;
    std::optional<PlayerAnimator::TransitionPose> transitionPose = std::nullopt;
    std::optional<TurboEffectState> turboEffects = std::nullopt;
    std::optional<PlayerPotionState> potion = std::nullopt;
};

struct EnemyCombatState {
    u64 instance = 0; // not an enemy-pool slot
    u32 kind = 0;
    u32 tier = 1;
    u32 variant = 0;
    ReplicaEnemyLife life = ReplicaEnemyLife::Active;
    bool hitFlash = false;
    f32 health = 0;
    f32 fullHealth = 0;
    Vec3 position{0};
    f32 yaw = 0;
    CombatAnimation animation;
    f32 alpha = 1;
    f32 scale = 1;
    u32 deathSkin = 0; ///< trusted Enemies::kDeathSkins index; zero means no replacement
    u32 deathFrame = 0;
};

/** IDs live in a source namespace: separate pools may use the same counter.
 * Resource IDs are negotiated by the trusted asset roster, never file paths or
 * process pointers. These are visuals only, with no damage or collision inputs. */
enum class ProjectileSource : u8 {
    Player,
    Enemy,
    PlayerEffect,
    WorldEffect,
    Streak,
    Arrival,
    Sumner,
    Promotion,
    TowerRelic,
    TowerWizard,
    PlayerRider
};
struct ProjectileState {
    static constexpr u32 kAlong = 1;
    static constexpr u32 kSolidWorld = 2;
    static constexpr u32 kUnlit = 4;
    static constexpr u32 kDepthWrite = 8;
    static constexpr u32 kAdditive = 16;
    ProjectileSource source = ProjectileSource::Player;
    u64 instance = 0;
    u32 continuity = 1;
    u32 resource = 0;
    Mat4 placement{1};
    Vec3 direction{0};
    f32 age = 0;
    f32 radius = 1;
    f32 forward = 0;
    Color tint = Color::white();
    CombatAnimation animation;
    f32 textureFrame = 0;
    f32 alpha = 1;
    u32 flags = kDepthWrite;
    struct Rollback {
        ShotIdentity origin;
        u32 character = 0;
        PlayerMissiles::FlightCheckpoint flight;
        bool held = false; ///< the host world currently holds its projectile clock
        bool valid() const;
    };
    std::optional<Rollback> rollback = std::nullopt;
    /** Immutable emitter history survives the physical missile for native particle
     * tails. The enclosing placement/direction is its exact final emitter pose. */
    struct Particles {
        u32 character = 0;
        PlayerMissiles::ParticleFlight flight;
        std::optional<ShotIdentity> origin;
        bool held = false; ///< the host also holds particle-only retirement tails
        u8 potion = 0;     ///< zero for a weapon, otherwise the native bottle kind (1..4)
        std::optional<PotionIdentity> potionOrigin = std::nullopt;
        bool valid() const;
    };
    std::optional<Particles> particles = std::nullopt;
    /** Authored B/C area-effect provenance, separate from physical missiles. */
    std::optional<EffectIdentity> effectOrigin = std::nullopt;
    /** A native thrown bottle, not a weapon volley or a speculative impact burst. */
    struct PotionRollback {
        PotionIdentity origin;
        u8 kind = 1;
        PlayerMissiles::FlightCheckpoint flight;
        bool held = false;
        bool valid() const;
    };
    std::optional<PotionRollback> potion = std::nullopt;
    const ShotIdentity* releaseOrigin() const {
        if (rollback) {
            return &rollback->origin;
        }
        return particles && particles->origin ? &*particles->origin : nullptr;
    }
    const PotionIdentity* potionReleaseOrigin() const {
        if (potion) {
            return &potion->origin;
        }
        return particles && particles->potionOrigin ? &*particles->potionOrigin : nullptr;
    }
    auto key() const { return std::pair{source, instance}; }
    bool valid() const;
};

/** Visible pickup meshes only. IDs are append-only item indexes within an epoch;
 * absence removes an item. Collection, amounts, physics and ownership stay on the
 * host. Resource IDs refer to the trusted load-time archive/tree roster. */
struct PickupState {
    u64 instance = 0;
    u32 resource = 0;
    u32 continuity = 1;
    Mat4 placement{1};
    CombatAnimation animation;
    f32 textureFrame = 0;
    f32 alpha = 1;
    bool valid() const;
};

enum class FixtureSource : u8 {
    Chest,
    ChestPreview,
    Gate,
    Switch,
    Generator,
    Barrel,
    Trap,
    Rock,
    Rubble,
    Statue,
    Portal,
    SecretPortal
};
struct FixtureState {
    FixtureSource source = FixtureSource::Chest;
    u64 instance = 0;
    u32 resource = 0;
    u32 continuity = 1;
    Mat4 placement{1};
    CombatAnimation pose;
    u32 meshSequence = 0;
    f32 meshFrame = 0;
    u32 textureSequence = 0;
    f32 textureFrame = 0;
    f32 textureClock = 0;
    f32 alpha = 1;
    bool cameraFacing = false;
    auto key() const { return std::pair{source, instance}; }
    bool valid() const;
};

/** Atomic host checkpoint, including its motion/camera tick. Absence removes an
 * entity; a changed instance is a spawn, never a resurrection of a recycled slot.
 * This slice covers player health/actions, ordinary enemies and fighter meshes.
 * Inventory and one-shot audio/particle VFX are separate work. Pickup and
 * projectile models, effect-tree meshes, streaks and the level's moving/fading scene geometry share
 * this same atomic checkpoint. Geometry is optional for transport probes; a playable replica view
 * requires it. */
struct CombatSnapshot {
    static constexpr usize kMaxEnemies = 128;
    // These are visual components, not physical missiles: one ordinary throw
    // contributes its posed model and streak. Four rapid five-way players can
    // exceed 128 before adding familiars, enemy shots or world effects.
    static constexpr usize kMaxProjectiles = 1024;
    static constexpr usize kMaxPickups = 384;
    static constexpr usize kMaxFixtures = 1024;
    MotionSnapshot motion;
    f32 worldTextureFrame = 0; ///< host texture clock, held during cutscenes that freeze it
    s32 musicArea = -1;
    u32 musicLevel = 255;
    std::array<std::optional<PlayerCombatState>, InputCommand::kSeats> players;
    std::vector<EnemyCombatState> enemies;    // strictly increasing instance IDs
    std::vector<ProjectileState> projectiles; // strictly increasing source/instance pairs
    std::vector<PickupState> pickups;         // strictly increasing instance IDs
    std::vector<FixtureState> fixtures;       // strictly increasing source/instance pairs
    std::vector<FighterMeshState> fighters;   // strictly increasing actor/part pairs
    std::optional<SceneGeometry> geometry;
    std::optional<HudSnapshot> hud;
    std::optional<WorldMotion::State> worldMotion;
    u32 worldMotionTicks = 0;
    /** Complete ordinary-opponent pools from this exact motion tick. Optional
     * correction data, never a presentation-interpolated simulation seed. The
     * containing packet's unchanged byte budget may refuse a combined frame;
     * senders must deliberately omit this optional state or send a later seed,
     * not truncate pools or discard existing presentation/replay components. */
    std::optional<OpponentState> opponents = std::nullopt;
    bool valid() const;
};

/** Full snapshot payload, not a datagram: use CombatReplica::packets for bounded
 * chunks. Explicit scalar encoding; never memcpy game objects or save records. */
class CombatPacket {
public:
    static constexpr usize kHeaderBytes = 64;
    static constexpr usize kCompanionBytes = 84 + 4 + AnimationCheckpoint::kBytes;
    static constexpr usize kShadowBytes = 28;
    static constexpr usize kMovementBytes = 52;
    static constexpr usize kRollbackBytes = PlayerActionCheckpoint::kBytes;
    static constexpr usize kRollbackInputBytes =
        InputPacket::kHeaderBytes + InputPacket::kCommandBytes;
    static constexpr usize kWeaponBytes = 68;
    static constexpr usize kKnockbackBytes = PlayerKnockbackCheckpoint::kBytes + 8;
    static constexpr usize kModifierBytes = 20;
    static constexpr usize kArmEquipmentBytes = 4;
    static constexpr usize kPlayerBytes = 36 + 2 * kCompanionBytes + kShadowBytes + kMovementBytes +
                                          kRollbackBytes + kRollbackInputBytes + kWeaponBytes +
                                          kKnockbackBytes + kModifierBytes + kArmEquipmentBytes;
    static constexpr usize kEnemyBytes = 88;
    static constexpr usize kShotRollbackBytes = 84;
    static constexpr usize kParticleBytes = 96;
    static constexpr usize kEffectOriginBytes = 36;
    static constexpr usize kPotionRollbackBytes = 80;
    static constexpr usize kProjectileBytes =
        148 + kShotRollbackBytes + kParticleBytes + kEffectOriginBytes + kPotionRollbackBytes;
    static constexpr usize kPickupBytes = 96;
    static constexpr usize kFixtureBytes = 120;
    static constexpr usize kGeometryHeaderBytes = 20;
    static constexpr usize kGeometryObjectBytes = 64;
    static constexpr usize kMeleeBytes =
        4 + InputCommand::kSeats * (4 + PlayerMeleeCheckpoint::kMaxBytes);
    static constexpr usize kTurboBytes =
        4 + InputCommand::kSeats *
                (8 + PlayerTurboCheckpoint::kMaxBytes + PlayerMeleeCheckpoint::kMaxBytes);
    static constexpr usize kReplayBytes =
        4 + InputCommand::kSeats *
                (16 + PlayerLocomotionCheckpoint::kMaxBytes + PlayerPoseCheckpoint::kMaxBytes +
                 TurboEffectCheckpoint::kMaxBytes + PlayerPotionCheckpoint::kMaxBytes);
    static constexpr usize kOpponentHeaderBytes = 20; ///< length plus exact epoch/tick echo
    // Opponent corrections consume unused room inside this existing cap. It is
    // deliberately not increased by the opponent codec's independent maximum.
    static constexpr usize kMaxBytes =
        kHeaderBytes + MotionPacket::kMaxBytes + InputCommand::kSeats * kPlayerBytes +
        CombatSnapshot::kMaxEnemies * kEnemyBytes +
        CombatSnapshot::kMaxProjectiles * kProjectileBytes +
        CombatSnapshot::kMaxPickups * kPickupBytes + CombatSnapshot::kMaxFixtures * kFixtureBytes +
        kGeometryHeaderBytes + SceneGeometry::kMaxStates * kGeometryObjectBytes +
        FighterPacket::kMaxBytes + HudPacket::kMaxBytes + kMeleeBytes + kTurboBytes +
        PlatformCheckpoint::kMaxBytes + kReplayBytes;
    static std::optional<std::vector<u8>> encode(const CombatSnapshot& snapshot);
    static std::optional<CombatSnapshot> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
