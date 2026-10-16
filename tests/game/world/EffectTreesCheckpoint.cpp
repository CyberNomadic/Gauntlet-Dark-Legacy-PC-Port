#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

#include "game/world/EffectTrees.h"

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
bool resourcesValid(std::span<const EffectTrees::PoolResource> resources) {
    if (resources.size() >
        EffectTrees::PoolCheckpoint::kMaxEffects + EffectTrees::PoolCheckpoint::kMaxBanks) {
        return false;
    }
    std::unordered_set<u32> ids;
    for (const auto& resource : resources) {
        if (resource.id == 0 || !ids.insert(resource.id).second || resource.archive == nullptr ||
            resource.textures == nullptr || resource.lenders.size() > 64 ||
            std::ranges::any_of(resource.lenders,
                                [](const auto* lender) { return lender == nullptr; })) {
            return false;
        }
        if (const auto* prototype = resource.prototype;
            prototype != nullptr && (prototype->archive != resource.archive ||
                                     !std::ranges::equal(prototype->lenders, resource.lenders) ||
                                     !prototype->then.empty())) {
            return false;
        }
    }
    return true;
}
const EffectTrees::PoolResource* findResource(std::span<const EffectTrees::PoolResource> resources,
                                              u32 id) {
    const auto found = std::ranges::find(resources, id, &EffectTrees::PoolResource::id);
    return found == resources.end() ? nullptr : &*found;
}
bool sameBinding(const EffectTrees::Effect& effect, const EffectTrees::Effect& prototype) {
    return prototype.archive == effect.archive && prototype.device == effect.device &&
           prototype.lenders == effect.lenders && prototype.emitParticles == effect.emitParticles &&
           prototype.missingObjectsAreEmpty == effect.missingObjectsAreEmpty;
}
bool birthParticles(const TreeParticles& particles) {
    const auto state = particles.checkpoint();
    if (!state.valid() || state.field.frameRemainder != 0 || state.field.lastAdvance != 0 ||
        state.field.heatFrames != 0) {
        return false;
    }
    for (usize index = 0; index < state.nodes.size(); ++index) {
        const auto& entry = state.field.entries[index];
        const auto& emitter = entry.emitter;
        // TreeParticles::bind seeds node i with i+1. These bounded node IDs
        // are below minstd_rand's modulus; no RNG draw precedes the first step.
        const auto phase = emitter.descriptor.delay > 0 ? ParticleEmitter::Phase::Delay
                                                        : ParticleEmitter::Phase::Emitting;
        if (entry.worldObject != -1 || !emitter.particles.empty() || emitter.phase != phase ||
            emitter.age != 0 || emitter.saved != 0 || !emitter.emitting ||
            emitter.random != state.nodes[index].node + 1) {
            return false;
        }
    }
    return true;
}
const EffectTrees::PoolResource* treeResource(std::span<const EffectTrees::PoolResource> resources,
                                              const EffectTrees::Effect& effect,
                                              const TreeInfo* tree) {
    const auto found = std::ranges::find_if(resources, [&](const auto& resource) {
        return resource.prototype != nullptr && resource.prototype->tree == tree &&
               tree != nullptr && resource.prototype->name == tree->name &&
               sameBinding(effect, *resource.prototype);
    });
    return found == resources.end() ? nullptr : &*found;
}
const EffectTrees::PoolResource* morphResource(std::span<const EffectTrees::PoolResource> resources,
                                               const EffectTrees::Effect& effect,
                                               std::string_view name) {
    if (effect.tree == nullptr || effect.device == nullptr || effect.archive == nullptr) {
        return nullptr;
    }
    const auto index = effect.archive->trees.find(name);
    if (!index) {
        return nullptr;
    }
    const auto* resource = treeResource(resources, effect, &effect.archive->trees.tree(*index));
    if (resource == nullptr ||
        (!(resource->prototype->model && resource->prototype->model->bound()) &&
         resource->prototype->particles.field().size() == 0) ||
        !birthParticles(resource->prototype->particles)) {
        return nullptr;
    }
    return resource;
}
} // namespace

bool EffectTrees::PoolCheckpoint::valid() const {
    if (effects.size() > kMaxEffects || banks.size() > kMaxBanks) {
        return false;
    }
    std::unordered_set<u32> ids;
    std::unordered_set<u64> instances;
    std::unordered_set<u32> bankIds;
    for (const auto& bank : banks) {
        if (bank.resource == 0 || !bankIds.insert(bank.resource).second || !bank.clock.valid()) {
            return false;
        }
    }
    for (const auto& entry : effects) {
        const auto& state = entry.state;
        if (entry.resource == 0 || entry.id == 0 || entry.instance == 0 ||
            !ids.insert(entry.id).second || !instances.insert(entry.instance).second ||
            (nextId != 0 && entry.id >= nextId) ||
            (nextInstance != 0 && entry.instance >= nextInstance) || !state.valid() ||
            (!state.setting.then.empty() && !state.tree) || !affine(entry.previousTransform) ||
            (entry.previousDirection && !finite(*entry.previousDirection)) ||
            !finite(entry.previousFrame) || entry.previousFrame < 0 ||
            (entry.previousTree && !state.tree) ||
            (entry.previousResource != 0 &&
             (!entry.previousTree || entry.previousResource == entry.resource)) ||
            (entry.provenance && (entry.provenance->player < 0 || entry.provenance->player >= 4 ||
                                  entry.provenance->generation == 0))) {
            return false;
        }
    }
    return true;
}

struct EffectTrees::PreparedRestore::Data {
    const EffectTrees* owner = nullptr;
    std::weak_ptr<const u8> lifetime;
    u64 generation = 0;
    std::vector<std::unique_ptr<Effect>> effects;
    std::vector<Motion*> motions;
    std::vector<std::unique_ptr<Motion>> motionCache;
    bool presentationAdvanced = false;
    u32 nextId = 1;
    u64 nextInstance = 1;
};
EffectTrees::PreparedRestore::PreparedRestore(std::unique_ptr<Data> data)
    : m_data(std::move(data)) {}
EffectTrees::PreparedRestore::PreparedRestore(PreparedRestore&&) noexcept = default;
EffectTrees::PreparedRestore&
EffectTrees::PreparedRestore::operator=(PreparedRestore&&) noexcept = default;
EffectTrees::PreparedRestore::~PreparedRestore() = default;

std::optional<EffectTrees::PoolCheckpoint>
EffectTrees::checkpointPool(std::span<const PoolResource> resources) const {
    if (!resourcesValid(resources) || m_effects.size() > PoolCheckpoint::kMaxEffects ||
        m_motions.size() > PoolCheckpoint::kMaxBanks) {
        return std::nullopt;
    }
    PoolCheckpoint result;
    result.nextId = m_nextId;
    result.nextInstance = m_nextInstance;
    result.presentationAdvanced = m_presentationAdvanced;
    for (const auto& motion : m_motions) {
        const auto resource = std::ranges::find_if(resources, [&](const PoolResource& candidate) {
            return candidate.archive == motion->archive &&
                   std::ranges::equal(candidate.lenders, motion->lenders);
        });
        if (resource == resources.end()) {
            return std::nullopt;
        }
        result.banks.push_back({resource->id, motion->animator.checkpoint()});
    }
    for (const auto& effect : m_effects) {
        auto state = captureEffect(*effect);
        if (!effect->then.empty() && morphResource(resources, *effect, effect->then) == nullptr) {
            return std::nullopt;
        }
        const auto resource = std::ranges::find_if(resources, [&](const PoolResource& candidate) {
            const auto* prototype = candidate.prototype;
            if (prototype == nullptr || prototype->archive != effect->archive ||
                prototype->tree != effect->tree || prototype->name != effect->name ||
                prototype->device != effect->device || prototype->lenders != effect->lenders ||
                prototype->emitParticles != effect->emitParticles ||
                prototype->missingObjectsAreEmpty != effect->missingObjectsAreEmpty ||
                prototype->particleTextureSlot != effect->particleTextureSlot ||
                !prototype->trails.canRestore(state.trails) ||
                !prototype->particles.canRestore(state.particles)) {
                return false;
            }
            for (usize index = 0; index < effect->trails.size(); ++index) {
                if ((!effect->particleTextureSlot || index != 0) &&
                    effect->trails.textureOf(index) != prototype->trails.textureOf(index)) {
                    return false;
                }
            }
            return true;
        });
        if (resource == resources.end()) {
            return std::nullopt;
        }
        u32 previousResource = 0;
        if (effect->previousTree != nullptr && effect->previousTree != effect->tree) {
            const auto* previous = treeResource(resources, *effect, effect->previousTree);
            if (previous == nullptr) {
                return std::nullopt;
            }
            previousResource = previous->id;
        }
        result.effects.push_back({resource->id, effect->id, effect->instance, std::move(state),
                                  effect->provenance, effect->previousTransform,
                                  effect->previousDirection, effect->previousTree != nullptr,
                                  effect->previousGeneration, effect->previousFrame,
                                  effect->presentationCaptured, previousResource});
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<EffectTrees::PreparedRestore>
EffectTrees::prepareRestorePool(const PoolCheckpoint& state,
                                std::span<const PoolResource> resources) const {
    if (!state.valid() || !resourcesValid(resources) || m_restoreGeneration == 0) {
        return std::nullopt;
    }
    EffectTrees candidate;
    for (const auto& bank : state.banks) {
        const auto* resource = findResource(resources, bank.resource);
        if (resource == nullptr || std::ranges::any_of(candidate.m_motions, [&](const auto& prior) {
                return prior->archive == resource->archive &&
                       std::ranges::equal(prior->lenders, resource->lenders);
            })) {
            return std::nullopt;
        }
        auto motion = std::make_unique<Motion>();
        motion->archive = resource->archive;
        motion->lenders.assign(resource->lenders.begin(), resource->lenders.end());
        motion->animator = *resource->textures;
        if (!motion->animator.restore(bank.clock)) {
            return std::nullopt;
        }
        candidate.m_motions.push_back(motion.get());
        candidate.m_motionCache.push_back(std::move(motion));
    }
    for (const auto& entry : state.effects) {
        const auto* resource = findResource(resources, entry.resource);
        if (resource == nullptr || resource->prototype == nullptr ||
            !std::ranges::any_of(candidate.m_motions, [&](const auto& bank) {
                return bank->archive == resource->archive &&
                       std::ranges::equal(bank->lenders, resource->lenders);
            })) {
            return std::nullopt;
        }
        auto effect = std::make_unique<Effect>(*resource->prototype);
        effect->id = entry.id;
        effect->instance = entry.instance;
        effect->morphPrototype.reset();
        if (!entry.state.setting.then.empty()) {
            const auto* future = morphResource(resources, *effect, entry.state.setting.then);
            if (future == nullptr) {
                return std::nullopt;
            }
            auto birth = std::make_shared<Effect>(*future->prototype);
            birth->morphPrototype.reset();
            effect->morphPrototype = std::move(birth);
            effect->then = entry.state.setting.then;
        }
        const TreeInfo* previousTree = entry.previousTree ? effect->tree : nullptr;
        if (entry.previousResource != 0) {
            const auto* previous = findResource(resources, entry.previousResource);
            if (previous == nullptr || previous->prototype == nullptr ||
                previous->prototype->tree == nullptr || previous->prototype->tree == effect->tree ||
                !sameBinding(*effect, *previous->prototype)) {
                return std::nullopt;
            }
            previousTree = previous->prototype->tree;
        }
        candidate.m_effects.push_back(std::move(effect));
        if (!candidate.restore(entry.id, entry.state)) {
            return std::nullopt;
        }
        auto& restored = *candidate.m_effects.back();
        restored.provenance = entry.provenance;
        restored.previousTransform = entry.previousTransform;
        restored.previousDirection = entry.previousDirection;
        restored.previousTree = previousTree;
        restored.previousGeneration = entry.previousGeneration;
        restored.previousFrame = entry.previousFrame;
        restored.presentationCaptured = entry.presentationCaptured;
        restored.presentationPose = {}; // a pure draw cache, re-evaluated before every use
    }
    auto result = std::make_unique<PreparedRestore::Data>();
    result->owner = this;
    result->lifetime = m_restoreOwner;
    result->generation = m_restoreGeneration;
    result->effects = std::move(candidate.m_effects);
    result->motions = std::move(candidate.m_motions);
    result->motionCache = std::move(candidate.m_motionCache);
    result->nextId = state.nextId;
    result->nextInstance = state.nextInstance;
    result->presentationAdvanced = state.presentationAdvanced;
    return PreparedRestore{std::move(result)};
}

bool EffectTrees::canCommit(const PreparedRestore& state) const noexcept {
    return state.m_data && state.m_data->owner == this && !state.m_data->lifetime.expired() &&
           state.m_data->lifetime.lock() == m_restoreOwner && m_restoreGeneration != 0 &&
           state.m_data->generation == m_restoreGeneration;
}
bool EffectTrees::commitRestore(PreparedRestore&& state) noexcept {
    if (!canCommit(state)) {
        return false;
    }
    auto data = std::move(state.m_data);
    m_effects.swap(data->effects);
    m_motions.swap(data->motions);
    m_motionCache.swap(data->motionCache);
    m_visualCache.clear();
    ++m_localEpoch;
    m_nextId = data->nextId;
    m_nextInstance = data->nextInstance;
    m_presentationAdvanced = data->presentationAdvanced;
    ++m_restoreGeneration;
    return true;
}
bool EffectTrees::restorePool(const PoolCheckpoint& state,
                              std::span<const PoolResource> resources) {
    auto prepared = prepareRestorePool(state, resources);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
