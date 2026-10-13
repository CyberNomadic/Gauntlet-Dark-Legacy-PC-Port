#pragma once

#include "game/netplay/ClientClock.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/LocalPotionPrediction.h"
#include "game/screens/LocalProjectilePrediction.h"
#include "game/screens/LocalTurboEffectPrediction.h"
#include "game/screens/OnlineCombatReplay.h"
#include "game/screens/OnlineCompletion.h"
#include "game/screens/OnlineLevelEntry.h"
#include "game/screens/OnlineOpponentReplay.h"
#include "game/screens/OnlineParty.h"
#include "game/screens/PlayReplication.h"
#include "game/screens/ReplicaAudio.h"
#include "game/screens/ReplicaStage.h"

namespace gdl::game {
/** Scene owner for an admitted online party. The host alone constructs a
 * PlayScene; a guest owns only native artwork and checkpoint presentation.
 * update() pumps the session once per 60 Hz application tick, including pause.
 * render() advances only presentation. Post-level transactions are host-ordered;
 * save I/O remains in the application's local-only checkpoint callback.
 * The session, selection, device and context dependencies must outlive us. */
class OnlinePlay {
public:
    enum class Phase : u8 { Closed, Loading, Playing, Paused, Shopping, Finished, Failed };
    enum class Failure : u8 {
        None,
        Session,
        Scene,
        Assets,
        Input,
        Capture,
        Presentation,
        UnsupportedTravel,
        Party
    };
    OnlinePlay();
    ~OnlinePlay();
    OnlinePlay(const OnlinePlay&) = delete;
    OnlinePlay& operator=(const OnlinePlay&) = delete;
    OnlinePlay(OnlinePlay&&) = delete;
    OnlinePlay& operator=(OnlinePlay&&) = delete;

    /** Initial scene: host supplies its locally resolved destination/options;
     * guests ignore those arguments and wait for the authenticated host scene ID.
     * Selection is frozen by OnlineSession before this call. */
    bool open(RenderDevice& device, const GameContext& context, OnlineSession& session,
              OnlineParty& selection, const LevelRef& initial = LevelRef::tower(),
              const PlayOptions& options = {});
    void close();
    Phase update(const SessionInputs::Frame& devices);
    /** Local menu input capture. A device pauses only its own character; -1 captures all
     * local devices. The shared simulation and other machines are never paused. */
    bool pause(s32 device = -1);
    bool resume(s32 device = -1);
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height, f64 seconds,
                f32 frameBlend);
    Phase phase() const {
        return m_phase == Phase::Playing && m_pausedDevices != 0 ? Phase::Paused : m_phase;
    }
    Failure failure() const { return m_failure; }
    PlayReplication::Result outcome() const { return m_outcome; }
    const PlayScene* hostScene() const;
    const ReplicaView* replica() const { return m_replica.view(); }
    const CombatSnapshot* shown() const;
    const LocalMovementPrediction::Metrics& predictionMetrics() const {
        return m_prediction.metrics();
    }
    /** Diagnostic A/B control for local movement, action and projectile replay;
     * never changes host authority or input scheduling. */
    void setMovementPrediction(bool enabled) { m_predictMovement = enabled; }
    OnlineLevelEntry::Phase entryPhase() const { return m_entry.phase(); }
    bool entryVisible() const { return m_entry.visible() || m_completion.visible(); }
    const OnlineCompletion& completion() const { return m_completion; }
    const AfterLevelScene* shopScene() const { return m_shop.isOpen() ? &m_shop : nullptr; }

    /** Stable native realm/index encoding, not directory-enumeration order.
     * A packet can select only a playable entry in the trusted local catalog. */
    static std::optional<u32> sceneId(const LevelCatalog& catalog, const LevelRef& level);
    static std::optional<LevelRef> levelOf(const LevelCatalog& catalog, u32 scene);

private:
    struct Host;
    bool load(const SessionInputs::Frame& devices);
    bool travel();
    bool loadShop();
    Phase updateFrame(const SessionInputs::Frame& devices);
    Phase updateShop(const SessionInputs::Frame& devices);
    Phase fail(Failure reason);
    void quiet();
    void feedback();
    RenderDevice* m_device = nullptr;
    GameContext m_context;
    std::function<void(s32, s32, ControlFeedback)> m_vibrate;
    std::function<void(u16)> m_unlockClasses;
    OnlineSession* m_session = nullptr;
    OnlineParty* m_selection = nullptr;
    PlayOptions m_options;
    std::vector<PartyMember> m_travelMembers;
    std::unique_ptr<Host> m_host;
    std::unique_ptr<Host> m_parent;
    std::optional<LevelRef> m_level;
    std::optional<LevelRef> m_secretParent;
    ReplicaStage m_replica;
    ReplicaAudio m_audio;
    ClientClock m_clock;
    LocalMovementPrediction m_prediction;
    OnlineOpponentReplay m_opponentReplay;
    OnlineCombatReplay m_combatReplay;
    LocalProjectilePrediction m_projectilePrediction;
    LocalPotionPrediction m_potionPrediction;
    LocalTurboEffectPrediction m_effectPrediction;
    bool m_presentEffects = false;
    LatencyTelemetry::Time m_latencyReportAt;
    bool m_predictMovement = true;
    OnlineLevelEntry m_entry;
    OnlineCompletion m_completion;
    AfterLevelScene m_shop;
    GameConfig m_shopConfig;
    bool m_afterShop = false;
    bool m_saveAfterShop = false;
    u64 m_entryEpoch = 0;
    u64 m_completionEpoch = 0;
    Phase m_phase = Phase::Closed;
    Failure m_failure = Failure::None;
    PlayReplication::Result m_outcome = PlayReplication::Result::Held;
    u64 m_epoch = 0;
    u32 m_pausedDevices = 0;
};
} // namespace gdl::game
