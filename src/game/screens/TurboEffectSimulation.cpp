#include "game/screens/TurboEffectSimulation.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace gdl::game {
namespace {
bool sameBank(const TurboEffectResources::Resource& first,
              const TurboEffectResources::Resource& second) {
    return first.archive == second.archive && first.lenders == second.lenders;
}

bool sameOwner(const EffectIdentity& first, const EffectIdentity& second) {
    return first.epoch == second.epoch && first.seat == second.seat && first.grant == second.grant;
}
} // namespace

void TurboEffectSimulation::bind(const TurboEffectResources& resources) {
    clear();
    m_resources = &resources;
}

void TurboEffectSimulation::clear() {
    m_effects.clear();
    m_entries.clear();
    m_bindings.clear();
    m_banks.clear();
    m_emitted.clear();
    m_resources = nullptr;
}

EffectTrees::Prepared
TurboEffectSimulation::prepared(const TurboEffectResources::Resource& resource) {
    return {resource.archive,
            resource.tree,
            &resource.model,
            &resource.particles,
            &resource.textures,
            resource.lenders,
            resource.descriptor ? &*resource.descriptor : nullptr,
            resource.texture,
            resource.textureSlot};
}

bool TurboEffectSimulation::validPose(const Pose& pose) {
    const auto matrix = [](const Mat4& value) {
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 4; ++row) {
                if (!std::isfinite(value[column][row]) || std::abs(value[column][row]) > 1e6f) {
                    return false;
                }
            }
        }
        return value[0][3] == 0 && value[1][3] == 0 && value[2][3] == 0 && value[3][3] == 1;
    };
    if (!matrix(pose.body) || !matrix(pose.root) || pose.nodes.size() > 128) {
        return false;
    }
    return std::ranges::all_of(pose.nodes, [&](const auto& node) {
        return node.first.size() <= 256 && node.first.find('\0') == std::string::npos &&
               (!node.second || matrix(*node.second));
    });
}

std::optional<Mat4> TurboEffectSimulation::placement(const Binding& binding, const Pose& pose) {
    if (binding.particle) {
        const auto& particle = *binding.particle;
        if (particle.previousEffect || particle.partnerFallback) {
            return std::nullopt;
        }
        Mat4 parent = pose.body;
        if (!particle.node.empty()) {
            const auto node = pose.nodes.find(particle.node);
            if (node == pose.nodes.end()) {
                return std::nullopt;
            }
            parent = node->second.value_or(parent);
        }
        return glm::translate(parent, particle.offset);
    }
    if (binding.attachment) {
        const auto& attachment = *binding.attachment;
        if (attachment.actor != TurboEmission::Actor::Caster) {
            return std::nullopt;
        }
        return (attachment.bodyParent ? pose.body : pose.root) * attachment.local;
    }
    return Mat4{1}; // No parent: preserve the native world placement.
}

bool TurboEffectSimulation::rememberBank(u32 resource) {
    const auto* selected = m_resources->resource(resource);
    if (selected == nullptr) {
        return false;
    }
    for (const auto id : m_banks) {
        if (sameBank(*selected, *m_resources->resource(id))) {
            return true;
        }
    }
    m_banks.push_back(resource);
    return true;
}

bool TurboEffectSimulation::start(const EffectIdentity& base, const TurboEmission::Strike& strike,
                                  const Pose& pose,
                                  std::span<const std::optional<u32>> particleSeeds) {
    if (m_resources == nullptr || !base.valid() || base.effectOrdinal != 0 || !validPose(pose) ||
        (!m_emitted.empty() && !sameOwner(base, *m_emitted.begin()))) {
        return false;
    }
    const auto* catalog = m_resources->strike(strike.row);
    if (catalog == nullptr || strike.effects.size() != catalog->resources.size() ||
        strike.effects.size() > EffectIdentity::kMaxEffects ||
        (!particleSeeds.empty() && particleSeeds.size() != strike.effects.size())) {
        return false;
    }
    usize repeated = 0;
    usize visible = 0;
    for (usize ordinal = 0; ordinal < strike.effects.size(); ++ordinal) {
        auto origin = base;
        origin.effectOrdinal = static_cast<u16>(ordinal);
        repeated += m_emitted.contains(origin) ? 1 : 0;
        const auto* resource = m_resources->resource(catalog->resources[ordinal]);
        const auto& emission = strike.effects[ordinal];
        if (resource == nullptr || resource->row != emission.row || emission.carriesFlight ||
            !emission.setting.then.empty() || emission.setting.morphIn ||
            emission.setting.holdForMorph) {
            return false;
        }
        if (resource->kind == TurboEffectResources::Kind::Invisible) {
            continue;
        }
        ++visible;
        const Binding binding{emission.attachment, emission.particle,
                              particleSeeds.empty() ? std::nullopt : particleSeeds[ordinal]};
        if (!placement(binding, pose) ||
            (resource->kind == TurboEffectResources::Kind::Particles &&
             (!binding.particle || !binding.particleSeed || *binding.particleSeed == 0 ||
              resource->descriptor != std::optional{binding.particle->descriptor})) ||
            (resource->kind == TurboEffectResources::Kind::Tree && binding.particle)) {
            return false;
        }
        EffectTrees::Checkpoint value;
        value.name = emission.authored.tree;
        value.tree = resource->tree != nullptr;
        value.setting = emission.setting;
        value.position = emission.position;
        if (!value.valid()) {
            return false;
        }
    }
    if (repeated == strike.effects.size()) {
        return true;
    }
    if (repeated != 0 || m_entries.size() + visible > kMaxEffects ||
        m_emitted.size() + strike.effects.size() > kMaxEmissions) {
        return false;
    }
    for (usize ordinal = 0; ordinal < strike.effects.size(); ++ordinal) {
        auto origin = base;
        origin.effectOrdinal = static_cast<u16>(ordinal);
        const auto resourceId = catalog->resources[ordinal];
        const auto& resource = *m_resources->resource(resourceId);
        m_emitted.insert(origin);
        if (resource.kind == TurboEffectResources::Kind::Invisible) {
            continue;
        }
        const auto& emission = strike.effects[ordinal];
        Binding binding{emission.attachment, emission.particle,
                        particleSeeds.empty() ? std::nullopt : particleSeeds[ordinal]};
        // Native PsfxDoParticle starts at identity, then attaches before its first
        // update. Tree bind initially uses the emitted world position in the same way.
        const u32 id = m_effects.startPrepared(prepared(resource), emission.position,
                                               emission.setting, Mat4{1}, binding.particleSeed);
        if (id == 0 || !rememberBank(resourceId)) {
            clear();
            return false;
        }
        m_effects.tag(id,
                      {origin.seat, origin.generation, origin.occurrence, origin.effectOrdinal});
        if (binding.attachment || binding.particle) {
            m_effects.placeAt(id, *placement(binding, pose));
        }
        m_entries.push_back({origin, resourceId, id});
        m_bindings.emplace(id, std::move(binding));
    }
    return true;
}

bool TurboEffectSimulation::step(f32 seconds, const Pose& pose) {
    if (m_resources == nullptr || !std::isfinite(seconds) || seconds < 0 || seconds > 0.1f ||
        !validPose(pose)) {
        return false;
    }
    std::vector<std::pair<u32, Mat4>> attachments;
    for (const auto& entry : m_entries) {
        const auto& binding = m_bindings.at(entry.id);
        const auto at = placement(binding, pose);
        if (!at) {
            return false;
        }
        if (binding.attachment || binding.particle) {
            attachments.emplace_back(entry.id, *at);
        }
    }
    m_effects.capturePresentation();
    for (const auto& [id, at] : attachments) {
        m_effects.placeAt(id, at);
    }
    m_effects.update(seconds);
    std::erase_if(m_entries, [&](const Entry& entry) {
        if (m_effects.playing(entry.id)) {
            return false;
        }
        m_bindings.erase(entry.id);
        return true;
    });
    return true;
}

TurboEffectSimulation::Checkpoint TurboEffectSimulation::checkpoint() const {
    Checkpoint out;
    for (const auto& entry : m_entries) {
        const auto& binding = m_bindings.at(entry.id);
        const auto effect = m_effects.checkpoint(entry.id);
        if (effect) {
            auto logical = entry;
            logical.id = 0; // Pool handles are reconstructed, never saved as identities.
            out.effects.push_back(
                {logical, *effect, binding.attachment, binding.particle, binding.particleSeed});
        }
    }
    for (const auto id : m_banks) {
        const auto& resource = *m_resources->resource(id);
        const auto clock = m_effects.textureCheckpoint(*resource.archive, resource.lenders);
        if (clock) {
            out.banks.push_back({id, *clock});
        }
    }
    out.emitted.assign(m_emitted.begin(), m_emitted.end());
    return out;
}

bool TurboEffectSimulation::restore(const Checkpoint& state) {
    if (m_resources == nullptr || state.effects.size() > kMaxEffects ||
        state.emitted.size() > kMaxEmissions || state.banks.size() > kMaxEmissions) {
        return false;
    }
    TurboEffectSimulation next;
    next.bind(*m_resources);
    for (const auto& origin : state.emitted) {
        if (!origin.valid() ||
            (!next.m_emitted.empty() && !sameOwner(origin, *next.m_emitted.begin())) ||
            !next.m_emitted.insert(origin).second) {
            return false;
        }
    }
    for (const auto& bank : state.banks) {
        const auto* resource = m_resources->resource(bank.resource);
        if (resource == nullptr || resource->archive == nullptr ||
            std::ranges::any_of(
                next.m_banks,
                [&](u32 id) { return sameBank(*resource, *m_resources->resource(id)); }) ||
            !next.m_effects.prepareBank(prepared(*resource)) ||
            !next.m_effects.restoreTextureClock(*resource->archive, resource->lenders,
                                                bank.clock)) {
            return false;
        }
        next.m_banks.push_back(bank.resource);
    }
    std::set<EffectIdentity> live;
    for (const auto& value : state.effects) {
        const auto* resource = m_resources->resource(value.entry.resource);
        const Binding binding{value.attachment, value.particle, value.particleSeed};
        if (resource == nullptr || resource->kind == TurboEffectResources::Kind::Invisible ||
            value.entry.id != 0 || !next.m_emitted.contains(value.entry.origin) ||
            !live.insert(value.entry.origin).second || !value.effect.valid() ||
            !value.effect.setting.then.empty() || value.effect.setting.morphIn ||
            value.effect.setting.holdForMorph ||
            (binding.attachment && binding.attachment->actor != TurboEmission::Actor::Caster) ||
            (binding.particle &&
             (binding.particle->previousEffect || binding.particle->partnerFallback)) ||
            (resource->kind == TurboEffectResources::Kind::Particles &&
             (!binding.particle || !binding.particleSeed || *binding.particleSeed == 0 ||
              resource->descriptor != std::optional{binding.particle->descriptor})) ||
            (resource->kind == TurboEffectResources::Kind::Tree && binding.particle) ||
            !std::ranges::any_of(next.m_banks, [&](u32 id) {
                return sameBank(*resource, *m_resources->resource(id));
            })) {
            return false;
        }
        const u32 id =
            next.m_effects.startPrepared(prepared(*resource), value.effect.position,
                                         value.effect.setting, Mat4{1}, value.particleSeed);
        if (id == 0 || !next.m_effects.restore(id, value.effect)) {
            return false;
        }
        const auto& origin = value.entry.origin;
        next.m_effects.tag(
            id, {origin.seat, origin.generation, origin.occurrence, origin.effectOrdinal});
        next.m_entries.push_back({origin, value.entry.resource, id});
        next.m_bindings.emplace(id, binding);
    }
    *this = std::move(next);
    return true;
}
} // namespace gdl::game
