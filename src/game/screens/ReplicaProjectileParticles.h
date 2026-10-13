#pragma once

#include "engine/world/TreeParticles.h"

#include "game/netplay/CombatSnapshot.h"

namespace gdl::game {
/** Native visual-only particle replay. No world, damage, audio or inventory is borrowed. */
class ReplicaProjectileParticles {
public:
    bool update(const ProjectileState& state, const TreeInfo& tree, const TreeParticles& prototype);
    const TreeParticles& particles() const { return m_particles; }
    TreeParticles& particles() { return m_particles; }

private:
    std::optional<ProjectileState::Particles> m_previous;
    const TreeInfo* m_tree = nullptr;
    u32 m_continuity = 0;
    Mat4 m_terminal{1};
    PlayerMissiles::FlightCheckpoint m_flight;
    AnimationPlayer m_animation;
    TreePose m_pose;
    TreeParticles m_particles;
};
} // namespace gdl::game
