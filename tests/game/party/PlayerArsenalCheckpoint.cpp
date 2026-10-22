#include "game/party/PlayerArsenalCheckpoint.h"

#include <unordered_set>
#include <utility>

namespace gdl::game {
bool PlayerArsenalCheckpoint::State::valid() const {
    return validAggregate() && missiles.valid();
}
bool PlayerArsenalCheckpoint::State::validAggregate() const {
    if (nextPotionKind < 1 || nextPotionKind > 4 || casts.size() > kMaxCasts ||
        (bound != missiles.visualsBound) || (!bound && !casts.empty())) {
        return false;
    }
    std::unordered_set<u32> ids;
    for (const auto& cast : casts) {
        if (cast.owner < 0 || cast.owner >= 4 || cast.effect == 0 ||
            !ids.insert(cast.effect).second) {
            return false;
        }
    }
    return true;
}
struct PlayerArsenalCheckpoint::Prepared::Data {
    const PlayerArsenal* owner = nullptr;
    std::weak_ptr<const u8> lifetime;
    u64 generation = 0;
    s32 nextPotionKind = 1;
    std::vector<PlayerArsenal::CastEffect> casts;
    std::optional<PlayerMissiles::Prepared> missiles;
};
PlayerArsenalCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
PlayerArsenalCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
PlayerArsenalCheckpoint::Prepared&
PlayerArsenalCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
PlayerArsenalCheckpoint::Prepared::~Prepared() = default;

std::optional<PlayerArsenalCheckpoint::State>
PlayerArsenalCheckpoint::capture(const PlayerArsenal& owner,
                                 std::span<const PlayerMissiles::Resource> resources,
                                 std::span<const EffectTrees::PoolResource> missileEffects) {
    if (owner.m_sim.casts.size() > State::kMaxCasts) {
        return std::nullopt;
    }
    auto missiles = owner.m_missiles.checkpoint(resources, missileEffects);
    if (!missiles) {
        return std::nullopt;
    }
    State result;
    result.bound = owner.m_resources.has_value();
    result.nextPotionKind = owner.m_sim.nextPotionKind;
    result.missiles = std::move(*missiles);
    for (const auto& cast : owner.m_sim.casts) {
        result.casts.push_back({cast.owner, cast.effect});
    }
    // The missile owner has already validated the complete captured pool.
    return result.validAggregate() ? std::optional{std::move(result)} : std::nullopt;
}
std::optional<PlayerArsenalCheckpoint::Prepared>
PlayerArsenalCheckpoint::prepare(PlayerArsenal& owner, const State& state,
                                 std::span<const s32> players,
                                 const EffectTrees::PoolCheckpoint& sharedEffects,
                                 std::span<const PlayerMissiles::Resource> resources,
                                 std::span<const EffectTrees::PoolResource> missileEffects) {
    if (!state.valid() || !sharedEffects.valid() || players.size() > 4 ||
        state.bound != owner.m_resources.has_value() || owner.m_restoreGeneration == 0) {
        return std::nullopt;
    }
    std::unordered_set<s32> slots;
    for (const auto player : players) {
        if (player < 0 || player >= 4 || !slots.insert(player).second) {
            return std::nullopt;
        }
    }
    auto prepared = std::make_unique<Prepared::Data>();
    prepared->owner = &owner;
    prepared->lifetime = owner.m_restoreOwner;
    prepared->generation = owner.m_restoreGeneration;
    prepared->nextPotionKind = state.nextPotionKind;
    prepared->missiles = owner.m_missiles.prepareRestore(state.missiles, resources, missileEffects);
    if (!prepared->missiles) {
        return std::nullopt;
    }
    for (const auto& cast : state.casts) {
        // Native followCaster removes a stopped effect on its next update.
        if (!slots.contains(cast.owner) ||
            (sharedEffects.nextId != 0 && cast.effect >= sharedEffects.nextId)) {
            return std::nullopt;
        }
        prepared->casts.push_back({cast.owner, cast.effect});
    }
    return Prepared{std::move(prepared)};
}
bool PlayerArsenalCheckpoint::canCommit(const PlayerArsenal& owner,
                                        const Prepared& prepared) noexcept {
    return prepared.m_data && prepared.m_data->owner == &owner &&
           !prepared.m_data->lifetime.expired() &&
           prepared.m_data->lifetime.lock() == owner.m_restoreOwner &&
           owner.m_restoreGeneration != 0 &&
           prepared.m_data->generation == owner.m_restoreGeneration && prepared.m_data->missiles &&
           owner.m_missiles.canCommit(*prepared.m_data->missiles);
}
bool PlayerArsenalCheckpoint::commit(PlayerArsenal& owner, Prepared&& prepared) noexcept {
    if (!canCommit(owner, prepared)) {
        return false;
    }
    auto state = std::move(prepared.m_data);
    if (!owner.m_missiles.commitRestore(std::move(*state->missiles))) {
        return false;
    }
    owner.m_sim.casts.swap(state->casts);
    owner.m_sim.nextPotionKind = state->nextPotionKind;
    ++owner.m_restoreGeneration;
    return true;
}
bool PlayerArsenalCheckpoint::restore(PlayerArsenal& owner, const State& state,
                                      std::span<const s32> players,
                                      const EffectTrees::PoolCheckpoint& sharedEffects,
                                      std::span<const PlayerMissiles::Resource> resources,
                                      std::span<const EffectTrees::PoolResource> missileEffects) {
    auto prepared = prepare(owner, state, players, sharedEffects, resources, missileEffects);
    return prepared && commit(owner, std::move(*prepared));
}
} // namespace gdl::game
