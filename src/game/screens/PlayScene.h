#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/world/AmbientDimmer.h"
#include "engine/world/WorldCamera.h"

#include "game/menu/CompassHud.h"
#include "game/netplay/FrameEventLedger.h"
#include "game/players/CharacterSave.h"
#include "game/players/ClassData.h"
#include "game/players/LevelResults.h"
#include "game/players/LevelWatch.h"
#include "game/players/Party.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/TurboMeter.h"
#include "game/screens/BossSequence.h"
#include "game/screens/ChallengeHud.h"
#include "game/screens/GameContext.h"
#include "game/screens/GameOver.h"
#include "game/screens/LevelArrivalPresentation.h"
#include "game/screens/LevelFixtures.h"
#include "game/screens/LevelMessages.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/PartyFigures.h"
#include "game/screens/PartyHud.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PartyNames.h"
#include "game/screens/PartyPickups.h"
#include "game/screens/PlayerAttacks.h"
#include "game/screens/PlayerHealth.h"
#include "game/screens/PortalDeparture.h"
#include "game/screens/RuneMeter.h"
#include "game/screens/SimulationObservation.h"
#include "game/screens/SumnerVisit.h"
#include "game/screens/SwitchCutscene.h"
#include "game/screens/TowerPromotion.h"
#include "game/screens/TowerRelics.h"
#include "game/screens/TowerWelcome.h"
#include "game/screens/TransitionScreen.h"
#include "game/screens/TriggerCues.h"
#include "game/world/BossCamera.h"
#include "game/world/CameraShake.h"
#include "game/world/EffectTrees.h"
#include "game/world/ExitPortals.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelTransporters.h"
#include "game/world/LevelWorld.h"
#include "game/world/NativeStateResources.h"
#include "game/world/PlayerArsenal.h"
#include "game/world/PlayerFigure.h"
#include "game/world/SecretChallenge.h"
#include "game/world/StartCamera.h"
#include "game/world/SumnerFigure.h"
#include "game/world/TowerAccess.h"
#include "game/world/TowerCamera.h"

namespace gdl::game {

/** An item dropped into the level by one of its item records' names. */
struct DroppedItem {
    std::string name;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/** How the tower may open other than as the game does, for tests and scenarios. */
struct PlayOptions {
    std::optional<Vec3> position;   ///< where the party stands instead of the entrance (the
                                    ///< start camera then does not ride in from its marker)
    std::optional<f32> yaw;         ///< the way it faces, instead of the entrance's
    std::optional<bool> welcome;    ///< whether Sumner welcomes it, else by its experience
    std::vector<DroppedItem> items; ///< dropped about the level once it opens
    u32 arrivalWorld = 0;           ///< the realm the party comes from, which picks the start point
                                    ///< it arrives at (none: the level's own entrance)
    bool arriving = false;          ///< the party comes through a portal: the transition picture is
                                    ///< up as the level opens, and clears
};

enum class PlayOutcome : u8 {
    Running,
    Leave,   ///< back to the title
    Travel,  ///< through an exit portal to destination()
    GameOver ///< the last death and defeat caption are complete: leave gameplay
};

/**
 * The tower with the party in it: each character stands at the entrance and walks under its
 * player's control, its body playing the class's sequences and its feet sounding on the stone,
 * the camera follows from the level's marker angles, the level's music loops from its stream,
 * Sumner idles at his lookout, and the status boxes line the bottom of the screen. The party
 * materialises held still, playing its entrance, under the level's title while the start
 * camera holds at the entrance marker and rides in to the follow camera. A party new to the
 * tower is then welcomed the way the original does it: his scroll of pages first, then his
 * gesture as the camera cuts to the crystals for five seconds, between black bars, with the
 * party held still. A player who steps up to Sumner is greeted and, two seconds on, handed
 * his scroll of hints, which holds play until it has burnt away; he waves them off after.
 * Holding the attack throws the character's weapon, again and again: the body winds up and
 * lets go where it stands, the weapon flies from the hand's side along its facing, and the
 * hand is empty until the throw is recovered from. A potion carried is used where the
 * character stands, its magic bursting about them, or thrown a little way to burst where it
 * lands. The powerups carried are named by the selector over the status box and, while worn,
 * quicken the character, spread their throws three or five ways, leave them half unseen or
 * a third larger. A level's chests and locked gates stand solid in the way until someone
 * against them spends a key, a chest then giving up what it held; its traps hurt whoever is
 * in them while they are out, by the level's own scales and the difficulty's; its barrels
 * break under thrown weapons and blasts, giving up what they held, blowing up or leaving a
 * cloud of gas. The original's help messages go up over a character the first time each
 * thing happens. A character whose health runs out falls and waits in the tower, its box
 * saying so; it comes back, as it entered the level, when the party returns there, and when
 * everyone has fallen the game-over sequence ends the session.
 */
class PlayScene {
    /** Live, value-owned control state. Pure gameplay helpers are copied directly,
     * not translated into a second checkpoint schema. Assets, devices, callbacks
     * and persistent save-slot identities stay in their existing owners. */
    struct Simulation {
        friend void visitNativeState(NativeStateDigest& digest, const Simulation& value) {
            digest.fields<23>(value);
        }
        BossCamera bossCamera;
        GameOver gameOver;
        SecretChallenge challenge;
        RuneMeter runeMeter;
        std::optional<usize> runeItem;
        bool secretTravel = false;
        Vec3 secretReturnPosition{0.0f};
        SwitchCutscene switchCutscene;
        PlayerHealth health;
        PartyPickups pickups;
        AmbientDimmer dimmer;
        LevelWatch levels;
        LevelRef destination;
        s32 refusedPortal = -1;  ///< the portal last found to lead nowhere, not to say so twice
        TowerAccess towerAccess; ///< what the tower opens to this party; empty elsewhere
        f32 playSeconds = 0.0f;
        f32 towerNoticeSeconds = 0.0f;
        CameraShake shake;
        TowerWelcome welcome;
        bool leaving = false;
        bool opponentsAdvanced = false;
        bool projectilesAdvanced = false;
        bool movementUpdated = false;

        bool operator==(const Simulation&) const = default;
    };

public:
    static constexpr s32 kPlayerCount = 4;
    static constexpr f32 kSpawnSpacing = 2.0f; ///< between party members at the entrance
    static constexpr s32 kSpawnTicks = LevelArrivalPresentation::kSpawnTicks;
    static constexpr s32 kCrystalTicks = TowerWelcome::kCrystalTicks;
    static constexpr f32 kGreetingSeconds = SumnerVisit::kGreetingSeconds;
    using Inputs = std::array<PlayInput, kPlayerCount>;

    using Intro = TowerWelcome::Intro;

    /** Brings the party into `world` (loading it when needed); false when the level or the
     * status boxes are not unpacked. */
    bool open(RenderDevice& device, const GameContext& context, LevelWorld& world,
              std::span<const PartyMember> party, const PlayOptions& options = {});
    void close();
    bool isOpen() const { return m_open; }
    u64 loadGeneration() const { return m_openGeneration; }

    PlayOutcome update(f64 deltaSeconds, const Inputs& inputs);
    struct RumbleOutput {
        s32 player = 0;
        s32 frames = 0;
        ControlFeedback feedback = ControlFeedback::Damage;
        bool operator==(const RumbleOutput&) const = default;
    };
    struct UnlockClassesOutput {
        u16 mask = 0;
        bool operator==(const UnlockClassesOutput&) const = default;
    };
    using OutputPayload = std::variant<RumbleOutput, UnlockClassesOutput>;
    using OutputLedger = FrameEventLedger<OutputPayload>;
    struct RecordedStep {
        PlayOutcome outcome = PlayOutcome::Running;
        OutputLedger::Journal journal;
        std::optional<SceneSoundEvents::Frame> audio;
    };
    /** Captures rumble and persistent class-unlock intents without invoking those
     * application callbacks. Native character rewards still change immediately.
     * Epoch/tick must be nonzero; invalid or nested calls throw before advancing.
     * The caller owns the fixed-roster epoch and supplies the completed tick.
     * Audio is journaled only after beginRecordedAudio; otherwise the offline
     * sound path remains immediate. Callers must explicitly select that boundary. */
    RecordedStep recordStep(u64 epoch, u64 tick, f64 deltaSeconds, const Inputs& inputs);
    /** Native audio gate, before the first snapshot of a loaded online epoch.
     * Rollback requires this dispatcher; offline play keeps immediate audio. */
    bool beginRecordedAudio(u64 epoch);
    /** Presentation boundary only, after abandoning history or confirming travel.
     * Ending recording invalidates snapshots from its audio epoch. */
    void endRecordedAudio();
    u64 recordedAudioEpoch() const { return m_recordedAudio.epoch(); }
    SceneSoundEvents::Result reconcileRecordedAudio(u64 firstTick,
                                                    std::span<const SceneSoundEvents::Frame> frames,
                                                    std::optional<u64> newHead = std::nullopt);
    SceneSoundEvents::Result confirmRecordedAudio(u64 allSeatsThrough);
    void dispatchRecordedAudio(const SceneSoundEvents::Result& result);
    void serviceRecordedAudio(f32 seconds);
    /** Dispatch a ledger-approved rumble Present or unlock Commit once. Online
     * callers must obtain Commit only from the all-seat-confirmed frontier.
     * Offline update dispatches its fresh intents immediately after the step. */
    bool dispatchOutput(const OutputLedger::Action& action) const;
    /** In-process history for this loaded scene. Copies the live value owners,
     * never serializes resources or restores across a load/party change. Output
     * and persistent writes must be gated by the caller before speculative use. */
    class LocalSnapshot {
    public:
        LocalSnapshot();
        ~LocalSnapshot();
        LocalSnapshot(LocalSnapshot&&) noexcept;
        LocalSnapshot& operator=(LocalSnapshot&&) noexcept;
        LocalSnapshot(const LocalSnapshot&) = delete;
        LocalSnapshot& operator=(const LocalSnapshot&) = delete;

    private:
        friend class PlayScene;
        struct Data;
        std::unique_ptr<Data> m_data;
    };
    bool saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    /** Read-only exact value equality of root control state, including hidden
     * helper clocks, coin identities and RNG. Not whole-scene equality: players,
     * world, other owners and outputs are outside this comparison. Uses normal
     * typed equality (including float ==), not byte/signed-zero comparison. */
    bool matchesLocalControl(const LocalSnapshot& saved) const;
    /** Resolve a local pointer against the currently presented Sumner menu.
     * Read-only: only the bounded semantic choice enters shared native input. */
    PlayInput captureNativeInput(s32 player, PlayInput input) const;
    /** Silence ambience and drain loops before the options menu stops gameplay updates. */
    void pauseGameplaySounds();
    /** The pause menu holds the level's sounds and speech where they are, and lets them go. */
    void holdSounds() { m_audio.holdSounds(); }
    void releaseSounds() { m_audio.releaseSounds(); }
    void render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth, f32 frameHeight,
                bool optionsOpen = false, f32 frameBlend = -1.0f);

    usize actorCount() const { return m_players.size(); }
    /** Read-only end-of-tick observation; callers must not retain the span across open/close. */
    std::span<const PlayerRuntime> participants() const { return m_players; }
    /** Periodic native agreement watchdog, without replica bindings or state
     * transfer. Observes party outcomes/motion, opponent pools and scene control;
     * it is not a complete serialization of every hidden simulation owner. */
    struct NativeDigestProfile {
        struct Sample {
            std::string_view owner;
            f64 microseconds = 0;
        };
        std::vector<Sample> samples;
    };
    /** Optional diagnostics only; the ordinary path does not read a clock. */
    std::optional<SimulationObservation::Digest>
    nativeAgreementDigest(NativeDigestProfile* profile = nullptr);
    /** Same schema over retained native values, without restoring or drawing. */
    std::optional<SimulationObservation::Digest> nativeLocalDigest(const LocalSnapshot& saved);
    /** Read-only native load-barrier preflight for message/hint resources whose
     * absence changes simulation holds. Does not change offline optional art. */
    bool nativePresentationReady() const;
    const LevelWorld* world() const { return m_world; }
    CameraView cameraView() const;
    /** Hard-cut rule used by local render interpolation. */
    bool cameraContinuous() const;
    u32 simulationTickRate() const {
        return m_context.config != nullptr ? m_context.config->timing.tickRate : 60;
    }
    /** The character driven by `player`, or null when that player is not in the party. */
    const PlayerActor* actor(s32 player) const;
    /** The body animation of `player`'s character, or null without a figure for it. */
    const PlayerAnimator* animator(s32 player) const;
    const TowerCamera& camera() const { return m_camera; }
    /** The camera the scene is seen through: the start camera while it holds and rides in,
     * the crystals during the welcome's cut, else the follow camera. */
    WorldCamera viewCamera() const;
    /** Horizontal world direction, zero in the cursor dead zone; never a world target. */
    std::optional<Vec3> cursorAim(Vec2 cursor, const Vec3& position) const;
    /** The boss fight's camera, used while a boss level's boss stands. */
    const BossCamera& bossCamera() const { return m_sim.bossCamera; }
    bool bossCameraOn() const;
    BossCameraSubject bossSubject() const;
    const StartCamera& startCamera() const { return m_arrival.camera(); }
    /** The music's voice, kNoSound while nothing plays. */
    SoundHandle music() const { return m_audio.music(); }
    const LevelSoundscape& soundscape() const { return m_audio; }
    /** Sumner's voice over the scroll he is reading, kNoSound while he is quiet. */
    SoundHandle voice() const { return m_audio.voice(); }
    /** The sound of the target that opened before the party most recently and is still
     * opening (a gate's force field humming as it thins, a lift, a gate), kNoSound otherwise. */
    SoundHandle fieldSound() const { return m_audio.fieldSound(); }
    bool exitFlameOn() const { return m_audio.exitFlameOn(); }
    Intro intro() const { return m_sim.welcome.intro(); }
    const ScrollBox& scroll() const { return m_messages.scroll(); }
    std::optional<LevelMessages::Look> scrollLook() const { return m_messages.look(); }
    const HintMenu& hints() const { return m_sumnerVisit.menu(); }
    const PlayerMissiles& missiles() const { return m_arsenal.missiles(); }
    PlayerArsenal::SoundCapture captureWeaponSounds(PlayerArsenal::SoundObserver observer) {
        return {&m_arsenal, std::move(observer)};
    }
    bool projectilesAdvanced() const { return m_sim.projectilesAdvanced; }
    const ExitPortals& portals() const { return m_portals; }
    const LevelTransporters& transporters() const { return m_transporters; }
    const PortalDeparture& departure() const { return m_departure; }
    const SwitchCutscene& switchCutscene() const { return m_sim.switchCutscene; }
    const LevelFixtures& fixtures() const { return m_fixtures; }
    const Chests& chests() const { return m_fixtures.chests(); }
    const LockedGates& gates() const { return m_fixtures.gates(); }
    const Traps& traps() const { return m_fixtures.traps(); }
    const Breakables& barrels() const { return m_fixtures.barrels(); }
    const SafeRocks& safeRocks() const { return m_fixtures.safeRocks(); }
    const Rubble& rubble() const { return m_fixtures.rubble(); }
    const LevelOpponents& opponents() const { return m_opponents; }
    const Enemies& enemies() const { return m_opponents.enemies(); }
    Enemies& enemies() { return m_opponents.enemies(); }
    const Generators& generators() const { return m_opponents.generators(); }
    Generators& generators() { return m_opponents.generators(); }
    const PlacedEnemies& placedEnemies() const { return m_opponents.placements(); }
    const Critters& critters() const { return m_opponents.critters(); }
    const CritterStatues& statues() const { return m_opponents.statues(); }
    Critters& critters() { return m_opponents.critters(); }
    const EnemyMissiles& enemyMissiles() const { return m_opponents.missiles(); }
    const Bosses& bosses() const { return m_opponents.bosses(); }
    Bosses& bosses() { return m_opponents.bosses(); }
    /** The boss's meter, when the level has one: its name and health for the HUD. */
    std::optional<BossView> bossView() const {
        return m_opponents.bosses().present() ? std::optional<BossView>(m_opponents.bosses().view())
                                              : std::nullopt;
    }
    const BossMeters& bossMeter() const { return m_opponents.meter(); }
    /** The wizard's visit once the boss has fallen. */
    const BossVictory& victory() const { return m_bossSequence.victory().state(); }
    /** The archive folder a character's figure was loaded from, for tests. */
    std::optional<std::filesystem::path> figureDirectory(usize index) const {
        return index < m_players.size() && m_players[index].figure != nullptr
                   ? std::optional<std::filesystem::path>(m_players[index].figure->directory())
                   : std::nullopt;
    }
    const HelpMessages& help() const { return m_hud.help(); }
    const PartyHud& hud() const { return m_hud; }
    bool cinematicBars() const;
    bool hudVisible() const { return !m_sim.gameOver.active() && !cinematicBars(); }
    const MoveStrikes& strikes() const { return m_attacks.strikes(); }
    const AmbientDimmer& dimmer() const { return m_sim.dimmer; }
    /** Gives `player`'s character experience won in play, which also feeds its turbo meter
     * (unless it is in the middle of a turbo move), as a kill does in the original. */
    void awardExperience(s32 player, s32 amount, bool kill = true);
    /** Harms `player`'s character as a blow, a burn, a piercing or gas would, for tests. */
    void harm(s32 player, f32 damage, HurtKind kind);
    /** What `player`'s status box shows. */
    StatusBoxView status(s32 player) const { return PartyHud::status(player, m_players); }
    /** The turbo meter of `player`'s character, or null when that player is not in. */
    const TurboMeter* turboMeter(s32 player) const;
    /** `player`'s record in this level, or null when that player is not in. */
    const PlayerRuntime* runtime(s32 player) const {
        for (const PlayerRuntime& runtime : m_players) {
            if (runtime.actor.player() == player) {
                return &runtime;
            }
        }
        return nullptr;
    }
    /** Whether `player`'s character has fallen (dying or gone to the tower). */
    bool fallen(s32 player) const;
    /** Hurts `player`'s character, as anything in the level does. */
    void hurtPlayer(s32 player, f32 damage, HurtKind kind, bool directed = false);
    /** A blast at `position`: hurts and breaks what is within `radius`. */
    void blast(const Vec3& position, f32 radius, f32 damage);
    const TransitionScreen& transition() const { return m_transition; }
    /** Whether the party has gone through a portal and the picture is coming up over it. */
    bool leaving() const { return m_sim.leaving; }
    bool canPause(s32 player) const;
    /** Whether `player`, not in the party, may join: in the tower, with nothing scripted
     * running (game_main's MG_PLAY, world 13, no options or scripted camera). */
    bool canJoin(s32 player) const;
    const GameOver& gameOver() const { return m_sim.gameOver; }
    /** Where the party is bound once update() has said Travel, and the realm it leaves. */
    const LevelRef& destination() const { return m_sim.destination; }
    bool secretTravel() const { return m_sim.secretTravel; }
    const SecretChallenge& challenge() const { return m_sim.challenge; }
    std::optional<ChallengeHud::Look> hourglassLook() const;
    /** Pause the parent stage while the party visits a secret challenge. */
    void suspendForChallenge();
    /** Return progress to the preserved stage without replacing its entry checkpoint. */
    void resumeFromChallenge(std::span<const PartyMember> party);
    /** The party as it stands, with all it has gathered, for the next level. */
    std::vector<PartyMember> party() const;
    /** `party` (a snapshot of this scene's) as it leaves a level it gives up: everyone as they
     * came in, keeping only what they were taught and the slots they are kept in (kill_player,
     * then PlayerRestoreState in the tower). */
    std::vector<PartyMember> abandonedParty(std::span<const PartyMember> party) const;
    /** Quitting the game plays the defeat caption and voice (MG_OVER), after which update
     * returns GameOver. */
    void startGameOver();
    /** Associates a manual save with the live participant without restarting the level. */
    void setSaveSlot(s32 player, std::optional<usize> slot);
    std::vector<LevelResults> levelResults() const;
    const EffectTrees& effects() const { return m_effects; }
    /** The powerup selector over `player`'s box. */
    const PowerupSelector& selector(s32 player) const { return m_hud.selector(player); }
    /** How large a character is drawn: an ogre, one grown by a powerup, one of level 99. */
    const SumnerHints& hintTexts() const { return m_sumnerVisit.texts(); }
    const PickupHud& pickups() const { return m_hud.pickups(); }
    const RuneMeter& runeMeter() const { return m_sim.runeMeter; }
    const AmbientSounds& ambience() const { return m_audio.ambience(); }
    /** How far Sumner's beam of light has come up, 0 to 1. */
    /** Whether the party is still materialising: held under the level's title until the start
     * camera has ridden in, or for the effect's life when there is no start camera. */
    bool spawning() const { return m_arrival.active(); }
    MissileStreak weaponStreak(const PlayerActor& actor) { return m_arsenal.weaponStreak(actor); }
    std::optional<f32> weaponTextureFrame(const PlayerFigure& figure) const {
        return m_arsenal.weaponTextureFrame(figure);
    }
    std::optional<PlayerArsenal::WeaponRider> weaponRider(const PlayerActor& actor,
                                                          PlayerFigure& figure) const {
        return m_arsenal.weaponRider(actor, figure);
    }
    const LevelArrivalPresentation& arrival() const { return m_arrival; }
    usize spawnEffectCount() const { return m_arrival.effectCount(); }
    /** The folder a character's figure came from, when it loaded. */
    std::optional<std::filesystem::path> figureDirectory(s32 player) const;
    /** Whether a character's figure carries its weapon. */
    bool weaponHeld(s32 player) const;
    /** The folder holding a character's figure at their level: the costume tier of ten
     * levels (`BLU00` for levels 1 to 9, `BLU10` for 10 to 19, and so on) when it is
     * unpacked, else the untiered costume. */
    static std::filesystem::path costumeDirectory(const std::filesystem::path& unpackedRoot,
                                                  const CharacterSave& save);
    const SumnerFigure& sumner() const { return m_sumner; }
    const TowerPromotion& promotion() const { return m_promotion; }
    const TowerRelics& towerRelics() const { return m_towerRelics; }
    bool showingRelicCeremony() const { return relicCeremonyOn(); }
    s32 familiarTier(s32 player) const;

private:
    friend class SimulationObserver; // test-only read access, no restoration interface
    u64 m_openGeneration = 0;
    Simulation m_sim;
    struct OutputFrame {
        u64 epoch = 0;
        u64 tick = 0;
        OutputLedger::Journal* journal = nullptr;
    };
    OutputFrame* m_outputFrame = nullptr; ///< call-scoped collector, never snapshot state
    void emitOutput(OutputPayload payload);
    PlayOutcome advanceNative(f64 deltaSeconds, const Inputs& inputs);
    SimulationObservation::Digest simulationControlDigest() const;
    SimulationObservation::Digest opponentFollowupDigest() const;
    bool registerNativeDigestResources();
    bool appendSceneDigest(NativeStateDigest& digest, const LocalSnapshot* saved,
                           NativeDigestProfile* profile = nullptr) const;
    NativeStateResources m_nativeDigestResources; ///< immutable bindings, outside local history
    /** The bodies' shadows, after all of the level's floors (some of which are translucent). */
    PartyFigures::Scene figureScene(f32 frameBlend = 1.0f);
    void drawShadows(RenderDevice& device, const Mat4& clip, const Vec3& eye, f32 frameBlend,
                     f32 opponentBlend);
    bool scriptedCamera() const;
    void watchOpponents();
    void gatherLights();
    void beginChallenge();
    void collectChallengeCoin(usize item);
    bool updateChallenge(f32 seconds);
    void spawnParty(std::span<const PartyMember> party, const PlayOptions& options);
    void throwWeapon(const PlayerActor& actor);
    u32 acceptedPlayers(const Inputs& inputs) const;
    std::vector<TriggerVisitor> visitors() const;
    PartyPickups::Services pickupServices();
    void praiseStreaks(f32 seconds);
    bool leaveBy(usize portal);
    void updateFixtures(s32 ticks, f32 seconds);
    void updateTransporters(s32 ticks, f32 seconds, bool held);
    void updateSwitchCutscene(s32 ticks, f32 seconds);
    LevelFixtures::Events fixtureEvents();
    LevelOpponents::Events opponentEvents();
    PlayerAttacks::Targets attackTargets();
    void answerTowerPrompts(const Inputs& inputs);
    void hurt(usize index, f32 damage, HurtKind kind, bool directed = false,
              const PlayerImpact& impact = {});
    void updateEnemies(s32 ticks, f32 seconds);
    void strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer);
    void strikeGenerator(s32 id, f32 power, s32 byPlayer);
    void strikeCritter(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                       std::optional<Vec3> where = std::nullopt, bool close = false);
    void updateVictory(s32 ticks, f32 seconds);
    void settleBlasts();
    bool postHelp(s32 id, usize index, s32 number = -1);
    /** Answers levels gained with immediate health, effects and narration.
     * Appearance upgrades and title ceremonies wait for a tower return. */
    void updateLevels();
    void updatePromotion(s32 ticks, f32 seconds);
    bool ownsFigure(usize player, const PlayerFigure* figure) const;
    void activateFigure(usize player, PlayerFigure* figure);
    void upgradeFigure(usize player);
    void beginTowerRelics();
    void revealTowerRoutes();
    void updateTowerRelics(s32 ticks, f32 seconds);
    bool relicCeremonyOn() const;
    static constexpr f32 kLevelUpHealth = 100.0f;
    void sayWithName(usize index, std::string_view line, f32 wait);
    void updateHazardSurfaces(f32 seconds);
    void launchWeapon(usize index, const Vec3& direction, f32 scale, bool spreads);
    usize standingCount() const;
    bool isDown(usize index) const {
        return index < m_players.size() && m_players[index].life != PlayerLife::Standing;
    }
    /** Where the level finds a character: nowhere once it has fallen. */
    Vec3 presenceOf(usize index) const;
    void updateAmbience();
    void gatherRecordedAudio(const Inputs& inputs, PlayOutcome outcome);
    void beginSpawn(RenderDevice& device, const std::optional<WorldCamera>& marker);
    bool anyButton(const Inputs& inputs) const;
    bool openMessage(std::string_view name, usize page);
    void handleTriggerEvents();
    PartyMotion::Events motionEvents();
    void perform(usize index, PartyMotion::Action action);
    Vec3 limitStep(usize index, const Vec3& before, const Vec3& after) const;
    void updateSumnerVisit(f32 seconds);
    void updateHints(const Inputs& inputs, s32 ticks);

    RenderDevice* m_device = nullptr;
    GameContext m_context;
    LevelWorld* m_world = nullptr;
    CompassHud m_compass;
    ClassDataSet m_classes;
    PartyHud m_hud;
    Canvas m_canvas;
    TowerCamera m_camera;
    std::optional<Mat4> m_presentedClip;
    std::optional<Mat4> m_presentedCanvas; ///< local pointer mapping, never simulation state
    std::optional<WorldCamera> m_previousCamera;
    bool m_previousBossCamera = false;
    std::vector<PlayerRuntime> m_players;
    LevelSoundscape m_audio;
    SceneSoundEvents m_recordedAudio;
    ChallengeHud m_challengeHud;
    const Texture* m_runeFrame = nullptr;
    const Texture* m_runeColumn = nullptr;
    SumnerFigure m_sumner;
    TextureSet m_staticTextures;
    LevelMessages m_messages;
    PlayerArsenal m_arsenal;
    ExitPortals m_portals;
    LevelTransporters m_transporters;
    PlayerAttacks m_attacks;
    LevelFixtures m_fixtures;
    LevelOpponents m_opponents;
    TransitionScreen m_transition;
    EffectTrees m_effects;
    std::vector<PointLight> m_lights; ///< this frame's, gathered before it is drawn
    BossSequence m_bossSequence;
    SumnerVisit m_sumnerVisit;
    TowerPromotion m_promotion;
    TowerRelics m_towerRelics;
    LevelSoundscape::Speech m_relicVoice;
    LevelSoundscape::Speech m_promotionVoice;
    TriggerCues m_triggerCues;
    std::vector<std::unique_ptr<PlayerFigure>>
        m_promotionFigures; ///< retain borrowed voice/effect clips until close
    struct PromotionFigure {
        usize player = 0;
        s32 character = 0;
        s32 color = 0;
        s32 level = 0;
        PlayerFigure* figure = nullptr;
        PlayerFigure::LocalSnapshot initial;
    };
    std::vector<PromotionFigure> m_promotionBindings;

    ItemArchive m_weapons;              ///< shared weapon and effect assets
    LevelArrivalPresentation m_arrival; ///< borrows the weapons archive
    bool m_open = false;
    PortalDeparture m_departure;
    PartyFigures m_figures;
    PartyNames m_names;
};

} // namespace gdl::game
