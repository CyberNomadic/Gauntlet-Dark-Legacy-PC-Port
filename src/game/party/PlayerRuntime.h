#pragma once
#include <memory>
#include <optional>
#include <vector>

#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/characters/LevelResults.h"
#include "game/effects/BodyGlow.h"
#include "game/effects/HeadGem.h"
#include "game/effects/WeaponGlow.h"
#include "game/party/MikeyFigure.h"
#include "game/party/PlayerFigure.h"
#include "game/players/ComboMove.h"
#include "game/players/ExitWait.h"
#include "game/players/Knockback.h"
#include "game/players/MeleeStreak.h"
#include "game/players/MikeyDecoy.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerCapture.h"
#include "game/players/PlayerTransport.h"
#include "game/players/TurboMeter.h"
#include "game/players/TurboMove.h"

namespace gdl {
class NativeStateDigest;
}

namespace gdl::game {
enum class PlayerLife : u8 { Standing, Dying, InTower };

/** Value-owned participant state. Copying this value includes the complete character
 * record (not just the active class), pending combat decisions and movement contacts.
 * It owns no renderer, mixer, resource binding or callback. The scene's local history
 * can copy it directly, without the cross-process checkpoint restore protocol.
 *
 * This is one component, not a complete scene snapshot: action poses, projectiles,
 * effect stores, the party-wide RNG and the world must be rewound alongside it. */
struct PlayerSimulation {
    PlayerActor actor;
    bool cursorAiming = false; ///< manual facing must not be redirected by controller assistance
    /** This movement step's requested attack direction, which may differ from the
     * body yaw locked by a quick swing. Shared by acquisition and contact. */
    std::optional<Vec3> meleeFacing;
    struct AttackStep {
        Vec3 from{0};
        Vec3 to{0};
        bool operator==(const AttackStep&) const = default;
    };
    /** This invocation's intended collision-centre segment before creature correction,
     * never a retained target ID. Empty outside motion; queries then test standing contact. */
    std::optional<AttackStep> attackStep;
    PlayerCapture capture;
    ComboState combo; ///< its side of a two-player combo, none outside one
    PlayerTransport transport;
    MikeyDecoy mikey;
    CharacterSave entrySave; ///< restored when a fallen character leaves the level
    std::optional<LevelResults::Checkpoint> resultsCheckpoint; ///< prior linked stages, not saved
    s32 levelKills = 0; ///< creatures and generators credited during this level
    PlayerLife life = PlayerLife::Standing;
    bool towerPrompt = false; ///< fallen outside the tower, asked to wait there or quit
    bool departed = false;    ///< quit the game from that prompt: out of the party
    /** Live health minus the rounded save/HUD value. Retail damage_player (800785CC)
     * retains floating-point health; rounding each blow loses small post-armor hits.
     * Whole-point pickups preserve this fraction; new level runtimes start from the save. */
    f32 healthFraction = 0.0f;
    f32 painOwed = 0.0f;                   ///< accumulated damage not yet answered by a cry
    s32 hitSoundGap = 0;                   ///< ticks before another impact sound
    s32 heartbeatTicks = 0;                ///< ticks before the low-health heart beats again
    std::optional<Vec3> fixtureSpot;       ///< where the fixtures last saw it, for its step
    f32 surfaceGap = 0.0f;                 ///< seconds before a harmful surface can hurt it again
    s32 hitFlashTicks = 0;                 ///< two 30 Hz frames of the white damage skin
    s32 itTicks = 0;                       ///< since IT tagged this player; none when not it
    PlayerDeed gesture = PlayerDeed::None; ///< a pickup's or bad food's, played when free
    f32 gagSeconds = 0.0f; ///< retching at the stick let go for this long (field_898)
    /** A creature the lightning shield shocked, and how long before it may again. */
    struct ShockGap {
        s32 target = -1;
        f32 seconds = 0.0f;
        bool operator==(const ShockGap&) const = default;
    };
    std::vector<ShockGap> shockGaps;
    s32 deathHeld = -1;      ///< the Death a halo holds, by enemy slot; none when -1
    s32 deathDrainTicks = 0; ///< ticks until the next drain; separation cannot reset its cadence
    bool deathHaloHeard = false; ///< activation cue latched until Anti-Death is unequipped
    PlayerDeed reaction = PlayerDeed::None; ///< hit or gesture requested for the next update
    Knockback knockback;                    ///< the pushes of this frame's hits, and the slide
    TurboMeter turbo;
    bool pojoTurbo = false;     ///< this update requests breath from the meter, not an item charge
    std::vector<s32> helpHeard; ///< since the character was loaded, distinct from saved help
    TurboMove move;
    std::vector<usize> rammed; ///< barrels already hit by the current charge
    f32 blockLeft = 0.0f;      ///< seconds before another block effect
    bool blocked = false;      ///< its guard has shown a block this level (hud_flags 0x2000)
    f32 cloudGap = 0.0f;       ///< seconds before gas can harm this participant again
    f32 breathGap = 0.0f; ///< the great ones' shared hit gap (fxhittime): breath and blows alike
    f32 effectGap = 0.0f; ///< shared attached-area damage gate, independent of breath
    /** The floor it stood on at the end of the last frame (floor_name2), and where a moving
     * one then was: the body rides it as though parented to it. */
    struct Floor {
        s32 object = -1;
        u32 flags = 0;
        std::optional<Mat4> placement;
        bool operator==(const Floor&) const = default;
    };
    Floor floor;
    HeadGem gem;         ///< a hand of death's or health vampire's gem on the head
    MeleeStreak streak;  ///< close blows landed, towards the narrator's praise
    ExitWait exitWait;   ///< standing still on an exit for the rest of the party
    s32 nameTicks = 0;   ///< while over nought the name shows over the head (name_timer)
    u32 wornSpecial = 0; ///< the special flags worn last update, for their endings (old_flags)
    /** Complete typed native value equality, including every class's save and
     * action/contact internals. Figure resources and PlayerRuntime presentation
     * bindings are deliberately outside this component. */
    bool operator==(const PlayerSimulation&) const = default;
};

void visitNativeState(NativeStateDigest& digest, const PlayerSimulation& value);
void visitNativeState(NativeStateDigest& digest, const PlayerSimulation::AttackStep& value);
void visitNativeState(NativeStateDigest& digest, const PlayerSimulation::ShockGap& value);
void visitNativeState(NativeStateDigest& digest, const PlayerSimulation::Floor& value);

/** Runtime bindings stay at their existing addresses while simulation values rewind.
 * The public base keeps native participant consumers on the same live fields: there
 * is no second, hand-maintained list of fields to capture and restore. */
struct PlayerRuntime : PlayerSimulation {
    using Simulation = PlayerSimulation;
    const Simulation& simulation() const { return *this; }
    Simulation& simulation() { return *this; }

    struct Presentation {
        Vec3 position{0};
        f32 yaw = 0;
        bool continuous = false;
        u64 animationRevision = 0;
        template <class D> friend void visitNativeState(D& digest, const Presentation& value) {
            digest.template fields<4>(value);
        }
    };
    Presentation previous; ///< preceding draw placement, never used for collision
    std::unique_ptr<MikeyFigure> mikeyFigure;
    std::unique_ptr<PlayerFigure> figure; ///< null when character assets are unavailable
    std::optional<usize> slot; ///< persistent save slot; never rewound by speculative gameplay
    u32 deathHeldEffect = 0;   ///< presentation effect, not Death's gameplay hold
    SoundHandle deathHeldCry = kNoSound;
    SoundHandle deathHeldSuck = kNoSound;
    BodyGlow glow;
    WeaponGlow weaponGlow;
};

} // namespace gdl::game
