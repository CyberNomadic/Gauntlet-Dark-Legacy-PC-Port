#pragma once

#include "game/screens/OrdinaryCombatCheckpoint.h"
#include "game/screens/PartyDamage.h"
#include "game/screens/PartyMotion.h"

namespace gdl::game {

/** Executable ordinary combat phase under one complete party/population undo.
 * Starts at PartyMotion, AFTER the scene's world/powerup preparation. The caller
 * supplies the current gameplay camera; it is held observation, not a predicted
 * camera/world tick. Only static, fixture-free contacts and plain walking,
 * guard and nonlethal swarm melee are admitted. Attacks, pickups, world events,
 * lessons, death, and missile impacts refuse the complete phase.
 *
 * Sound/rumble/footstep and BLOCKFX presentation requests are returned only on
 * acceptance. Guard gameplay is exact; BLOCKFX allocation/attachment and figure
 * cosmetic clocks are deliberately deferred, not a full visual-world replay.
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
        bool valid() const;
    };
    struct Footstep {
        s32 player = -1;
        PartyMotion::Action foot = PartyMotion::Action::FirstFoot;
        Footing footing = Footing::Rock;
        bool operator==(const Footstep&) const = default;
    };
    enum class Status { Advanced, Invalid, Unsupported };
    struct Result {
        Status status = Status::Invalid;
        std::vector<CameraSubject> subjects;
        std::vector<Footstep> footsteps;
        std::vector<EnemyCue> enemyCues;
        std::optional<PlayerHealth::Heartbeat> heartbeat;
        std::vector<PartyDamage::Output> damage;
    };

    /** Every owner and trusted archive must outlive this view. The supplied
     * classes and fixture binding must match the checkpoint's native resources;
     * step verifies exact runtime addresses/order, not just matching player IDs. */
    OrdinaryCombatTick(OrdinaryCombatCheckpoint& checkpoint, const ClassDataSet& classes,
                       LevelFixtures& fixtures)
        : m_checkpoint(checkpoint), m_classes(classes), m_fixtures(fixtures) {}
    Result step(std::span<PlayerRuntime> party, std::span<const PlayInput> inputs,
                const Frame& frame);

private:
    bool bound(std::span<PlayerRuntime> party) const;
    bool environment() const;
    bool supported(const OrdinaryCombatCheckpoint::State& state,
                   std::span<const PlayerRuntime> party) const;
    OrdinaryCombatCheckpoint& m_checkpoint;
    const ClassDataSet& m_classes;
    LevelFixtures& m_fixtures;
};

} // namespace gdl::game
