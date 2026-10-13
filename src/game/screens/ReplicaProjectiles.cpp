#include "game/screens/ReplicaProjectiles.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "game/enemies/CombatantAssets.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/screens/LevelArrivalPresentation.h"
#include "game/screens/TowerPromotion.h"
#include "game/screens/TowerRelics.h"
#include "game/world/PlayerMissiles.h"
#include "game/world/SumnerFigure.h"

namespace gdl::game {
bool ProjectileResources::available(u32 id) const {
    return id != 0 && id <= 65535 && m_entries.size() < 1024 && !m_entries.contains(id);
}
bool ProjectileResources::addModel(u32 id, const TreeModel& model) {
    if (!available(id) || !model.bound() || modelId(&model) != 0) {
        return false;
    }
    auto& entry = m_entries[id];
    entry.source = &model;
    entry.model = model;
    return true;
}
bool ProjectileResources::addStreak(u32 id, const Texture& texture) {
    if (!available(id) || streakId(&texture) != 0) {
        return false;
    }
    m_entries[id].texture = &texture;
    return true;
}
bool ProjectileResources::addEffect(u32 id, const EffectTrees::Effect& prototype,
                                    RenderDevice& device) {
    if (!available(id) || prototype.archive == nullptr || prototype.tree == nullptr ||
        effectId(prototype) != 0) {
        return false;
    }
    Entry entry;
    entry.archive = prototype.archive;
    entry.tree = prototype.tree;
    entry.lenders = prototype.lenders;
    entry.model = prototype.model;
    if (std::ranges::any_of(prototype.tree->nodes,
                            [](const auto& node) { return node.particle >= 0; })) {
        TreePose pose;
        if (entry.tree->sequences.empty()) {
            pose.rest(*entry.tree);
        } else {
            pose.evaluate(*entry.tree, 0, 0);
        }
        entry.particles.bind(*entry.tree, *prototype.archive, device, Mat4{1}, pose.matrices(),
                             entry.lenders);
        entry.particles.setLocalScales(pose.poses());
    }
    if (!entry.model.bound() && entry.particles.field().size() == 0) {
        return false;
    }
    entry.textures.bind(prototype.archive->trees.textureAnimations(), prototype.archive->textures,
                        device, prototype.lenders);
    m_entries.emplace(id, std::move(entry));
    return true;
}
bool ProjectileResources::addTree(u32 id, ItemArchive& archive, std::string_view tree,
                                  RenderDevice& device, std::span<TextureSet* const> lenders) {
    const auto index = archive.trees.find(tree);
    if (!available(id) || !archive.loaded() || !index) {
        return false;
    }
    EffectTrees::Effect prototype;
    prototype.archive = &archive;
    prototype.tree = &archive.trees.tree(*index);
    prototype.lenders.assign(lenders.begin(), lenders.end());
    return prototype.model.bind(*prototype.tree, archive.models, archive.textures, device,
                                lenders) &&
           addEffect(id, prototype, device);
}
bool ProjectileResources::addCombatant(u32 firstId, CombatantAssets& stock, RenderDevice& device,
                                       std::span<TextureSet* const> lenders) {
    if (firstId == 0 || firstId > 65535 || !stock.archive.loaded()) {
        return false;
    }
    std::set<std::string> names;
    const auto collect = [&](const CritterData& data) {
        for (const auto& cue : data.sounds()) {
            if (!cue.shows() || (cue.flags & (0x0F000000U | CombatEffectDefinition::kSkin)) != 0) {
                continue; // Custom emitters/skins are not effect-tree meshes.
            }
            const auto index = stock.archive.trees.find(cue.tree);
            if (!index) {
                continue; // Shared WEAPONS effects belong to their own archive roster.
            }
            const auto& tree = stock.archive.trees.tree(*index);
            const bool mesh = std::ranges::any_of(tree.nodes, [](const auto& node) {
                return !node.object.empty() ||
                       std::ranges::any_of(node.objectFrames,
                                           [](const auto& run) { return !run.object.empty(); });
            });
            if (mesh) {
                names.insert(cue.tree);
            }
        }
    };
    collect(stock.data);
    for (const auto& child : stock.children) {
        collect(child);
    }
    if (names.size() > 65536U - firstId) {
        return false;
    }
    ProjectileResources candidate = *this;
    u32 id = firstId;
    for (const auto& name : names) {
        if (!candidate.addTree(id++, stock.archive, name, device, lenders)) {
            return false;
        }
    }
    *this = std::move(candidate);
    return true;
}
u32 ProjectileResources::modelId(const TreeModel* model) const {
    if (model != nullptr) {
        for (const auto& [id, entry] : m_entries) {
            if (entry.source == model) {
                return id;
            }
        }
    }
    return 0;
}
u32 ProjectileResources::streakId(const Texture* texture) const {
    if (texture != nullptr) {
        for (const auto& [id, entry] : m_entries) {
            if (entry.texture == texture) {
                return id;
            }
        }
    }
    return 0;
}
u32 ProjectileResources::effectId(const EffectTrees::Effect& effect) const {
    return treeId(effect.archive, effect.tree, effect.lenders);
}
u32 ProjectileResources::treeId(const ItemArchive* archive, const TreeInfo* tree,
                                std::span<TextureSet* const> lenders) const {
    u32 selfContained = 0;
    for (const auto& [id, entry] : m_entries) {
        // A self-contained tower figure uses the same native tree even when its
        // preloaded stage model has extra fallback texture banks. Own-bank
        // textures always take precedence; explicit lender lists still match exactly.
        if (lenders.empty() && entry.archive == archive && entry.tree == tree && tree != nullptr) {
            selfContained = id;
        }
        if (entry.archive == archive && entry.tree == tree && entry.tree != nullptr &&
            std::ranges::equal(entry.lenders, lenders)) {
            return id;
        }
    }
    return selfContained;
}
bool ProjectileResources::accepts(const ProjectileState& state) const {
    if (!state.valid()) {
        return false;
    }
    const auto found = m_entries.find(state.resource);
    if (found == m_entries.end()) {
        return false;
    }
    const auto& entry = found->second;
    if (state.source == ProjectileSource::Streak) {
        return entry.texture != nullptr && state.animation.generation == 0;
    }
    const bool effect = state.source == ProjectileSource::PlayerEffect ||
                        state.source == ProjectileSource::WorldEffect ||
                        state.source >= ProjectileSource::Arrival;
    if (effect != (entry.tree != nullptr) ||
        (!entry.model.bound() && entry.particles.field().size() == 0)) {
        return false;
    }
    if (state.animation.generation == 0) {
        return true;
    }
    return entry.tree != nullptr && state.animation.sequence < entry.tree->sequences.size() &&
           state.animation.frame <=
               static_cast<f32>(
                   std::max(0, entry.tree->sequences[state.animation.sequence].frames - 1));
}
bool ProjectileResources::sampleWeapon(ProjectileState& state, f32 age, f32 textureFrame) const {
    const auto found = m_entries.find(state.resource);
    if (found == m_entries.end() || found->second.tree == nullptr || !std::isfinite(age) ||
        age < 0 || age >= PlayerMissiles::kLifeSeconds) {
        return false;
    }
    state.animation = {};
    const auto& sequences = found->second.tree->sequences;
    if (!sequences.empty()) {
        AnimationPlayer player;
        player.start(sequences.front(), 0);
        player.advance(age, true);
        state.animation = {0, 0, player.generation(), player.presentationFrame(), 1};
    }
    state.textureFrame = textureFrame;
    return accepts(state);
}

bool ProjectileResources::acceptsWeapon(const PlayerWeaponState& state) const {
    if (!state.valid()) {
        return false;
    }
    ProjectileState probe;
    probe.source = ProjectileSource::PlayerEffect;
    probe.instance = 1;
    probe.resource = state.resource;
    if (!sampleWeapon(probe, 0, state.textureFrame.value_or(0))) {
        return false;
    }
    if (state.rider != 0) {
        probe.source = ProjectileSource::PlayerRider;
        probe.resource = state.rider;
        if (!sampleWeapon(probe, 0, state.riderTextureFrame.value_or(0))) {
            return false;
        }
    }
    if (state.streak != 0) {
        probe.source = ProjectileSource::Streak;
        probe.resource = state.streak;
        probe.animation = {};
        if (!accepts(probe)) {
            return false;
        }
    }
    return true;
}

bool ProjectileResources::prepareParticles(const ProjectileState& state,
                                           ReplicaProjectileParticles& particles) const {
    const auto found = m_entries.find(state.resource);
    return state.particles && found != m_entries.end() && found->second.tree != nullptr &&
           particles.update(state, *found->second.tree, found->second.particles);
}

bool ProjectileResources::hasParticles(u32 resource) const {
    const auto found = m_entries.find(resource);
    return found != m_entries.end() && found->second.particles.field().size() != 0;
}

f32 ProjectileResources::particleLifetime(u32 resource) const {
    const auto found = m_entries.find(resource);
    if (found == m_entries.end()) {
        return 0;
    }
    f32 lifetime = 0;
    const auto& field = found->second.particles.field();
    for (usize index = 0; index < field.size(); ++index) {
        const auto& descriptor = field.emitter(index).descriptor();
        lifetime =
            std::max(lifetime, static_cast<f32>(descriptor.particleLife + descriptor.particleFade) /
                                   ParticleDescriptor::kFrameRate);
    }
    return lifetime;
}

void ProjectileResources::drawParticles(RenderDevice& device, const ProjectileState& state,
                                        ReplicaProjectileParticles& particles, const Mat4& clip,
                                        const CameraFrame& camera) {
    const auto found = m_entries.find(state.resource);
    if (found == m_entries.end() || found->second.tree == nullptr || !state.valid()) {
        return;
    }
    auto& field = particles.particles();
    const bool held = state.particles && state.particles->held;
    // Match native integer counter wrapping before adding the fractional frame.
    // Passing an absolute clock as an offset loses precision on short cycles.
    const f32 whole = std::floor(state.textureFrame);
    found->second.textures.seek(static_cast<u32>(whole));
    found->second.textures.apply(
        field, *found->second.tree, state.animation.sequence, state.animation.frame,
        held ? std::nullopt : std::optional<f32>{state.textureFrame - whole});
    field.draw(device, clip, camera.right, camera.up, held ? -1.0f : 1.0f);
}

void ProjectileResources::draw(RenderDevice& device, const ProjectileState& state, const Mat4& clip,
                               const WorldLighting& lighting, const CameraFrame& camera,
                               TreeModel::Pass pass) {
    if (!accepts(state)) {
        return;
    }
    if (state.particles && state.particles->flight.retiring) {
        return;
    }
    auto& entry = m_entries.at(state.resource);
    if (state.source == ProjectileSource::Streak) {
        if (pass == TreeModel::Pass::DepthWriting || pass == TreeModel::Pass::Opaque) {
            return;
        }
        const MissileStreak streak{entry.texture, state.tint, state.forward};
        const auto batch = streak.geometry(Vec3{state.placement[3]}, state.direction, state.age,
                                           state.radius, camera);
        DrawState draw;
        draw.alphaTest = DrawState::kTranslucentAlphaTest;
        device.draw(batch, *entry.texture, clip, draw);
        return;
    }
    auto& model = entry.model;
    const auto& animation = state.animation;
    const auto occlusion = (state.flags & ProjectileState::kSolidWorld) != 0
                               ? TreeModel::Occlusion::SolidWorld
                               : TreeModel::Occlusion::Authored;
    const Mat4 placement = (state.flags & ProjectileState::kAlong) != 0
                               ? camera.along(state.placement, state.direction)
                               : state.placement;
    if (entry.tree == nullptr) {
        model.draw(device, clip, placement, lighting, {}, &camera, state.alpha, pass, occlusion);
        return;
    }
    TreePose pose;
    if (animation.generation == 0 || entry.tree->sequences[animation.sequence].frames == 0) {
        pose.rest(*entry.tree);
    } else {
        pose.evaluate(*entry.tree, animation.sequence, animation.frame, false, true);
    }
    const f32 whole = std::floor(state.textureFrame);
    entry.textures.seek(static_cast<u32>(whole));
    entry.textures.apply(model, *entry.tree, animation.sequence, animation.frame,
                         state.textureFrame - whole);
    model.setPresentationFrame(animation.sequence, animation.frame);
    model.setAppearance((state.flags & ProjectileState::kUnlit) != 0, state.tint,
                        (state.flags & ProjectileState::kDepthWrite) != 0,
                        (state.flags & ProjectileState::kAdditive) != 0);
    if (state.source == ProjectileSource::Arrival || state.source == ProjectileSource::Sumner ||
        state.source == ProjectileSource::Promotion) {
        // STARTFX is a posed ring at the feet, not a camera-facing missile tree.
        model.draw(device, clip, placement, lighting, pose.matrices(), nullptr, state.alpha, pass);
        return;
    }
    const auto matrices = pose.drawMatrices(placement, camera);
    model.draw(device, clip, Mat4{1}, lighting, matrices, nullptr, state.alpha, pass, occlusion);
}

bool ProjectileCapture::append(CombatSnapshot& snapshot, const ProjectileResources& resources,
                               const PlayerMissiles& players, const EnemyMissiles& enemies,
                               const EffectTrees* worldEffects,
                               const LevelArrivalPresentation* arrival, const Tower* tower) {
    std::vector<ProjectileState> shots;
    const auto add = [&](ProjectileState state) {
        if (shots.size() >= CombatSnapshot::kMaxProjectiles || !resources.accepts(state)) {
            return false;
        }
        shots.push_back(state);
        return true;
    };
    for (usize i = 0; i < players.count(); ++i) {
        const auto& missile = players.missile(i);
        ProjectileState shot;
        shot.instance = missile.instance;
        shot.continuity = missile.continuity;
        shot.placement = PlayerMissiles::transformOf(missile);
        shot.direction = missile.velocity;
        if (missile.effect == 0 && missile.model != nullptr && missile.model->bound()) {
            shot.resource = resources.modelId(missile.model);
            shot.flags |= missile.spec->spin == 0 ? ProjectileState::kAlong : 0;
            if (!add(shot)) {
                return false;
            }
        }
        if (missile.streak.texture != nullptr) {
            shot.source = ProjectileSource::Streak;
            shot.resource = resources.streakId(missile.streak.texture);
            shot.age = missile.age;
            shot.radius = missile.spec->radius;
            shot.forward = missile.streak.forward;
            shot.tint = missile.streak.color;
            if (!add(shot)) {
                return false;
            }
        }
    }
    for (usize i = 0; i < enemies.count(); ++i) {
        const auto& missile = enemies.missile(i);
        if (missile.model == nullptr || !missile.model->bound()) {
            continue;
        }
        ProjectileState shot;
        shot.source = ProjectileSource::Enemy;
        shot.instance = missile.instance;
        shot.continuity = missile.continuity;
        shot.resource = resources.modelId(missile.model);
        shot.placement = EnemyMissiles::transformOf(missile);
        shot.flags |= ProjectileState::kSolidWorld;
        if (!add(shot)) {
            return false;
        }
    }
    const auto effects = [&](const EffectTrees& source, ProjectileSource type) {
        for (usize i = 0; i < source.count(); ++i) {
            const auto& effect = source.effect(i);
            const auto* history =
                type == ProjectileSource::PlayerEffect ? players.visualHistory(effect.id) : nullptr;
            if ((effect.retiring && history == nullptr) || effect.tree == nullptr ||
                (!effect.model.bound() && history == nullptr)) {
                continue; // particle fields have their own future replication stream
            }
            ProjectileState shot;
            shot.source = type;
            if (history != nullptr) {
                shot.particles = ProjectileState::Particles{
                    static_cast<u32>(history->character), history->flight, {}};
                shot.particles->potion = static_cast<u8>(history->potion);
                if (history->rider == effect.id) {
                    shot.source = ProjectileSource::PlayerRider;
                }
            }
            if (type == ProjectileSource::PlayerEffect) {
                for (usize missile = 0; missile < players.count(); ++missile) {
                    if (players.missile(missile).rider == effect.id) {
                        shot.source = ProjectileSource::PlayerRider;
                        break;
                    }
                }
            }
            shot.instance = effect.instance;
            shot.continuity = effect.continuity;
            shot.resource = resources.effectId(effect);
            shot.placement = effect.transform();
            shot.flags = (effect.depthWrite ? ProjectileState::kDepthWrite : 0) |
                         (effect.unlit ? ProjectileState::kUnlit : 0) |
                         (effect.additive ? ProjectileState::kAdditive : 0);
            if (effect.flightDirection) {
                shot.flags |= ProjectileState::kAlong;
                shot.direction = *effect.flightDirection;
            }
            shot.tint = effect.tint;
            shot.alpha = effect.opacity();
            shot.textureFrame = source.textureFrame(effect);
            if (!effect.tree->sequences.empty() && effect.player.playing()) {
                shot.animation = {0, effect.player.sequence(), effect.player.generation(),
                                  effect.player.frame(), 1};
            }
            if (!add(shot)) {
                return false;
            }
        }
        return true;
    };
    if (tower != nullptr) {
        const auto figure = [&](const TreeInfo* tree, const AnimationPlayer& animation,
                                const Mat4& placement, ProjectileSource source, f32 textureFrame) {
            if (tree == nullptr) {
                return true;
            }
            ProjectileState state;
            state.source = source;
            state.instance = 1;
            state.resource = resources.treeId(tower->items, tree);
            state.placement = placement;
            state.textureFrame = textureFrame;
            if (source == ProjectileSource::Promotion) {
                state.flags = ProjectileState::kUnlit | ProjectileState::kAdditive;
            }
            state.animation = {0, animation.sequence(), animation.generation(), animation.frame(),
                               1};
            return add(state);
        };
        if ((tower->sumner != nullptr &&
             !figure(tower->sumner->tree(), tower->sumner->animation(), tower->sumner->placement(),
                     ProjectileSource::Sumner, 0)) ||
            (tower->promotion != nullptr && tower->promotion->active() &&
             !figure(tower->promotion->tree(), tower->promotion->animation(),
                     tower->promotion->wizardTransform(), ProjectileSource::Promotion,
                     tower->promotion->textureFrame())) ||
            (tower->relics != nullptr &&
             (!effects(tower->relics->figures(), ProjectileSource::TowerRelic) ||
              (tower->speech && tower->relics->phase() == TowerRelics::Phase::Speech &&
               !effects(tower->relics->wizard(), ProjectileSource::TowerWizard))))) {
            return false;
        }
    }
    if (!effects(players.visuals(), ProjectileSource::PlayerEffect) ||
        (worldEffects != nullptr && !effects(*worldEffects, ProjectileSource::WorldEffect))) {
        return false;
    }
    if (arrival != nullptr) {
        for (usize i = 0; i < arrival->effectCount(); ++i) {
            if (const auto view = arrival->effectPresentation(i)) {
                ProjectileState state;
                state.source = ProjectileSource::Arrival;
                state.instance = i + 1;
                state.resource = resources.treeId(view->archive, view->tree);
                state.placement = glm::translate(Mat4{1}, view->position);
                state.animation = {0, view->sequence, view->generation, view->frame, 1};
                state.textureFrame = view->textureClock;
                if (!add(state)) {
                    return false;
                }
            }
        }
    }
    std::ranges::sort(shots, {}, &ProjectileState::key);
    auto candidate = snapshot;
    candidate.projectiles = std::move(shots);
    if (!candidate.valid()) {
        return false;
    }
    snapshot = std::move(candidate);
    return true;
}
bool ReplicaProjectiles::begin(u64 epoch) {
    if (epoch == 0 || epoch <= m_epoch) {
        return false;
    }
    m_epoch = epoch;
    m_tick.reset();
    m_shots.clear();
    m_particles.clear();
    return true;
}
void ReplicaProjectiles::clear() {
    m_epoch = 0;
    m_tick.reset();
    m_shots.clear();
    m_particles.clear();
}
bool ReplicaProjectiles::show(const CombatSnapshot& snapshot,
                              const ProjectileResources& resources) {
    if (m_epoch == 0 || snapshot.motion.epoch != m_epoch || !snapshot.valid() ||
        (m_tick && snapshot.motion.tick < *m_tick) ||
        !std::ranges::all_of(snapshot.projectiles,
                             [&](const auto& shot) { return resources.accepts(shot); })) {
        return false;
    }
    std::set<ParticleKey> live;
    for (const auto& shot : snapshot.projectiles) {
        if (!shot.particles) {
            continue;
        }
        const auto key = particleKey(shot);
        if (!live.insert(key).second) {
            return false;
        }
    }
    for (const auto& shot : snapshot.projectiles) {
        if (shot.particles && !resources.prepareParticles(shot, m_particles[particleKey(shot)])) {
            // Validation above preserves the previous presentation on malformed packets.
            // A failed native replay is different: discard its entire visual cache so
            // no partially advanced particles can accompany the last accepted meshes.
            m_particles.clear();
            m_shots.clear();
            return false;
        }
    }
    std::erase_if(m_particles, [&](const auto& entry) { return !live.contains(entry.first); });
    m_tick = snapshot.motion.tick;
    m_shots = snapshot.projectiles;
    return true;
}
void ReplicaProjectiles::draw(RenderDevice& device, ProjectileResources& resources,
                              const Mat4& clip, const WorldLighting& lighting,
                              const CameraFrame& camera, TreeModel::Pass pass) const {
    for (const auto& shot : m_shots) {
        resources.draw(device, shot, clip, lighting, camera, pass);
        if (shot.particles && pass != TreeModel::Pass::DepthWriting &&
            pass != TreeModel::Pass::Opaque) {
            const auto found = m_particles.find(particleKey(shot));
            if (found != m_particles.end()) {
                resources.drawParticles(device, shot, found->second, clip, camera);
            }
        }
    }
}

ReplicaProjectiles::ParticleKey ReplicaProjectiles::particleKey(const ProjectileState& state) {
    const auto origin = state.particles ? state.particles->origin : std::optional<ShotIdentity>{};
    const auto potionOrigin =
        state.particles ? state.particles->potionOrigin : std::optional<PotionIdentity>{};
    return {state.source, state.resource, origin || potionOrigin ? 0 : state.instance, origin,
            potionOrigin};
}

const TreeParticles* ReplicaProjectiles::particles(const ProjectileState& state) const {
    const auto found = m_particles.find(particleKey(state));
    return found != m_particles.end() ? &found->second.particles() : nullptr;
}
} // namespace gdl::game
