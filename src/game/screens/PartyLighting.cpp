#include "game/screens/PartyLighting.h"

#include "game/screens/LevelOpponents.h"
#include "game/screens/PartyFigures.h"
#include "game/world/EffectTrees.h"

namespace gdl::game {
void PartyLighting::gather(std::vector<PointLight>& lights, const EffectTrees& effects,
                           const LevelOpponents& opponents, std::span<const PlayerRuntime> party,
                           const LevelInfo* level) {
    lights.clear();
    effects.lights(lights);
    opponents.lights(lights);
    PartyFigures::addLanterns(lights, party, level);
    if (lights.size() > WorldLighting::kMostPoints) {
        lights.resize(WorldLighting::kMostPoints);
    }
}
} // namespace gdl::game
