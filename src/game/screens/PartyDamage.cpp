#include "game/screens/PartyDamage.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <memory>

#include "game/combat/Damage.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/PartyHelpAdmission.h"
#include "game/screens/PlayerRuntimeCheckpoint.h"

namespace gdl::game {
namespace {
constexpr f32 kBlockLessonAfter = 60;
constexpr f32 kBlockWorth = 2;
constexpr f32 kBlockPerDamage = 0.01f;
constexpr f32 kBlockLeast = 0.333f;
constexpr f32 kBlockMost = 1;
constexpr f32 kMagnitude = 1'000'000;

bool bounded(f32 value) {
    return std::isfinite(value) && std::abs(value) <= kMagnitude;
}

bool validBlow(const EnemyBlow& blow) {
    return blow.player >= 0 && blow.player < 4 && blow.kind >= 0 && blow.kind < kSwarmKindCount &&
           blow.tier >= 1 && blow.tier <= 3 && bounded(blow.damage) && blow.damage >= 0 &&
           bounded(blow.direction.x) && bounded(blow.direction.y) && bounded(blow.direction.z) &&
           blow.ward <= EnemyMeleeWard::HealthVamp;
}

/** Prepared before the first native mutation. Native damage cannot replace a
 * bound figure or class; rollback therefore commits without allocation. This
 * also unwinds if recording a string/cue throws during the native callbacks. */
class Undo {
public:
    explicit Undo(PlayerHealth& health) : m_health(health), m_random(health.checkpoint()) {}
    ~Undo() {
        if (!m_armed) {
            return;
        }
        for (const auto& player : m_players) {
            if (!player.owner->canCommit(player.prepared)) {
                std::terminate();
            }
        }
        for (auto& player : m_players) {
            if (!player.owner->commitRestore(std::move(player.prepared))) {
                std::terminate();
            }
        }
        if (!m_health.restore(m_random)) {
            std::terminate();
        }
    }
    bool add(PlayerRuntime& runtime, const ClassStats* stats) {
        auto owner = std::make_unique<PlayerRuntimeCheckpoint>(runtime, stats);
        const auto state = owner->checkpoint();
        if (!state) {
            return false;
        }
        auto prepared = owner->prepareRestore(*state);
        if (!prepared) {
            return false;
        }
        m_players.push_back({std::move(owner), std::move(*prepared)});
        return true;
    }
    void arm() { m_armed = true; }
    void accept() { m_armed = false; }

private:
    struct Player {
        std::unique_ptr<PlayerRuntimeCheckpoint> owner;
        PlayerRuntimeCheckpoint::Prepared prepared;
    };
    PlayerHealth& m_health;
    PlayerHealth::Checkpoint m_random;
    std::vector<Player> m_players;
    bool m_armed = false;
};
} // namespace

bool PartyDamage::Context::valid() const {
    return bounded(damageScale) && damageScale >= 0 && bounded(playSeconds) && playSeconds >= 0 &&
           (!lastRealmLevel || (realm >= 0 && realm <= LevelRecord::kMostRealm));
}

std::optional<PartyDamage::EnemyImpact> PartyDamage::enemy(const EnemyBlow& blow,
                                                           const PlayerRuntime& player,
                                                           std::span<const LevelEnemy> roster,
                                                           s32 bossType) {
    if (player.actor.player() != blow.player || !PlayerHealth::canBeDamaged(player)) {
        return std::nullopt;
    }
    auto sound = EnemyFeedback::meleeSound(blow.kind, blow.tier, roster, bossType);
    const HurtKind kind = sound ? HurtKind::QuietBlow : HurtKind::Blow;
    return EnemyImpact{std::move(sound), kind, {blow.flags, blow.direction}};
}

void PartyDamage::hurt(PlayerHealth& health, PlayerRuntime& player, f32 damage, HurtKind kind,
                       bool directed, const Context& context, const PlayerHealth::Events& events,
                       const PlayerImpact& impact, const ClassStats* stats) {
    if (context.cutscene) {
        return;
    }
    const bool stood = player.life == PlayerLife::Standing;
    auto dispatch = events;
    dispatch.learnBlock = [&] {
        if (context.playSeconds > kBlockLessonAfter && events.learnBlock) {
            events.learnBlock();
        }
    };
    health.hurt(player, damage, kind, directed, context.tower, context.damageScale, dispatch,
                impact, context.bossEncounter, stats);
    if (stood && player.life == PlayerLife::Dying && context.lastRealmLevel) {
        player.entrySave.progress().levels.recordBossDeath(context.realm);
    }
}

std::optional<f32> PartyDamage::block(PlayerRuntime& player, f32 taken, f32 left) {
    if (player.blockLeft > 0 || taken <= kBlockWorth) {
        return std::nullopt;
    }
    const f32 shown = std::clamp(kBlockPerDamage * left, kBlockLeast, kBlockMost);
    player.blockLeft = shown;
    player.blocked = true;
    return shown;
}

PartyDamage::Result PartyDamage::replayBlows(std::span<const EnemyBlow> blows,
                                             std::span<PlayerRuntime> players, PlayerHealth& health,
                                             const ClassDataSet& classes,
                                             std::span<const LevelEnemy> roster,
                                             const Context& context, s32 bossType) {
    if (blows.size() > kMaxBlows || players.size() > 4 || !context.valid() ||
        !std::ranges::all_of(blows, validBlow)) {
        return {};
    }
    Undo undo(health);
    u32 occupied = 0;
    for (auto& player : players) {
        const s32 slot = player.actor.player();
        const auto* stats = classes.stats(player.actor.save().character);
        if (slot < 0 || slot >= 4 || (occupied & (1U << slot)) != 0 || stats == nullptr) {
            return {};
        }
        occupied |= 1U << slot;
        if (!undo.add(player, stats)) {
            return {Status::Unsupported, {}};
        }
    }
    undo.arm();
    auto result = stageBlows(blows, players, health, classes, roster, context, bossType);
    if (result.status == Status::Applied) {
        undo.accept();
    }
    return result;
}

PartyDamage::Result PartyDamage::stageBlows(std::span<const EnemyBlow> blows,
                                            std::span<PlayerRuntime> players, PlayerHealth& health,
                                            const ClassDataSet& classes,
                                            std::span<const LevelEnemy> roster,
                                            const Context& context, s32 bossType) {
    if (blows.size() > kMaxBlows || players.size() > 4 || !context.valid() ||
        !std::ranges::all_of(blows, validBlow)) {
        return {};
    }
    u32 occupied = 0;
    for (const auto& player : players) {
        const s32 slot = player.actor.player();
        if (slot < 0 || slot >= 4 || (occupied & (1U << slot)) != 0 ||
            classes.stats(player.actor.save().character) == nullptr) {
            return {};
        }
        occupied |= 1U << slot;
    }
    Result result{Status::Applied, {}};
    for (const auto& blow : blows) {
        // Ordinary native swarm melee only generates low and knock-back flags.
        // Other effect kinds require their own owner/phase rather than treating
        // an arbitrary damage packet as a newly certified swarm interaction.
        if (blow.ward != EnemyMeleeWard::None ||
            (blow.flags & ~(Damage::kLow | PlayerImpact::kKnockBack)) != 0) {
            return {Status::Unsupported, {}};
        }
        for (auto& player : players) {
            const auto impact = enemy(blow, player, roster, bossType);
            if (!impact) {
                continue;
            }
            const s32 slot = player.actor.player();
            if (impact->sound) {
                result.outputs.push_back({OutputKind::Sound, slot, *impact->sound});
            }
            auto hit = stageHit(player, players, health, classes, blow.damage, impact->kind, true,
                                context, impact->impact);
            if (hit.status != Status::Applied) {
                return {hit.status, {}};
            }
            result.outputs.insert(result.outputs.end(),
                                  std::make_move_iterator(hit.outputs.begin()),
                                  std::make_move_iterator(hit.outputs.end()));
        }
    }
    return result;
}

PartyDamage::Result PartyDamage::stageHit(PlayerRuntime& player,
                                          std::span<const PlayerRuntime> party,
                                          PlayerHealth& health, const ClassDataSet& classes,
                                          f32 amount, HurtKind kind, bool directed,
                                          const Context& context, const PlayerImpact& impact) {
    const s32 slot = player.actor.player();
    const auto* stats = classes.stats(player.actor.save().character);
    if (slot < 0 || slot >= 4 || stats == nullptr || party.size() > 4 || !context.valid() ||
        !bounded(amount) || amount < 0 || !bounded(impact.direction.x) ||
        !bounded(impact.direction.y) || !bounded(impact.direction.z)) {
        return {};
    }
    Result result{Status::Applied, {}};
    bool learns = false;
    const PlayerHealth::Events events{
        .block =
            [&](f32 taken, f32 left) {
                if (const auto duration = block(player, taken, left)) {
                    result.outputs.push_back({OutputKind::Block, slot, {}, *duration});
                }
            },
        .sound =
            [&](std::string_view name) {
                result.outputs.push_back({OutputKind::Sound, slot, std::string{name}});
            },
        .cry =
            [&](std::string_view name) {
                result.outputs.push_back({OutputKind::Cry, slot, std::string{name}});
            },
        .named =
            [&](std::string_view name, f32 wait) {
                result.outputs.push_back({OutputKind::Named, slot, std::string{name}, wait});
            },
        .learnBlock =
            [&] {
                learns |= !PartyHelpAdmission::suppressedForPlayer(HelpMessages::kLearnBlock, slot,
                                                                   party);
            },
        .vibrate =
            [&](s32 frames) {
                result.outputs.push_back({OutputKind::Rumble, slot, {}, 0, frames});
            },
        .logDeaths = false};
    hurt(health, player, amount, kind, directed, context, events, impact, stats);
    if (learns || player.life != PlayerLife::Standing) {
        return {Status::Unsupported, {}};
    }
    return result;
}

} // namespace gdl::game
