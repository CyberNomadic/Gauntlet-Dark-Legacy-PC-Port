#pragma once

#include <array>
#include <map>
#include <memory>
#include <vector>

#include "game/netplay/TurboEffectCheckpoint.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/TurboEffectSimulation.h"
#include "game/world/PlayerFigure.h"

namespace gdl::game {
/** Post-authority native B/C tree presentation. Only complete, preloaded tree
 * chains can start; standalone particle RNG and already-confirmed effect tails
 * remain authoritative. There are no combat, audio or camera-shake callbacks. */
class LocalTurboEffectPrediction {
public:
    static constexpr usize kMaxOrigins = TurboEffectSimulation::kMaxEmissions;
    void begin(const MatchContext& context, u8 localPeer);
    void clear();
    bool bind(u8 seat, const TurboEffectResources& resources, const PlayerFigure& figure,
              const ItemArchive& weapons, f32 bodyScale);
    void update(const CombatSnapshot* authority, const LocalMovementPrediction& movement);
    /** Replaces only recognized local effect identities. Latest confirmed meshes
     * bridge the shared interpolation delay without replaying their particle RNG. */
    bool present(CombatSnapshot& displayed) const;
    void draw(RenderDevice& device, const CombatSnapshot& displayed, const Mat4& clip,
              const WorldLighting& lighting, const CameraFrame& camera, f32 blend) const;
    const TurboEffectSimulation* simulation(u8 seat) const;

private:
    struct Seat {
        const TurboEffectResources* resources = nullptr;
        const PlayerFigure* figure = nullptr;
        const ItemArchive* weapons = nullptr;
        f32 bodyScale = 1;
        std::unique_ptr<TurboEffectSimulation> replay;
        std::optional<SeatMotion> endpoint;
    };
    bool replay(Seat& seat, u8 index, const CombatSnapshot& authority,
                const LocalMovementPrediction::Replay& frames);
    bool owned(const EffectIdentity& origin) const;
    bool compatible(const CombatSnapshot& displayed) const;
    MatchContext m_context;
    u8 m_local = 0;
    std::optional<u64> m_tick;
    std::array<Seat, InputCommand::kSeats> m_seats;
    std::map<EffectIdentity, u64> m_observed; ///< last host sighting; absent tails stay tombstoned
    std::vector<ProjectileState> m_confirmed;
};
} // namespace gdl::game
