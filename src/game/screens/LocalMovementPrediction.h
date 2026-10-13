#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "engine/world/WorldCollision.h"
#include "engine/world/WorldScene.h"

#include "game/netplay/MatchSession.h"
#include "game/netplay/PlayerTurboCheckpoint.h"
#include "game/netplay/RollbackInputHistory.h"
#include "game/players/Knockback.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/TurboMove.h"
#include "game/screens/PotionSimulation.h"

namespace gdl::game {
class PlayerActor;
struct ClassStats;
/** Selective rollback of locally owned movement, ordinary ranged/guard actions,
 * host-approved melee/turbo/charge actions in bounded static regions and confirmed grounded
 * reactions. Shield equipment admits only native locomotion; its attack, contact,
 * and damage behavior remains authoritative.
 * Restore a completed host tick and replay retained input,
 * including concealed gaps. World AI, damage, inventory, projectiles and one-shot
 * effects remain authoritative; replay observes releases without dispatching them.
 * Remote players retain snapshot interpolation. This is not world rollback. */
class LocalMovementPrediction {
public:
    static constexpr usize kHistory = RollbackInputHistory::kHistory;
    static constexpr u64 kStaleTicks = 30;
    struct Metrics {
        u64 samples = 0;
        u64 corrections = 0;
        u64 cuts = 0;
        u64 stalled = 0;
        f32 largestCorrection = 0;
        f32 resolvedDeadlineMs = 0; ///< sampled input to observed post-step deadline, not execution
        usize pending = 0;
    };
    /** A native throw boundary reached during this complete replay, not an event
     * to execute. Identity uses epoch, outer seat, grant, tick, generation and kind;
     * the latest held-input seed is not the command that originated the action. */
    struct Release {
        u64 tick = 0;
        u64 generation = 0;
        SeatMotion motion;
        bool strong = false;
        bool cursorAiming = false;
        f32 attackSeconds = 0;
        u8 shots = 1;        ///< native fan size at this release, not the later displayed inventory
        u32 weaponFlags = 0; ///< complete certified native weapon mask at this release
        u32 emission = 0;  ///< zero for ordinary throws; native chain occurrence for a turbo volley
        Vec3 direction{0}; ///< turbo volley bearing, independent of the character's facing
        bool targetResolved = false; ///< release-local acquisition, including an empty result
        std::optional<Vec3> target;  ///< not the authority checkpoint's earlier aim
        bool aimingKnown(bool ranged) const {
            return emission != 0 || cursorAiming || targetResolved || ranged;
        }
    };
    struct Replay {
        /** Completed native steps, including exact action cursors and observed
         * strike occurrences. Visual consumers may replay attachments from these
         * values; they must never execute the strike's gameplay callbacks. */
        struct Frame {
            u64 tick = 0;
            SeatMotion motion;
            PlayerAnimator::Checkpoint animation;
            std::optional<PlayerAnimator::TransitionPose> transitionPose;
            std::vector<TurboMove::Strike> strikes;
        };
        u64 endTick = 0;
        std::vector<Release> releases;
        std::vector<Frame> frames;
        std::vector<PotionSimulation::Release> potions;
    };
    enum class ReplayStatus { Remote, Inactive, Active, Complete, Unsupported };
    struct ReplayActor {
        ReplayStatus status = ReplayStatus::Inactive;
        u64 tick = 0; ///< age of this state; completed/unsupported seats do not invent later input
        std::optional<SeatMotion> motion;
        std::optional<PlayerAnimator::Checkpoint> animation;
    };
    struct ReplayTick {
        u64 tick = 0;
        std::array<ReplayActor, InputCommand::kSeats> actors;
    };
    struct GeneratorContact {
        PlayerLocomotionState::Identity identity;
        Obstacle box;
    };
    /** Complete current native rosters, in ascending pool-slot order. These
     * replace only tagged swarm/generator observations, not anonymous bodies,
     * fixtures or party members. They do not extend a movement certificate. */
    struct DynamicContacts {
        std::vector<PlayerLocomotionState::Body> swarm;
        std::vector<GeneratorContact> generators;
    };
    struct ContactOverlay {
        u64 epoch = 0;
        u64 tick = 0; ///< upcoming player step; geometry is observed before that step
        std::array<std::optional<DynamicContacts>, InputCommand::kSeats> seats;
    };
    /** Synchronous private-world coordination. Each pass begins at the exact
     * authority checkpoint, visits completed local actor ticks in ascending
     * order, then ends once. A fresh correction can perform two independent
     * passes; begin must restore the consumer's private world each time.
     * beforeTick observes all actors before any advances and may supply fresh
     * private-world contacts for active local seats. An absent seat retains its
     * captured contacts; a present empty roster removes only tagged contacts.
     * Rejecting any callback or overlay discards every local speculative result in that
     * pass. Remote/unsupported/completed seats are explicit observations, never
     * synthesized input. Callbacks must not mutate this predictor or authority. */
    struct ReplayCallbacks {
        std::function<bool(const CombatSnapshot&)> begin;
        std::function<bool(const ReplayTick&, ContactOverlay&)> beforeTick;
        std::function<bool(const ReplayTick&)> afterTick;
        std::function<void(bool committed)> end;
    };
    void begin(const MatchContext& context, u8 localPeer, const WorldCollision& collision,
               const WorldScene* scene = nullptr, const WorldLayout* layout = nullptr);
    void clear();
    /** The tree and optional native class data are borrowed until clear/rebinding.
     * Turbo replay requires data for the exact roster class, not its base costume. */
    bool setPlayer(u8 seat, const TreeInfo& tree, const ClassStats* stats = nullptr,
                   s32 character = -1);
    /** Trusted native companion tree and body scale from this seat's loaded roster. */
    bool setFireShield(u8 seat, const TreeInfo& tree, f32 bodyScale);
    /** Exactly once per application simulation tick, even if no command was sent.
     * Never replay unsent/coalesced input or an already resolved simulation tick. */
    void update(const CombatSnapshot* authority, std::span<const InputCommand> sent,
                const ReplayCallbacks& callbacks = {});
    void present(CombatSnapshot& displayed, f32 blend) const;
    /** Complete native local replay, replaced on update. Null on any unsafe or
     * unsupported path. Releases contain only ticks after the host checkpoint. */
    const Replay* replayed(u8 seat) const;
    const Metrics& metrics() const { return m_metrics; }
    u64 epoch() const { return m_context.epoch; }

private:
    struct Seat {
        const TreeInfo* tree = nullptr;
        const ClassStats* stats = nullptr;
        s32 character = -1;
        const TreeInfo* fireShieldTree = nullptr;
        f32 bodyScale = 1;
        bool companionReplayed = false;
        std::optional<CompanionState> fireShield;
        std::optional<SeatMotion> pose;
        std::optional<SeatMotion> previous;
        std::optional<PlayerAnimator::Checkpoint> checkpoint;
        std::optional<PlayerAnimator::TransitionPose> transitionPose;
        std::optional<Knockback::Checkpoint> knockback;
        std::optional<PlayerFloorState> floor;
        std::optional<SceneGeometry> geometry;
        std::vector<u32> geometryObjects;
        std::optional<PlayerShadowState> shadow;
        std::optional<PlayerTurboState> turbo;
        PlayerDeed reaction = PlayerDeed::None;
        std::optional<Replay> replay;
        bool authoritativeOnly = false;
        CombatAnimation animation;
        CombatAnimation previousAnimation;
        Vec3 correction{0};
        Vec3 previousCorrection{0};
    };
    struct Continuation;
    std::unique_ptr<Continuation> prepare(usize index);
    void replayAll(const ReplayCallbacks& callbacks);
    bool currentAuthority(usize index) const;
    void authoritative(usize index);
    static bool supported(const InputCommand& input, const PlayerCombatState& state,
                          bool turboReplay = false, bool potionReplay = false);
    MatchContext m_context;
    u8 m_local = 0;
    u64 m_clock = 0;
    u64 m_receivedAt = 0;
    std::optional<CombatSnapshot> m_authority;
    WorldCollision m_collision;
    std::unique_ptr<WorldScene> m_geometry;
    const WorldLayout* m_layout = nullptr;
    RollbackInputHistory m_history;
    std::array<Seat, InputCommand::kSeats> m_seats;
    Metrics m_metrics;
};
} // namespace gdl::game
