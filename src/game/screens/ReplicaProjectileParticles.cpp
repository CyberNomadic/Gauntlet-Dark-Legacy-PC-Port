#include "game/screens/ReplicaProjectileParticles.h"

namespace gdl::game {
bool ReplicaProjectileParticles::update(const ProjectileState& state, const TreeInfo& tree,
                                        const TreeParticles& prototype) {
    if (!state.particles || !state.particles->valid()) {
        return false;
    }
    const auto& requested = *state.particles;
    const auto& history = requested.flight;
    if (prototype.field().size() == 0) {
        m_particles = prototype;
        m_previous = requested;
        m_tree = &tree;
        return true;
    }
    const bool continuous =
        m_previous && m_tree == &tree && m_continuity == state.continuity &&
        m_previous->character == requested.character && m_previous->origin == requested.origin &&
        m_previous->potion == requested.potion &&
        m_previous->potionOrigin == requested.potionOrigin &&
        m_previous->flight.birth == history.birth &&
        m_previous->flight.stepSeconds == history.stepSeconds &&
        m_previous->flight.totalSteps <= history.totalSteps &&
        m_previous->flight.activeSteps <= history.activeSteps &&
        (!m_previous->flight.retiring ||
         (history.retiring && m_previous->flight.activeSteps == history.activeSteps &&
          m_terminal == state.placement));
    if (!continuous) {
        m_previous.reset();
        m_tree = &tree;
        m_continuity = state.continuity;
        m_particles = prototype;
        m_flight = history.birth;
        m_animation = {};
        if (tree.sequences.empty()) {
            m_pose.rest(tree);
        } else {
            m_animation.start(tree.sequences.front(), 0);
            m_pose.evaluate(tree, 0, 0);
        }
        m_particles.setLocalScales(m_pose.poses());
    }
    const u32 completed = m_previous ? m_previous->flight.totalSteps : 0;
    for (u32 step = completed; step < history.totalSteps; ++step) {
        if (step < history.activeSteps) {
            PlayerMissiles flight;
            MissileLaunch launch;
            launch.potion = requested.potion;
            launch.spec = requested.potion != 0
                              ? &MissileSpec::potion()
                              : &MissileSpec::of(static_cast<s32>(requested.character));
            if (!flight.launch(launch)) {
                m_previous.reset();
                return false;
            }
            const u64 instance = flight.missile(0).instance;
            const bool restored = requested.potion != 0
                                      ? flight.restorePotionFlight(instance, m_flight)
                                      : flight.restoreFlight(instance, m_flight);
            if (!restored) {
                m_previous.reset();
                return false;
            }
            flight.update(history.stepSeconds, nullptr);
            if (flight.count() != 1) {
                m_previous.reset();
                return false;
            }
            m_flight = PlayerMissiles::flightCheckpoint(flight.missile(0));
            if (!tree.sequences.empty()) {
                m_animation.advance(history.stepSeconds, true);
                m_pose.evaluate(tree, m_animation.sequence(), m_animation.frame());
            }
            m_particles.setLocalScales(m_pose.poses());
            m_particles.step(history.stepSeconds, PlayerMissiles::transformOf(flight.missile(0)),
                             m_pose.matrices());
        } else {
            m_particles.stop();
            m_particles.step(history.stepSeconds, state.placement, m_pose.matrices());
        }
    }
    m_terminal = state.placement;
    m_previous = requested;
    return true;
}
} // namespace gdl::game
