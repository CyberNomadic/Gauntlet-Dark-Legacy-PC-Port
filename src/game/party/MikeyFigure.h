#pragma once

#include <random>

#include "engine/core/Types.h"

#include "game/effects/EffectTrees.h"
#include "game/level/ItemFigure.h"
#include "game/players/MikeyDecoy.h"

namespace gdl::game {
/** The dropped decoy's animated POWERUPS tree and its brief arrival sparkles. */
class MikeyFigure {
    friend void visitNativeState(NativeStateDigest& digest, const MikeyFigure& value);

public:
    void update(RenderDevice& device, ItemArchive& powerups, MikeyDecoy& decoy, f32 seconds,
                EffectTrees& effects);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera) const;
    bool shown() const { return m_shown && m_figure.hasFigure(); }
    void clear();
    /** The POWERUPS owner registers its archive before this retained figure. */
    bool registerNativeResources(NativeStateResources& resources) const;

private:
    ItemFigure m_figure;
    u32 m_generation = 0;
    bool m_shown = false;
    std::mt19937 m_random{0};
};
} // namespace gdl::game
