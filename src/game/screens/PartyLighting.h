#pragma once

#include <span>
#include <vector>

#include "engine/world/WorldLighting.h"

namespace gdl::game {
class EffectTrees;
class LevelOpponents;
struct PlayerRuntime;

/** The native draw-time point-light rebuild. This derives presentation from
 * current complete owners; it does not reuse a baseline's draw-cache list. */
class PartyLighting {
public:
    static void gather(std::vector<PointLight>& lights, const EffectTrees& effects,
                       const LevelOpponents& opponents, std::span<const PlayerRuntime> party,
                       const LevelInfo* level);
};
} // namespace gdl::game
