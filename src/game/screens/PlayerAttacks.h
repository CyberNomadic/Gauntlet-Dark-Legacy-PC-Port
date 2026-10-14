#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/world/AmbientDimmer.h"

#include "game/combat/MeleeGeometry.h"
#include "game/players/ItemAttack.h"
#include "game/players/MeleeSelection.h"
#include "game/screens/LevelFixtures.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/TurboEmission.h"
#include "game/world/CameraShake.h"
#include "game/world/MoveStrikes.h"
#include "game/world/PlayerArsenal.h"
#include "game/world/TargetAssist.h"
namespace gdl::game {
/** Resolves player attacks against level targets and owns their transient effects.
 * Arsenal owns projectile models; TurboMove owns per-player timelines. This component
 * connects animation actions to world hits, with no scene or retained party span.
 * Clear before releasing its borrowed effect store, arsenal, figures or archives. */
class PlayerAttacks {
public:
    static constexpr f32 kRamReach = 0.3f; ///< native charge contact margin beyond body radius
    struct Resources {
        RenderDevice& device;
        const ClassDataSet& classes;
        LevelWorld& world;
        ItemArchive& weapons;
        EffectTrees& effects;
        LevelSoundscape& audio;
        SoundPlayer* sounds;
        PlayerArsenal& arsenal;
        AmbientDimmer& dimmer;
        CameraShake* shake = nullptr;
        const MultiplayerMode* multiplayer = nullptr;
        f32 acquisitionCone = TargetAssist::kFacingDot;
        std::function<void(s32)>
            hammerImpact{}; // NOLINT(readability-redundant-member-init): preserve aggregate callers
    };
    struct Targets {
        LevelOpponents& opponents;
        LevelFixtures& fixtures;
        LevelFixtures::Events fixtureEvents;
        MultiplayerMode multiplayer = MultiplayerMode::Normal;
        // Preserve world-only aggregate callers on compilers warning about omitted fields.
        std::span<const PlayerRuntime> players{}; // NOLINT(readability-redundant-member-init)
        std::function<void(usize, f32, HurtKind, const PlayerImpact&)>
            hurt{};            // NOLINT(readability-redundant-member-init)
        s32 occupiedHand = -1; ///< a relic replaces this player's weapon glow
        /** An accepted melee blow on a creature, identified by its attacker's input slot. */
        std::function<void(s32)>
            meleeHit{}; // NOLINT(readability-redundant-member-init): preserve aggregate callers
    };
    void bind(const Resources& resources);
    void clear();
    void ramBarrels(usize index, std::span<PlayerRuntime> players, const Targets& targets);
    void shieldPotion(usize index, std::span<PlayerRuntime> players);
    void usePotion(usize index, std::span<PlayerRuntime> players);
    /** A broken world pickup, not a cast by any member of the party. */
    void shatterPotion(s32 kind, const Vec3& position);
    void useItemAttack(usize index, std::span<PlayerRuntime> players);
    void showBlock(usize index, f32 taken, f32 left, std::span<PlayerRuntime> players);
    void updateTurbo(usize index, s32 ticks, f32 seconds, std::span<PlayerRuntime> players,
                     const std::function<void(s32, usize)>& help);
    /** The combo's start (StartComboFX, pmotion.c 3173): the sphere of the weapons archive
     * tinted the partner's colour and lit in it, and the burst of the grabber's own colour,
     * about the grabber. */
    void comboStart(usize index, std::span<PlayerRuntime> players);
    /** A flier or charger against the level's items (PlayerMotion_HitTarget, pmotion.c
     * 1302): each barrel, wall, rock and generator its body touches takes `blow` once a
     * flight, knocked down, and the thrower's combo-hit row bursts where it struck. True
     * when it struck anything. */
    bool comboImpact(usize flier, usize thrower, f32 blow, std::span<PlayerRuntime> players,
                     const Targets& targets);
    /** Missiles, potion waves and item attacks fly on; first the elemental weapons' glow is
     * set at each hand for the frame. */
    void updateProjectiles(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    /** The elemental weapon's glow in each hand (PlayerProcessPowerups, player.c 5749): worn,
     * its element's WEAP_HOLD effect of the costume colour's effects rides the weapon hand at
     * the class's offset and scale for the character's tier; put out with the powerup, or
     * while the right gauntlet, the super shot or the thunder hammer fills the hand. */
    void glowWeapons(std::span<PlayerRuntime> players, s32 occupiedHand = -1);
    void updateStrikes(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    void updateShields(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    /** Queries touching Death without advancing damage, sounds or the held state. */
    std::optional<Vec3> deathContact(usize index, std::span<const PlayerRuntime> players,
                                     const Targets& targets) const;
    /** A halo wearer whose nearest thing ahead is Death holds him, drawing a point off him a
     * frame; where he is while held, else nothing (and the hold let go). `allowed` false
     * lets any hold go. */
    std::optional<Vec3> grabDeath(usize index, s32 ticks, bool allowed,
                                  std::span<PlayerRuntime> players, const Targets& targets);
    /** Stops held-drain loops when simulation pauses; a live hold restarts them on resume. */
    void stopDeathSounds(std::span<PlayerRuntime> players);
    static constexpr f32 kGrabReach = 30.0f; ///< PlayerGetTarget's reach
    static constexpr f32 kGrabCone = 0.707f; ///< how far ahead a new target must lie
    static constexpr f32 kHeldCone = 0.5f;   ///< and one already held
    /** The fire and lightning shields harm the creature their bearer stands against. */
    void updateArmour(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    static constexpr f32 kArmourReach = 1.0f;      ///< past the body's radius
    static constexpr f32 kFireShieldDamage = 3.0f; ///< a 30 Hz frame, never waiting
    static constexpr f32 kLightningShieldDamage = 20.0f;
    static constexpr f32 kShockGap = 1.0f; ///< seconds before the lightning strikes one again
    void cry(usize index, std::string_view which, std::span<PlayerRuntime> players);
    std::optional<Vec3> aim(const PlayerActor& actor, const Vec3& facing,
                            const Targets& targets) const;
    /** What the attack buttons ask of a character: a close attack when something is within
     * a swing, or within a step while the stick moves, else a throw. `chain` is how far into
     * a chain of close attacks it is. */
    PlayerDeed attackDeed(const PlayerActor& actor, bool strong, const Targets& targets,
                          bool moved = false, s32 chain = 0, std::optional<Vec3> facing = {}) const;
    /** Conservative ordinary-throw eligibility at every facing. Nearby melee or
     * item substitutions require authoritative resolution, even when automatic
     * melee is disabled or a contact shield suppresses its unpressed attacks. */
    bool rangedOnly(const PlayerActor& actor, const Targets& targets) const;
    /** The selected close target may start a walking body's unpressed quick strike.
     * Movement, input and animation eligibility belong to the caller. */
    PlayerDeed automaticMeleeDeed(const PlayerActor& actor, const Targets& targets,
                                  const Vec3& facing) const;
    /** Where the nearest thing to strike lies (AnimAction's collision bits): a held attack
     * button reaches a unit further. */
    MeleeSense meleeSense(const PlayerActor& actor, bool held, const Targets& targets,
                          std::optional<Vec3> facing = {}) const;
    /** Pointer-free all-bearing selection at fixed feet. It deliberately recomputes
     * a stationary contact instead of retaining the last moving attackStep. Hurt
     * multiplayer remains native-only because its party fallback is not captured. */
    std::optional<MeleeSelection::Snapshot> meleeAcquisition(const PlayerActor& actor,
                                                             const Targets& targets) const;
    /** Complete nearby cylinder context for bounded moving acquisition. Refuses
     * unsupported geometry instead of silently omitting a possible target. */
    std::optional<std::vector<MeleeGeometry::Target>> meleeGeometry(const PlayerActor& actor,
                                                                    const Targets& targets,
                                                                    f32 movementRadius,
                                                                    bool requireClose = true) const;
    static constexpr f32 kSwingReach = MeleeSelection::kSwingReach;
    static constexpr f32 kStepReach = MeleeSelection::kStepReach;
    static constexpr f32 kHeldReach = MeleeSelection::kHeldReach;
    static constexpr f32 kLowEnemy = 4.0f; ///< the height under which an enemy is short
    static constexpr f32 kLowThing = 3.5f; ///< and anything else
    void melee(usize index, std::span<PlayerRuntime> players, const Targets& targets);
    /** The same native contact, with generator presentation supplied by a
     * transaction-local publisher. A refusal requires complete caller undo. */
    bool melee(usize index, std::span<PlayerRuntime> players, const Targets& targets,
               const LevelOpponents::GeneratorPublisher& generators,
               const LevelFixtures::BarrelPublisher& barrels = {});
    const MoveStrikes& strikes() const { return m_strikes; }
    usize shieldCount() const { return m_shields.size(); }
    /** Persistent outgoing damage not owned by ordinary-opponent replay. A
     * completed player action can leave these areas active for later ticks. */
    bool pendingGameplay() const {
        return m_strikes.count() != 0 || !m_items.empty() || !m_potions.empty() ||
               !m_shields.empty();
    }

    /** Native base damage used to prepare the same visual strike descriptors. */
    f32 meleeDamageOf(usize index, std::span<const PlayerRuntime> players) const;

private:
    friend struct PlayerAttacksTestPeer;
    friend class PlayerAttacksCheckpoint;
    friend class CombatRollbackCheckpoint;
    friend class OrdinaryCombatTick;
    friend class OrdinaryProjectilePhase;
    friend class OrdinaryStrongPhase;
    std::shared_ptr<const u8> m_restoreOwner = std::make_shared<const u8>(0);
    u64 m_restoreGeneration = 1;
    std::optional<s32> deathContactSlot(usize index, std::span<const PlayerRuntime> players,
                                        const Targets& targets) const;
    void beginPotion(const MissileImpact& impact);
    void updatePotions(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    void updateItems(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    /** The native projectile phase split at its output queue. Ordinary replay
     * checks the complete batch before invoking any impact-side world owner. */
    std::vector<MissileImpact>
    advanceProjectileFlights(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    /** Shared native flight phase before surviving-endpoint scenery/audio and
     * impact-queue consumption. The caller owns any subsequent refusal/undo. */
    void stepProjectileFlights(f32 seconds, std::span<PlayerRuntime> players,
                               const Targets& targets);
    bool dispatchProjectileImpact(
        const MissileImpact& impact, std::span<PlayerRuntime> players, const Targets& targets,
        const std::function<void(const MissileImpact&, f32)>& presentation = {},
        const LevelOpponents::GeneratorPublisher& generators = {},
        const LevelFixtures::BarrelPublisher& barrels = {});
    void strikeTarget(const MissileTarget& target, f32 damage, u32 flags, const PlayerActor& owner,
                      std::span<PlayerRuntime> players, const Targets& targets);
    struct ItemArea {
        ItemAttack attack;
        usize actor = 0;
        u32 effect = 0;
        f32 elapsed = 0;
        f32 lifetime = 1;
        bool pojoTurbo = false;
        std::vector<s32> hit;
    };
    std::vector<ItemArea> m_items;
    struct PotionBurst {
        MissileImpact impact;
        f32 elapsed = 0;
        f32 duration = 1;
        std::vector<s32> hit;
        std::vector<s32> blessed; ///< what its caster's class perk has reached
        bool struck = false;      ///< whether it has harmed anything
    };
    std::vector<PotionBurst> m_potions;
    static void enchantChests(PotionBurst& burst, f32 radius, f32 power, const Targets& targets);
    void healFrom(s32 owner, f32 harm, const Vec3& at, std::span<PlayerRuntime> players,
                  const Targets& targets);
    static constexpr s32 kHealingLevel = 75;   ///< from here magic heals as it harms
    static constexpr f32 kHealingShare = 0.1f; ///< of the harm, to the caster
    static constexpr f32 kHealingShareALevel = 0.016f;
    static constexpr f32 kHealingOthers = 0.5f; ///< of that, to the others near
    static constexpr std::string_view kHealingEffect = "LEVELUP_GRE"; ///< FX_HEALENEMY
    static void bless(PotionBurst& burst, f32 radius, std::span<const PlayerRuntime> players,
                      const Targets& targets);
    /** A DMG_HEAL hit feeds its dealer the target family's credited damage, not health loss. */
    void healHit(s32 owner, u32 flags, f32 credit, const Vec3& at, std::span<PlayerRuntime> players,
                 const Targets& targets);
    std::vector<MissileTarget> projectileTargets(const Targets& targets) const;
    /** What a thrown weapon or a burst can strike: the targets and shootable switches. */
    std::vector<MissileTarget> strikeTargets(const Targets& targets) const;
    /** Shared melee/ranged acquisition geometry; excludes cover without removing its collider. */
    std::vector<MissileTarget> acquisitionTargets(const Targets& targets) const;
    /** A same-step swarm contact precedes a fresh weighted acquisition query. */
    static std::optional<MissileTarget>
    contactEnemy(const PlayerActor& actor, const Targets& targets, const Vec3& facing, f32 range);
    static std::optional<MissileTarget> contactSeed(const PlayerActor& actor,
                                                    const Targets& targets, bool stationary);
    static MeleeSelection::Target meleeCandidate(const PlayerActor& actor,
                                                 const MissileTarget& body);
    static MeleeSelection::Contact meleeContact(const PlayerActor& actor, const MissileTarget& body,
                                                const Targets& targets);
    struct MeleeQuery {
        MeleeSelection::Snapshot selection;
        std::vector<MissileTarget> bodies;
    };
    MeleeQuery meleeQuery(const PlayerActor& actor, const Targets& targets,
                          std::optional<Vec3> facing) const;
    struct MeleeTarget {
        MissileTarget body;
        f32 distance = 0;
    };
    std::optional<MeleeTarget> meleeTarget(const PlayerActor& actor, const Targets& targets,
                                           const Vec3& facing, f32 reach) const;
    static MeleeSense senseOf(const PlayerActor& actor, bool held, const MeleeTarget& selected);
    /** Player melee is a forward-cone fallback, never displacing an enemy or item. */
    static std::optional<MissileTarget>
    meleePlayer(const PlayerActor& actor, const Targets& targets, f32 reach, const Vec3& facing);
    /** Sets off the switch a strike hit, unless it was only gas; true when it was one. */
    bool strikeSwitch(s32 id, u32 flags);
    /** What magic leaves alone: every barrel but one that holds something, the walls, the
     * rocks and the switches. */
    static bool immuneToMagic(s32 id, const Targets& targets);
    void shootPotion(const MissileImpact& impact, std::span<PlayerRuntime> players,
                     const Targets& targets);
    static ItemArchive* moveEffectsOf(usize index, std::span<PlayerRuntime> players);
    ItemArchive* moveEffectArchive(usize index, std::span<PlayerRuntime> players,
                                   std::string_view tree);
    /** One strike of `index`'s class, set going where the character stands, or at `at`. */
    void fireStrike(usize index, s32 strikeIndex, std::span<PlayerRuntime> players,
                    std::optional<Vec3> at = std::nullopt,
                    std::optional<usize> occurrence = std::nullopt);
    struct StrikeEvents {
        std::function<std::optional<u32>(ItemArchive&, std::string_view, const Vec3&,
                                         const EffectTrees::Setting&)>
            tree;
        std::function<std::optional<u32>(ItemArchive&, const ParticleDescriptor&, u32)> particle;
        std::function<void(usize, usize, std::string_view)> sound;
        std::function<void()> shake;
    };
    std::optional<TurboEmission::Strike> prepareStrike(usize index, s32 row,
                                                       std::span<PlayerRuntime> players,
                                                       std::optional<Vec3> at = std::nullopt);
    bool applyStrike(usize index, const TurboEmission::Strike& prepared,
                     std::span<PlayerRuntime> players, std::optional<usize> occurrence,
                     const StrikeEvents& events, std::optional<Vec3> at = std::nullopt);
    u32 startParticles(usize index, const TurboEmission::Particle& emission, u32 parent,
                       std::span<PlayerRuntime> players, const StrikeEvents& events);
    void stepTurbo(usize index, s32 ticks, f32 seconds, std::span<PlayerRuntime> players,
                   const TurboMove::Events& events,
                   const std::function<void(std::string_view)>& cry);
    void updateParticles(std::span<PlayerRuntime> players);
    void updateMoveAttachments(std::span<PlayerRuntime> players);
    /** Stationary effects parented to the caster by DoPlyrSfx, with their area carrier. */
    struct MoveAttachment {
        usize actor = 0;
        u32 effect = 0;
        u32 strike = 0;
        Mat4 local{1};
        bool bodyParent = false; ///< SFXX 1 bypasses the first animated node
    };
    std::vector<MoveAttachment> m_moveAttachments;
    struct ParticleEffect {
        usize actor = 0;
        u32 effect = 0;
        u32 parent = 0;
        std::string node;
        Vec3 offset{0.0f};
    };
    std::vector<ParticleEffect> m_particleEffects;
    void presentStrikeHit(usize index, const MoveStrike& row, const Vec3& at,
                          std::span<PlayerRuntime> players, bool sound = true);
    static bool isCreature(s32 id);
    static constexpr s32 kEnemyTargetBase = 1000;
    static constexpr s32 kGeneratorTargetBase = 2000;
    static constexpr s32 kCritterTargetBase = 3000;
    static constexpr s32 kBossTargetBase = 4000;
    static constexpr s32 kSafeRockTargetBase = 5000;
    static constexpr s32 kWallTargetBase = 6000;
    static constexpr s32 kSwitchTargetBase = 7000; ///< the triggers that are shot
    static constexpr s32 kItemStopBase = 7500;     ///< chests, gates: stop a weapon unharmed
    static constexpr s32 kChestTargetBase = 9000;  ///< chests, which only magic reaches
    static constexpr s32 kPotionTargetBase = 8000; ///< the bottles lying about
    static constexpr s32 kStatueTargetBase = 9500; ///< the statues a blow wakes
    static constexpr s32 kPlayerTargetBase = 10000;
    static constexpr f32 kShotMagicShare = 0.8f; ///< of a shot bottle's magic (lbl_80346310)
    static constexpr f32 kPotionDamage = 40.0f;  ///< start_magic's

    std::optional<Resources> m_resources;
    MoveStrikes m_strikes;
    /** The effect that goes along with a strike that flies. */
    struct StrikeEffect {
        u32 strike = 0;
        u32 effect = 0;
    };
    std::vector<StrikeEffect> m_strikeEffects;
    /** Whose strike a number is, and which of their class's, for what it shows on a hit. */
    struct StrikeSource {
        u32 strike = 0;
        usize actor = 0;
        s32 row = -1;
        struct Contact {
            s32 target = -1;
            f32 remaining = 0;
        };
        std::vector<Contact> contacts;
        MultiplayerMode multiplayer = MultiplayerMode::Normal;
        u32 damageType = 0; ///< reflection can spend a launched strike's pass-through
    };
    std::vector<StrikeSource> m_strikeSources;
    std::vector<StrikeHit> advanceStrikes(f32 seconds, std::span<PlayerRuntime> players);
    std::vector<MissileTarget> strikeContacts(const StrikeHit& hit, const StrikeSource& source,
                                              const Targets& targets) const;
    void dispatchStrikeHit(const StrikeHit& hit, std::span<PlayerRuntime> players,
                           const Targets& targets);
    void finishStrikes();
    /** A potion's magic ringing a character: it goes about with them and harms what it
     * touches, every so often, until it is spent. */
    static constexpr f32 kShieldHitAgain = 0.25f; ///< ProcessEffects' per-target fxhittime
    struct PotionShield {
        usize actor = 0;
        u32 flags = 0;
        u32 effect = 0;
        f32 radius = 0.0f;
        f32 damage = 0.0f;
        f32 secondsLeft = 0.0f;
        /** What it has struck and how long until it may again (fxhittime per owner). */
        struct Mark {
            s32 kind = 0;
            s32 id = 0;
            f32 wait = 0.0f;
        };
        std::vector<Mark> marks;
        std::vector<s32> blessed; ///< what its bearer's class perk has reached
    };
    std::vector<PotionShield> m_shields;
};
} // namespace gdl::game
