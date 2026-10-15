#pragma once

#include <memory>

#include "game/screens/NativePlayDriver.h"
#include "game/screens/NativePlaySetup.h"
#include "game/screens/OnlineCompletion.h"
#include "game/screens/OnlineLevelEntry.h"

namespace gdl::game {
class AfterLevelScene;
class OnlineParty;
class OnlineSession;

/** Campaign owner for an agreed native-input session. Every peer owns and renders
 * the actual PlayScene; no replicated scene or speculative guest simulation is
 * involved. update() pumps transport once per 60 Hz application tick, even when
 * inputs are held, a local menu is open, or a terminal scene awaits departure.
 * The device, session, selection and borrowed context services must outlive us. */
class NativeOnlinePlay {
public:
    enum class Phase : u8 { Closed, Loading, Playing, Paused, Shopping, Finished, Failed };
    enum class Failure : u8 { None, Session, Scene, Assets, Input, Presentation, Party };

    NativeOnlinePlay();
    ~NativeOnlinePlay();
    NativeOnlinePlay(const NativeOnlinePlay&) = delete;
    NativeOnlinePlay& operator=(const NativeOnlinePlay&) = delete;
    NativeOnlinePlay(NativeOnlinePlay&&) = delete;
    NativeOnlinePlay& operator=(NativeOnlinePlay&&) = delete;

    /** setup is the same owner whose digest the session already agreed before
     * becoming Active. It remains alive across ordinary travel and secret-parent
     * suspension. This method does not replace the setup agreement handshake. */
    bool open(RenderDevice& device, const GameContext& context, OnlineSession& session,
              OnlineParty& selection, std::unique_ptr<NativePlaySetup> setup);
    void close();
    Phase update(const SessionInputs::Frame& devices);
    /** Captures only the specified local device's controls; never pauses the
     * shared scene or its clocks. -1 captures/releases all local devices. */
    bool pause(s32 device = -1);
    bool resume(s32 device = -1);
    /** Only completed shop/secret checkpoints, never the current live scene. */
    bool hasCompletedProgress() const;
    bool saveCompletedProgress();
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height, f64 seconds,
                f32 frameBlend);

    Phase phase() const;
    Failure failure() const;
    NativePlayDriver::Result outcome() const;
    const PlayScene* scene() const;
    u64 completedTick() const;
    OnlineLevelEntry::Phase entryPhase() const;
    bool entryVisible() const;
    const OnlineCompletion& completion() const;
    const AfterLevelScene* shopScene() const;

    static std::optional<u32> sceneId(const LevelCatalog& catalog, const LevelRef& level);
    static std::optional<LevelRef> levelOf(const LevelCatalog& catalog, u32 scene);

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};
} // namespace gdl::game
