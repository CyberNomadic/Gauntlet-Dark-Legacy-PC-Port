#pragma once

#include <array>
#include <map>
#include <optional>
#include <vector>

#include "game/netplay/PotionIdentity.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/ReplicaProjectiles.h"

namespace gdl::game {
/** Presentation-only bottle replay in a private native missile pool. Successful
 * local releases never spend saved potions, apply healing/damage, or dispatch
 * impact audio. The newest complete host bottle roster replaces older local
 * copies, including bottles that disappeared before the next snapshot. */
class LocalPotionPrediction {
public:
    void begin(const MatchContext& context, u8 localPeer, const WorldCollision& collision,
               const ItemArchive& weapons, const ProjectileResources& resources,
               const WorldScene* scene = nullptr, const WorldLayout* layout = nullptr);
    void clear();
    void update(const CombatSnapshot* authority, const LocalMovementPrediction& movement,
                std::span<const InputCommand> sent = {});
    void present(CombatSnapshot& displayed, f32 blend) const;
    usize count() const { return m_shots.size(); }

private:
    struct Shot {
        ProjectileState current;
        ProjectileState previous;
    };
    bool owned(const PotionIdentity& origin) const;
    bool remember(ProjectileState shot, const std::map<PotionIdentity, ProjectileState>& previous);
    MatchContext m_context;
    u8 m_local = 0;
    WorldCollision m_collision;
    std::unique_ptr<WorldScene> m_geometry;
    const WorldLayout* m_layout = nullptr;
    const ProjectileResources* m_resources = nullptr;
    std::array<u32, 5> m_bottles{};
    std::array<u64, InputCommand::kSeats> m_inputTicks{};
    std::optional<CombatSnapshot> m_authority;
    std::array<bool, InputCommand::kSeats> m_present{};
    std::vector<Shot> m_shots;
    std::map<PotionIdentity, u64> m_instances;
    u64 m_nextInstance = u64{3} << 62;
    u64 m_clock = 0;
    u64 m_receivedAt = 0;
};
} // namespace gdl::game
