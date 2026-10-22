#include "game/party/PartyLighting.h"

#include "game/effects/EffectTrees.h"
#include "game/enemies/LevelOpponents.h"
#include "game/party/PartyFigures.h"

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
