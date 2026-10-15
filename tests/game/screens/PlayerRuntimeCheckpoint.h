#pragma once

#include <memory>
#include <optional>

#include "game/screens/PlayerRuntime.h"

namespace gdl::game {

/** Test-only local gameplay state for one already-bound participant. This is not a
 * network packet or a save-file projection: every class's progress and the
 * original inventory slot ordering are retained. External sound/effect owners
 * and rendering-only figure clocks are deliberately outside this transaction. */
class PlayerRuntimeCheckpoint {
public:
    struct State {
        static constexpr usize kMaxRecords = 65535;
        s32 player = 0;
        CharacterSave save;
        PlayerActor::Checkpoint actor;
        bool cursorAiming = false;
        std::optional<Vec3> meleeFacing;
        std::optional<PlayerRuntime::AttackStep> attackStep;
        PlayerRuntime::Presentation previous;
        PlayerCapture::Checkpoint capture;
        ComboState combo;
        PlayerTransport::Checkpoint transport;
        MikeyDecoy::Checkpoint mikey;
        std::optional<PlayerFigure::GameplayCheckpoint> figure;
        std::optional<usize> slot;
        CharacterSave entrySave;
        std::optional<LevelResults::Checkpoint> resultsCheckpoint;
        s32 levelKills = 0;
        PlayerLife life = PlayerLife::Standing;
        bool towerPrompt = false;
        bool departed = false;
        f32 healthFraction = 0;
        f32 painOwed = 0;
        s32 hitSoundGap = 0;
        s32 heartbeatTicks = 0;
        std::optional<Vec3> fixtureSpot;
        f32 surfaceGap = 0;
        s32 hitFlashTicks = 0;
        s32 itTicks = 0;
        PlayerDeed gesture = PlayerDeed::None;
        f32 gagSeconds = 0;
        std::vector<PlayerRuntime::ShockGap> shockGaps;
        s32 deathHeld = -1;
        s32 deathDrainTicks = 0;
        bool deathHaloHeard = false;
        PlayerDeed reaction = PlayerDeed::None;
        Knockback::Checkpoint knockback;
        TurboMeter::Checkpoint turbo;
        bool pojoTurbo = false;
        std::vector<s32> helpHeard;
        TurboMove::Checkpoint move;
        std::vector<usize> rammed;
        f32 blockLeft = 0;
        bool blocked = false;
        f32 cloudGap = 0;
        f32 breathGap = 0;
        f32 effectGap = 0;
        PlayerRuntime::Floor floor;
        HeadGem::Checkpoint gem;
        f32 glow = 0;
        WeaponGlow::Checkpoint weaponGlow;
        MeleeStreak::Checkpoint streak;
        ExitWait::Checkpoint exitWait;
        s32 nameTicks = 0;
        u32 wornSpecial = 0;

        bool valid() const;
        bool operator==(const State& other) const;
    };

    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class PlayerRuntimeCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    /** Runtime and trusted class data must outlive the owner and its preparations.
     * The owner cannot rebind to another party member or resource roster. */
    PlayerRuntimeCheckpoint(PlayerRuntime& runtime, const ClassStats* stats);
    PlayerRuntimeCheckpoint(const PlayerRuntimeCheckpoint&) = delete;
    PlayerRuntimeCheckpoint& operator=(const PlayerRuntimeCheckpoint&) = delete;
    PlayerRuntimeCheckpoint(PlayerRuntimeCheckpoint&&) = delete;
    PlayerRuntimeCheckpoint& operator=(PlayerRuntimeCheckpoint&&) = delete;
    ~PlayerRuntimeCheckpoint();

    /** Refuses uncaptured external handles, Mikey's private figure owner, and
     * companion bindings whose gameplay attachment pose is not captured. */
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation-free and callback-free after successful canCommit, provided
     * no other participant mutates these bound owners between the two calls. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    bool supported() const;
    PlayerRuntime& m_runtime;
    const ClassStats* m_stats;
    const s32 m_player;
    const s32 m_character;
    const s32 m_color;
    u64 m_generation = 0;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};

} // namespace gdl::game
