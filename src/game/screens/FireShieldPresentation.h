#pragma once

#include "engine/world/TextureAnimator.h"

#include "game/netplay/CombatSnapshot.h"

namespace gdl::game {
/** Native fire-shield companion clocks only. Its trusted tree is preloaded;
 * replay cannot bind resources, emit sounds, burn targets or change inventory. */
class FireShieldPresentation {
public:
    bool restore(const TreeInfo& tree, const std::optional<CompanionState>& state);
    /** Selection precedes the player's action update; companion clocks follow it. */
    bool step(bool preStepRunning, f32 seconds, f32 alpha);
    std::optional<CompanionState> sample(const Mat4& body) const;

private:
    const TreeInfo* m_tree = nullptr;
    AnimationPlayer m_animation;
    TextureAnimator m_textures;
    bool m_shown = false;
    f32 m_alpha = 1;
};
} // namespace gdl::game
