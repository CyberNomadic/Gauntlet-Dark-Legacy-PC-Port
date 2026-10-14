#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "game/enemies/EnemyMissiles.h"
#include "game/screens/PartyDamage.h"
#include "game/world/EffectTrees.h"

namespace gdl::game {
class LevelOpponents;

/** Native enemy-missile landing presentation. Resolution selects only the
 * already-bound shared weapon archive; it does not load art or start effects. */
struct EnemyMissileEmission {
    ItemArchive* archive = nullptr;
    std::string_view tree;
    Vec3 position{0};
    EffectTrees::Setting setting;
    std::string_view sound;
    f32 level = 0; ///< native 0..255 volume before nearest-player attenuation

    static EnemyMissileEmission resolve(const EnemyMissileHit&, ItemArchive& weapons);
    std::optional<EffectTrees::Prepared> prepare(std::span<const EffectTrees::PoolResource>,
                                                 std::span<TextureSet* const> lenders,
                                                 const RenderDevice&) const;
};

/** The native landing phase AFTER swarm missile flight and BEFORE enemy cues
 * and great-creature AI. The encompassing complete combat transaction owns
 * undo: a refusal can consume hits and mutate health/effects, but produces no
 * external audio, renderer/resource work, or committed output. */
class OrdinaryEnemyMissilePhase {
public:
    static constexpr usize kMaxImpacts = 4096;
    enum class Refusal { None, Binding, Hit, WorldExplosion, Resources, Damage };
    struct Sound {
        std::string name;
        Vec3 position{0};
        f32 volume = 0;
        bool operator==(const Sound&) const = default;
    };
    struct Result {
        Refusal refusal = Refusal::None;
        usize impacts = 0;
        std::vector<Sound> sounds;
        std::vector<PartyDamage::Output> damage;
        bool advanced() const { return refusal == Refusal::None; }
    };

    /** Additional domain admission after complete incoming checkpoint
     * validation. Does not authorize bombs, blast rings, piercing/reflection,
     * safe-rock damage, or pickup/world explosion chains. World-owner ancestor
     * effects are checked against the actual bound level during advance. */
    static bool supported(const EnemyMissiles::Checkpoint&);
    /** Requires the caller's successful complete pool/binding validation and
     * same-tick OpponentSimulation::stageCombat admission. That boundary owns
     * the exclusion of unrelated rock/blast queues; this phase consumes only
     * landings. The caller must rewind every owner and discard this Result on
     * refusal, including when an earlier hit in the batch already changed it. */
    static Result advance(LevelOpponents&, std::span<PlayerRuntime>, PlayerHealth&,
                          const ClassDataSet&, const PartyDamage::Context&,
                          std::span<const EffectTrees::PoolResource>);
};
} // namespace gdl::game
