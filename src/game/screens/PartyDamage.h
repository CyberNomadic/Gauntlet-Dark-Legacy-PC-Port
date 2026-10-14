#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/enemies/Enemies.h"
#include "game/screens/PlayerHealth.h"

namespace gdl::game {

/** Shared native damage dispatch. The scene still owns its ordinary output
 * callbacks; a private damage phase records them without playing audio,
 * creating effects, or touching controller hardware. */
class PartyDamage {
public:
    struct Context {
        bool tower = false;
        f32 damageScale = 1;
        bool bossEncounter = false;
        bool cutscene = false;
        bool lastRealmLevel = false;
        s32 realm = -1;
        f32 playSeconds = 0;
        bool valid() const;
    };
    struct EnemyImpact {
        /** A present empty name still selects QuietBlow in the original path. */
        std::optional<std::string> sound;
        HurtKind kind = HurtKind::Blow;
        PlayerImpact impact;
    };

    static std::optional<EnemyImpact> enemy(const EnemyBlow& blow, const PlayerRuntime& player,
                                            std::span<const LevelEnemy> roster, s32 bossType = -1);
    static void hurt(PlayerHealth& health, PlayerRuntime& player, f32 damage, HurtKind kind,
                     bool directed, const Context& context, const PlayerHealth::Events& events,
                     const PlayerImpact& impact = {}, const ClassStats* stats = nullptr);
    /** Mutates the native guard ledger, returning the accepted BLOCKFX duration.
     * Resource availability is the caller's concern; the native wrapper calls
     * this only after its existing bound-services check. */
    static std::optional<f32> block(PlayerRuntime& player, f32 taken, f32 left);

    enum class OutputKind { Sound, Cry, Named, Rumble, Block };
    struct Output {
        OutputKind kind = OutputKind::Sound;
        s32 player = -1; ///< native input-slot identity, not party-vector index
        std::string name;
        f32 seconds = 0; ///< narration wait or accepted block duration
        s32 frames = 0;  ///< native controller feedback frames
        bool operator==(const Output&) const = default;
    };
    enum class Status { Applied, Invalid, Unsupported };
    struct Result {
        Status status = Status::Invalid;
        std::vector<Output> outputs;
    };
    static constexpr usize kMaxBlows = 1024;
    /** Runs the same phase under a CALLER-OWNED transaction. Unsupported can
     * leave partial gameplay mutations: the caller must restore its full saved
     * tick before continuing. No callbacks are dispatched, and refused outputs
     * are discarded. Unlike replayBlows this never commits child restores, so
     * an encompassing prepared combat/world undo remains valid. */
    static Result stageBlows(std::span<const EnemyBlow> blows, std::span<PlayerRuntime> players,
                             PlayerHealth& health, const ClassDataSet& classes,
                             std::span<const LevelEnemy> roster, const Context& context,
                             s32 bossType = -1);
    /** A transaction for this DAMAGE phase, not the surrounding world tick.
     * The caller supplies the entire native-ordered party and already-produced
     * ordered blows. Missing/undamageable recipients are consumed as in native
     * play. Refusal restores every participant and the party-wide pain RNG;
     * outputs are returned only after acceptance. The caller must separately
     * restore the opponent queues and any preceding motion if it rejects the
     * encompassing tick. New help, ward and lethal interactions are not implemented here;
     * a lesson already suppressed by native history does not need a HUD mutation.
     * Uses already-bound class/figure resources; never loads or uploads them. */
    static Result replayBlows(std::span<const EnemyBlow> blows, std::span<PlayerRuntime> players,
                              PlayerHealth& health, const ClassDataSet& classes,
                              std::span<const LevelEnemy> roster, const Context& context,
                              s32 bossType = -1);
};

} // namespace gdl::game
