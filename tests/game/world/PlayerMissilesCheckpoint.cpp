#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

#include "game/world/PlayerMissiles.h"

namespace gdl::game {
namespace {
bool finite(f32 value) {
    return std::isfinite(value) && std::abs(value) <= 1e6f;
}
bool finite(const Vec3& value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}
bool affine(const Mat4& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (!finite(value[column][row])) {
                return false;
            }
        }
    }
    return value[0][3] == 0 && value[1][3] == 0 && value[2][3] == 0 && value[3][3] == 1;
}
bool validRelease(const std::optional<MissileRelease>& release) {
    if (!release) {
        return true;
    }
    const u32 slot = release->emission >> 16;
    return release->character >= 0 && release->character <= 16 && release->generation != 0 &&
           release->ordinal < 5 &&
           (release->emission == 0 ||
            (slot >= 1 && slot <= 128 && !release->strong && release->ordinal == 0));
}
bool validPotion(const std::optional<MissilePotionRelease>& release) {
    return !release || release->generation != 0;
}
bool validSound(MissileWallSound sound) {
    return static_cast<u8>(sound) <= static_cast<u8>(MissileWallSound::Ricochet);
}
bool resourcesValid(std::span<const PlayerMissiles::Resource> resources) {
    if (resources.size() > PlayerMissiles::Checkpoint::kMaxResources) {
        return false;
    }
    std::unordered_set<u32> ids;
    for (usize index = 0; index < resources.size(); ++index) {
        const auto& resource = resources[index];
        if (resource.id == 0 || !ids.insert(resource.id).second || resource.spec == nullptr ||
            !finite(resource.spec->radius) || resource.spec->radius <= 0 ||
            !finite(resource.spec->spin) || !finite(resource.spec->weight)) {
            return false;
        }
        for (usize prior = 0; prior < index; ++prior) {
            if (resources[prior].spec == resource.spec &&
                resources[prior].model == resource.model &&
                resources[prior].streak == resource.streak) {
                return false;
            }
        }
    }
    return true;
}
const PlayerMissiles::Resource* findResource(std::span<const PlayerMissiles::Resource> resources,
                                             u32 id) {
    const auto found = std::ranges::find(resources, id, &PlayerMissiles::Resource::id);
    return found == resources.end() ? nullptr : &*found;
}
} // namespace

bool PlayerMissiles::Checkpoint::valid() const {
    if (missiles.size() > kMaxMissiles || history.size() > kMaxMissiles ||
        impacts.size() > kMaxEvents || !finite(ricochetIn) || ricochetIn < 0 || ricochetIn > 1 ||
        !visuals.valid() ||
        (!visualsBound && (!visuals.effects.empty() || !visuals.banks.empty()))) {
        return false;
    }
    const auto effectKnown = [&](u32 id) {
        return id == 0 || std::ranges::any_of(visuals.effects,
                                              [id](const auto& value) { return value.id == id; });
    };
    const auto usedEffect = [&](u32 id) {
        return id == 0 || (visualsBound && (visuals.nextId == 0 || id < visuals.nextId));
    };
    std::unordered_set<u64> instances;
    usize contacts = 0;
    for (const auto& flight : missiles) {
        if (flight.instance == 0 || !instances.insert(flight.instance).second ||
            (nextInstance != 0 && flight.instance >= nextInstance) || flight.continuity == 0 ||
            flight.owner < 0 || flight.owner >= 4 || !finite(flight.position) ||
            !finite(flight.velocity) || !finite(flight.tumble) || !finite(flight.age) ||
            flight.age < 0 || flight.age >= kLifeSeconds || !finite(flight.previousPosition) ||
            !finite(flight.previousVelocity) || !finite(flight.previousTumble) ||
            !finite(flight.previousAge) || flight.previousAge < 0 ||
            flight.previousAge >= kLifeSeconds || flight.potion < 0 || flight.potion > 4 ||
            !finite(flight.potency) || !finite(flight.damage) || !finite(flight.scale) ||
            flight.scale <= 0 || flight.resource == 0 || !validSound(flight.wallSound) ||
            !validRelease(flight.release) || !validPotion(flight.potionRelease) ||
            (flight.release && (flight.potion != 0 || flight.potionRelease)) ||
            (flight.potionRelease && flight.potion == 0) || !finite(flight.streakForward) ||
            static_cast<u8>(flight.multiplayer) > static_cast<u8>(MultiplayerMode::Hurt) ||
            !finite(flight.playerHitGap) || flight.playerHitGap < 0 ||
            !effectKnown(flight.effect) || !effectKnown(flight.rider)) {
            return false;
        }
        if (flight.pierced.size() > kMaxContacts - contacts) {
            return false;
        }
        contacts += flight.pierced.size();
        if (flight.playerContacts.size() > 4 ||
            flight.playerContacts.size() > kMaxContacts - contacts) {
            return false;
        }
        contacts += flight.playerContacts.size();
        std::unordered_set<s32> targets;
        for (const s32 target : flight.pierced) {
            if (!targets.insert(target).second) {
                return false;
            }
        }
        std::unordered_set<s32> players;
        for (const auto& contact : flight.playerContacts) {
            // Native leaves elapsed cooldowns in the list, including negatives.
            if (contact.player < 0 || contact.player >= 4 || !finite(contact.remaining) ||
                !players.insert(contact.player).second) {
                return false;
            }
        }
    }
    instances.clear();
    for (const auto& visual : history) {
        if (visual.instance == 0 || !instances.insert(visual.instance).second ||
            (nextInstance != 0 && visual.instance >= nextInstance) || visual.owner < 0 ||
            visual.owner >= 4 || visual.character < 0 || visual.character > 16 ||
            visual.potion < 0 || visual.potion > 4 || !validRelease(visual.release) ||
            !validPotion(visual.potionRelease) ||
            (visual.release && (visual.potion != 0 || visual.potionRelease)) ||
            (visual.potionRelease && visual.potion == 0) || !visual.flight.valid() ||
            !affine(visual.placement) || !finite(visual.direction) || !usedEffect(visual.effect) ||
            !usedEffect(visual.rider)) {
            return false;
        }
        const auto live = std::ranges::find(missiles, visual.instance, &Flight::instance);
        if (live != missiles.end() &&
            (visual.owner != live->owner || visual.potion != live->potion ||
             visual.release != live->release || visual.potionRelease != live->potionRelease ||
             visual.effect != live->effect || visual.rider != live->rider ||
             visual.flight.retiring)) {
            return false;
        }
    }
    for (const auto& impact : impacts) {
        if (!finite(impact.position) || impact.owner < 0 || impact.owner >= 4 ||
            impact.potion < 0 || impact.potion > 4 || !finite(impact.potency) ||
            !finite(impact.damage) || !validSound(impact.wallSound) || impact.node < -1 ||
            impact.worldObject < -1 || impact.player < -1 || impact.player >= 4 ||
            !finite(impact.direction) || !finite(impact.playerHitGap) || impact.playerHitGap < 0) {
            return false;
        }
    }
    return true;
}

struct PlayerMissiles::Prepared::Data {
    const PlayerMissiles* owner = nullptr;
    std::weak_ptr<const u8> lifetime;
    u64 generation = 0;
    std::vector<Missile> missiles;
    std::vector<MissileImpact> impacts;
    std::vector<VisualHistory> history;
    std::optional<EffectTrees::PreparedRestore> visuals;
    u64 nextInstance = 1;
    f32 ricochetIn = 0;
};
PlayerMissiles::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
PlayerMissiles::Prepared::Prepared(Prepared&&) noexcept = default;
PlayerMissiles::Prepared& PlayerMissiles::Prepared::operator=(Prepared&&) noexcept = default;
PlayerMissiles::Prepared::~Prepared() = default;

std::optional<PlayerMissiles::Checkpoint>
PlayerMissiles::checkpoint(std::span<const Resource> resources,
                           std::span<const EffectTrees::PoolResource> effects) const {
    if (!resourcesValid(resources) || m_sim.missiles.size() > Checkpoint::kMaxMissiles ||
        m_sim.impacts.size() > Checkpoint::kMaxEvents ||
        m_sim.visualHistory.size() > Checkpoint::kMaxMissiles) {
        return std::nullopt;
    }
    const auto visuals = m_visuals.checkpointPool(effects);
    if (!visuals) {
        return std::nullopt;
    }
    Checkpoint state;
    state.visuals = *visuals;
    state.visualsBound = m_device != nullptr;
    state.nextInstance = m_sim.nextInstance;
    state.ricochetIn = m_sim.ricochetIn;
    state.history = m_sim.visualHistory;
    for (const auto& missile : m_sim.missiles) {
        const auto resource = std::ranges::find_if(resources, [&](const Resource& candidate) {
            return candidate.spec == missile.spec && candidate.model == missile.model &&
                   candidate.streak == missile.streak.texture;
        });
        if (resource == resources.end()) {
            return std::nullopt;
        }
        state.missiles.push_back({missile.instance,
                                  missile.continuity,
                                  missile.owner,
                                  missile.position,
                                  missile.velocity,
                                  missile.tumble,
                                  missile.age,
                                  missile.previousPosition,
                                  missile.previousVelocity,
                                  missile.previousTumble,
                                  missile.previousAge,
                                  missile.presentationCaptured,
                                  missile.potion,
                                  missile.potency,
                                  missile.damage,
                                  missile.scale,
                                  resource->id,
                                  missile.effect,
                                  missile.rider,
                                  missile.wallSound,
                                  missile.flags,
                                  missile.pierced,
                                  missile.release,
                                  missile.streak.color,
                                  missile.streak.forward,
                                  missile.potionRelease,
                                  missile.multiplayer,
                                  missile.playerHitGap,
                                  missile.breaksPotions,
                                  missile.playerContacts});
    }
    for (const auto& impact : m_sim.impacts) {
        u32 resourceId = 0;
        if (!impact.effect.empty()) {
            const auto resource = std::ranges::find_if(resources, [&](const Resource& candidate) {
                return candidate.spec->impactTree == impact.effect;
            });
            if (resource == resources.end()) {
                return std::nullopt;
            }
            resourceId = resource->id;
        }
        state.impacts.push_back({impact.position, impact.owner, impact.potion, impact.potency,
                                 impact.damage, impact.target, resourceId, impact.wallSound,
                                 impact.flags, impact.node, impact.worldObject, impact.liquid,
                                 impact.player, impact.direction, impact.stun,
                                 impact.playerHitGap});
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}

std::optional<PlayerMissiles::Prepared>
PlayerMissiles::prepareRestore(const Checkpoint& state, std::span<const Resource> resources,
                               std::span<const EffectTrees::PoolResource> effects) const {
    if (!state.valid() || !resourcesValid(resources) ||
        state.visualsBound != (m_device != nullptr) || m_restoreGeneration == 0) {
        return std::nullopt;
    }
    auto visuals = m_visuals.prepareRestorePool(state.visuals, effects);
    if (!visuals) {
        return std::nullopt;
    }
    auto result = std::make_unique<Prepared::Data>();
    result->owner = this;
    result->lifetime = m_restoreOwner;
    result->generation = m_restoreGeneration;
    result->visuals = std::move(visuals);
    result->nextInstance = state.nextInstance;
    result->ricochetIn = state.ricochetIn;
    result->history = state.history;
    result->missiles.reserve(state.missiles.size());
    for (const auto& flight : state.missiles) {
        const auto* resource = findResource(resources, flight.resource);
        if (resource == nullptr) {
            return std::nullopt;
        }
        result->missiles.push_back({flight.instance,
                                    flight.continuity,
                                    flight.owner,
                                    flight.position,
                                    flight.velocity,
                                    flight.tumble,
                                    flight.age,
                                    flight.previousPosition,
                                    flight.previousVelocity,
                                    flight.previousTumble,
                                    flight.previousAge,
                                    flight.presentationCaptured,
                                    flight.potion,
                                    flight.potency,
                                    flight.damage,
                                    flight.scale,
                                    resource->spec,
                                    resource->model,
                                    flight.effect,
                                    flight.rider,
                                    flight.wallSound,
                                    flight.flags,
                                    flight.pierced,
                                    flight.release,
                                    {resource->streak, flight.streakColor, flight.streakForward},
                                    flight.potionRelease,
                                    flight.multiplayer,
                                    flight.playerHitGap,
                                    flight.breaksPotions,
                                    flight.playerContacts});
    }
    result->impacts.reserve(state.impacts.size());
    for (const auto& impact : state.impacts) {
        std::string_view effect;
        if (impact.effectResource != 0) {
            const auto* resource = findResource(resources, impact.effectResource);
            if (resource == nullptr || resource->spec->impactTree.empty()) {
                return std::nullopt;
            }
            effect = resource->spec->impactTree;
        }
        result->impacts.push_back(
            {impact.position, impact.owner, impact.potion, impact.potency, impact.damage,
             impact.target, effect, impact.wallSound, impact.flags, impact.node, impact.worldObject,
             impact.liquid, impact.player, impact.direction, impact.stun, impact.playerHitGap});
    }
    return Prepared{std::move(result)};
}

bool PlayerMissiles::canCommit(const Prepared& state) const noexcept {
    return state.m_data && state.m_data->owner == this && !state.m_data->lifetime.expired() &&
           state.m_data->lifetime.lock() == m_restoreOwner && m_restoreGeneration != 0 &&
           state.m_data->generation == m_restoreGeneration && state.m_data->visuals &&
           m_visuals.canCommit(*state.m_data->visuals);
}
bool PlayerMissiles::commitRestore(Prepared&& state) noexcept {
    if (!canCommit(state)) {
        return false;
    }
    auto data = std::move(state.m_data);
    // Child preflight above makes this infallible without intervening gameplay.
    if (!m_visuals.commitRestore(std::move(*data->visuals))) {
        return false;
    }
    m_sim.missiles.swap(data->missiles);
    m_sim.impacts.swap(data->impacts);
    m_sim.visualHistory.swap(data->history);
    m_sim.nextInstance = data->nextInstance;
    m_sim.ricochetIn = data->ricochetIn;
    ++m_restoreGeneration;
    return true;
}
bool PlayerMissiles::restore(const Checkpoint& state, std::span<const Resource> resources,
                             std::span<const EffectTrees::PoolResource> effects) {
    auto prepared = prepareRestore(state, resources, effects);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
