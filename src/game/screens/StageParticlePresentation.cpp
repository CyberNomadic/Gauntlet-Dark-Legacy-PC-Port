#include "game/screens/StageParticlePresentation.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "engine/world/ParticleField.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeParticles.h"

#include "game/screens/LevelFixtures.h"
#include "game/screens/LevelOpponents.h"
#include "game/world/EffectTrees.h"
#include "game/world/ExitPortals.h"
#include "game/world/FallingPiece.h"
#include "game/world/ItemFigure.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {
namespace {
// Same discontinuity boundary as native ItemFigure, PlacedItems and EffectTrees.
constexpr f32 kCutDistance = 32;
struct Row {
    StageParticlePresentation::Phase phase{};
    mutable ParticleField field;
    mutable TreeParticles nodes;
    std::optional<TextureAnimator> textures;
    const TreeInfo* tree = nullptr; // immutable archive, never a live pool entry
    std::optional<u32> trailTexture;
    u32 sequence = 0;
    f32 frame = 0;
    f32 previousFrame = 0;
    f32 presentationFrame = 0;
    bool continuous = false;
    bool sameGeneration = false;
    bool advanced = false;
    bool nativeOnly = false;
    bool textureFollowsHold = false;
    bool cameraFacing = true;

    void draw(RenderDevice& device, const Mat4& clip, const CameraFrame& camera, f32 alpha) const {
        const f32 clockAlpha = nativeOnly ? -1 : alpha;
        const f32 particleAlpha = advanced ? clockAlpha : -1;
        f32 visualFrame = frame;
        if (clockAlpha >= 0 && continuous) {
            visualFrame = sameGeneration ? glm::mix(previousFrame, presentationFrame, clockAlpha)
                                         : presentationFrame;
        }
        if (textures) {
            const auto offset =
                textures->presentationOffset(textureFollowsHold ? particleAlpha : clockAlpha);
            if (tree != nullptr && (!textureFollowsHold || offset)) {
                textures->apply(nodes, *tree, sequence, visualFrame, offset);
            }
            field.clearTextureBlends();
            if (offset && trailTexture) {
                for (usize i = 0; i < textures->size(); ++i) {
                    if (!textures->keyed(i) && textures->slot(i) == *trailTexture) {
                        const auto sample = textures->motion(i, offset);
                        if (sample.frame != nullptr) {
                            field.setTextureBlend(0, *sample.frame, sample.nextFrame,
                                                  sample.frameBlend);
                        }
                    }
                }
            }
        }
        const auto basis = cameraFacing ? camera : CameraFrame{};
        nodes.draw(device, clip, basis.right, basis.up, particleAlpha);
        field.draw(device, clip, basis.right, basis.up, particleAlpha);
    }
};
bool accumulate(const ParticleField& field, usize& count) {
    const usize added = field.particleCount();
    if (added > StageParticlePresentation::kMaxParticles - count) {
        return false;
    }
    count += added;
    return field.checkpoint().valid();
}
bool validLight(const PointLight& light) {
    const auto finite = [](const Vec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    return finite(light.position) && finite(light.color) && std::isfinite(light.radius) &&
           light.radius >= 0 && std::isfinite(light.intensity);
}
} // namespace

struct StageParticlePresentation::Data {
    struct WorldMesh {
        TreeModel model;
        Mat4 placement{1};
        std::optional<FallingPiece> falling;
    };
    struct Decoration {
        std::unique_ptr<ItemFigure> figure;
        Phase phase = Phase::Fixtures;
        bool interpolated = false;
        bool cameraFacing = true;
    };
    u64 epoch = 0, tick = 0;
    std::weak_ptr<const void> lifetime;
    std::vector<Row> rows;
    std::vector<Decoration> decorations;
    std::vector<WorldMesh> worldMeshes;
    std::optional<StageSnapshotSampling::Swarm> swarm;
    std::optional<StageSnapshotSampling::Party> party;
    std::vector<PointLight> lights;
    f32 fallingRemainder = 0;
    f32 fallingSeconds = 0;
    bool worldAdvanced = false;
    usize particles = 0;
    usize count() const { return rows.size() + decorations.size() + worldMeshes.size(); }
    bool add(Row row) {
        if (count() >= kMaxRows || !accumulate(row.field, particles) ||
            !accumulate(row.nodes.field(), particles) || !row.nodes.checkpoint().valid() ||
            (row.textures && !row.textures->checkpoint().valid())) {
            return false;
        }
        rows.push_back(std::move(row));
        return true;
    }
};

StageParticlePresentation::StageParticlePresentation(std::unique_ptr<Data> data)
    : m_data(std::move(data)) {}
StageParticlePresentation::StageParticlePresentation(StageParticlePresentation&&) noexcept =
    default;
StageParticlePresentation&
StageParticlePresentation::operator=(StageParticlePresentation&&) noexcept = default;
StageParticlePresentation::~StageParticlePresentation() = default;

std::optional<std::vector<StageParticlePresentation::Figure>>
StageParticlePresentation::collectFigures(const LevelWorld& world, const LevelFixtures& fixtures,
                                          const LevelOpponents& opponents) {
    std::vector<Figure> result;
    const auto add = [&](const ItemFigure& figure, Phase phase, bool interpolated,
                         bool cameraFacing, bool retainMesh = false) {
        if (result.size() == kMaxRows) {
            return false;
        }
        result.push_back({&figure, interpolated, phase, cameraFacing, retainMesh});
        return true;
    };
    // LevelWorld::drawOpaque calls LevelTriggers::draw then Rotators::draw.
    for (usize i = 0; i < world.triggers().size(); ++i) {
        const auto* figure = world.triggers().figure(i);
        if (figure != nullptr && world.triggers().trigger(i).enabled &&
            !add(*figure, Phase::WorldFixtures, false, false)) {
            return std::nullopt;
        }
    }
    for (usize i = 0; i < world.rotators().size(); ++i) {
        if (const auto& pad = world.rotators().rotator(i).pad;
            pad && !add(*pad, Phase::WorldFixtures, false, false, true)) {
            return std::nullopt;
        }
    }
    // LevelFixtures::draw: revealed preview precedes its chest. Traps and
    // scenery use DepthWriting here, which does not draw ItemFigure particles.
    for (usize i = 0; i < fixtures.chests().size(); ++i) {
        const auto& chest = fixtures.chests().chest(i);
        if (chest.shown && !chest.gone) {
            if (chest.revealed && !add(chest.preview, Phase::SolidFixtures, true, true)) {
                return std::nullopt;
            }
            if (!add(chest.figure, Phase::SolidFixtures, true, true)) {
                return std::nullopt;
            }
        }
    }
    for (usize i = 0; i < fixtures.gates().size(); ++i) {
        const auto& gate = fixtures.gates().gate(i);
        if (gate.shown && !add(gate.figure, Phase::SolidFixtures, true, false)) {
            return std::nullopt;
        }
    }
    for (usize i = 0; i < fixtures.barrels().size(); ++i) {
        const auto& barrel = fixtures.barrels().barrel(i);
        if (barrel.shown && !barrel.gone &&
            !add(barrel.figure, Phase::SolidFixtures, true, false)) {
            return std::nullopt;
        }
    }
    // Safe rocks, rubble and ordinary generator bodies draw TreeModel directly;
    // they do not own/draw TreeParticles. Do not invent emitters from their nodes.
    for (usize i = 0; i < opponents.statues().count(); ++i) {
        if (!add(opponents.statues().figure(i), Phase::SolidFixtures, false, true)) {
            return std::nullopt;
        }
    }
    for (const auto& generator : opponents.generators().presentation()) {
        if (generator.bossFigure != nullptr &&
            !add(*generator.bossFigure, Phase::SolidFixtures, false, false)) {
            return std::nullopt;
        }
    }
    // LevelFixtures::drawEffects, after the secret portal effect pass.
    for (usize i = 0; i < fixtures.traps().size(); ++i) {
        const auto& trap = fixtures.traps().trap(i);
        if (trap.shown && !add(trap.figure, Phase::Fixtures, true, true)) {
            return std::nullopt;
        }
    }
    for (usize i = 0; i < fixtures.scenery().size(); ++i) {
        const auto& prop = fixtures.scenery().prop(i);
        if (prop.shown && !add(prop.figure, Phase::Fixtures, true, true, true)) {
            return std::nullopt;
        }
    }
    return result;
}

std::unique_ptr<StageParticlePresentation>
StageParticlePresentation::capture(u64 epoch, u64 tick,
                                   const std::shared_ptr<const void>& resourceLifetime,
                                   const Sources& sources) {
    if (epoch == 0 || !resourceLifetime || sources.fixtures.size() > kMaxRows ||
        sources.lights.size() > WorldLighting::kMostPoints ||
        !std::ranges::all_of(sources.lights, validLight)) {
        return nullptr;
    }
    auto result = std::make_unique<Data>();
    result->epoch = epoch;
    result->tick = tick;
    result->lifetime = resourceLifetime;
    result->lights.assign(sources.lights.begin(), sources.lights.end());
    if (sources.enemies != nullptr) {
        result->swarm = StageSnapshotSampling::captureSwarm(*sources.enemies);
    }
    if (!sources.party.empty()) {
        result->party = StageSnapshotSampling::captureParty(sources.party);
        if (!result->party) {
            return nullptr;
        }
    }

    // These owners draw outside WorldScene and FixtureCapture. TreeModel's
    // value copy retains all live CPU appearance/texture overrides (including
    // wall white flashes) without owning or allocating GPU resources. Only
    // immutable meshes/textures are borrowed under the picture lifetime token.
    const auto& falling = sources.world.fallingScenery();
    result->fallingRemainder = falling.frameRemainder();
    result->fallingSeconds = falling.updateSeconds();
    result->worldAdvanced = sources.world.particlesAdvanced();
    if (!std::isfinite(result->fallingRemainder) || !std::isfinite(result->fallingSeconds)) {
        return nullptr;
    }
    for (usize i = 0; i < falling.size(); ++i) {
        const auto& piece = falling.piece(i);
        if (piece.shown && piece.motion.visible) {
            if (result->count() >= kMaxRows) {
                return nullptr;
            }
            result->worldMeshes.push_back({piece.model, Mat4{1}, piece.motion});
        }
    }
    const auto& walls = sources.world.walls();
    for (usize i = 0; i < walls.size(); ++i) {
        if (walls.standing(i)) {
            if (result->count() >= kMaxRows) {
                return nullptr;
            }
            const auto& wall = walls.wall(i);
            result->worldMeshes.push_back({wall.model, wall.transform, std::nullopt});
        }
    }

    const auto& items = sources.world.placedItems();
    for (usize i = 0; i < items.size(); ++i) {
        const auto& item = items.item(i);
        if (!item.visible || item.particles.field().particleCount() == 0) {
            continue;
        }
        Row row;
        row.phase = Phase::Pickups;
        row.nodes = item.particles;
        row.tree = item.figure;
        if (const auto* textures = items.textureAnimator(item.archive)) {
            row.textures = *textures;
        }
        row.sequence = item.player.sequence();
        row.frame = item.player.frame();
        row.previousFrame = item.previousFrame;
        row.presentationFrame = item.player.presentationFrame();
        row.continuous =
            item.presentationCaptured && item.player.playing() &&
            glm::distance(Vec3{item.previousTransform[3]}, item.position) <= kCutDistance;
        row.sameGeneration = item.previousGeneration == item.player.generation();
        row.advanced = items.particlesAdvanced();
        if (!result->add(std::move(row))) {
            return nullptr;
        }
    }
    Row bursts;
    bursts.phase = Phase::Pickups;
    bursts.field = items.bursts();
    bursts.advanced = items.particlesAdvanced();
    if (!result->add(std::move(bursts))) {
        return nullptr;
    }
    Row ambient;
    ambient.phase = Phase::World;
    ambient.field = sources.world.particles();
    ambient.advanced = sources.world.particlesAdvanced();
    if (!result->add(std::move(ambient))) {
        return nullptr;
    }

    const auto figure = [&](const ItemFigure& item, bool interpolated, Phase phase,
                            bool cameraFacing) {
        if (phase != Phase::WorldFixtures && phase != Phase::SolidFixtures &&
            phase != Phase::Fixtures) {
            return false;
        }
        if (item.particles().field().particleCount() == 0) {
            return true;
        }
        if (!item.hasFigure()) {
            return false;
        }
        const auto state = item.checkpoint();
        if (!state) {
            return false;
        }
        Row row;
        row.phase = phase;
        row.cameraFacing = cameraFacing;
        row.nodes = item.particles();
        row.tree = item.presentation().tree;
        row.textures = item.textureAnimator();
        row.sequence = state->textureSequence;
        row.frame = static_cast<f32>(state->textureFrame);
        row.previousFrame = state->previousFrame;
        AnimationPlayer animation;
        const TreeSequenceInfo* sequence = nullptr;
        if (state->animation.playing) {
            if (row.tree == nullptr || state->animation.sequence >= row.tree->sequences.size()) {
                return false;
            }
            sequence = &row.tree->sequences[state->animation.sequence];
        }
        if (!animation.restore(state->animation, sequence)) {
            return false;
        }
        row.presentationFrame = animation.presentationFrame();
        row.continuous =
            state->presentationCaptured && !state->holdPose && state->animation.playing;
        row.sameGeneration = state->previousGeneration == state->animation.generation;
        row.advanced = state->presentationAdvanced;
        row.nativeOnly = !interpolated;
        return result->add(std::move(row));
    };
    if (sources.portals != nullptr) {
        for (usize i = 0; i < sources.portals->size(); ++i) {
            const auto& portal = sources.portals->portal(i);
            if (portal.secret && !portal.consumed &&
                !figure(portal.icon, false, Phase::Fixtures, true)) {
                return nullptr;
            }
        }
    }
    for (const auto& entry : sources.fixtures) {
        if (entry.figure == nullptr) {
            return nullptr;
        }
        if (entry.retainMesh) {
            if (entry.phase != Phase::WorldFixtures && entry.phase != Phase::Fixtures) {
                return nullptr;
            }
            // NoGeometry scenery remains a valid, genuinely unbound owner.
            if (!entry.figure->hasFigure()) {
                continue;
            }
            const auto state = entry.figure->checkpoint();
            auto copy = state ? entry.figure->restored(*state) : nullptr;
            if (!copy || result->count() >= kMaxRows ||
                !accumulate(copy->particles().field(), result->particles)) {
                return nullptr;
            }
            result->decorations.push_back(
                {std::move(copy), entry.phase, entry.interpolated, entry.cameraFacing});
        } else if (!figure(*entry.figure, entry.interpolated, entry.phase, entry.cameraFacing)) {
            return nullptr;
        }
    }

    const auto effects = [&](const EffectTrees& pool, Phase phase, bool outerAdvanced) {
        for (usize i = 0; i < pool.count(); ++i) {
            const auto& effect = pool.effect(i);
            if (effect.trails.particleCount() == 0 &&
                effect.particles.field().particleCount() == 0) {
                continue;
            }
            Row row;
            row.phase = phase;
            row.field = effect.trails;
            row.nodes = effect.particles;
            row.tree = effect.tree;
            row.trailTexture = effect.particleTextureSlot;
            if (const auto* textures = pool.textureAnimator(effect.archive, effect.lenders)) {
                row.textures = *textures;
            }
            row.sequence = effect.player.sequence();
            row.frame = effect.player.frame();
            row.previousFrame = effect.previousFrame;
            row.presentationFrame = effect.player.presentationFrame();
            row.continuous = effect.presentationCaptured && effect.previousTree == effect.tree &&
                             effect.tree != nullptr && effect.player.playing() &&
                             glm::distance(Vec3{effect.previousTransform[3]},
                                           Vec3{effect.transform()[3]}) <= kCutDistance;
            row.sameGeneration = effect.previousGeneration == effect.player.generation();
            row.advanced = pool.presentationAdvanced() && outerAdvanced;
            row.nativeOnly = !outerAdvanced;
            row.textureFollowsHold = true;
            if (!result->add(std::move(row))) {
                return false;
            }
        }
        return true;
    };
    if (!effects(sources.projectiles, Phase::Projectiles, sources.projectilesAdvanced) ||
        !effects(sources.effects, Phase::Effects, true)) {
        return nullptr;
    }
    return std::unique_ptr<StageParticlePresentation>{
        new StageParticlePresentation(std::move(result))};
}

bool StageParticlePresentation::matches(u64 epoch, u64 tick) const {
    return m_data && !m_data->lifetime.expired() && m_data->epoch == epoch && m_data->tick == tick;
}
bool StageParticlePresentation::matches(u64 epoch, u64 tick,
                                        const std::weak_ptr<const void>& resources) const {
    return matches(epoch, tick) && !resources.expired() &&
           !m_data->lifetime.owner_before(resources) && !resources.owner_before(m_data->lifetime);
}
usize StageParticlePresentation::particleCount() const {
    return m_data ? m_data->particles : 0;
}
const StageSnapshotSampling::Swarm* StageParticlePresentation::swarm() const {
    return m_data && !m_data->lifetime.expired() && m_data->swarm ? &*m_data->swarm : nullptr;
}
const StageSnapshotSampling::Party* StageParticlePresentation::party() const {
    return m_data && !m_data->lifetime.expired() && m_data->party ? &*m_data->party : nullptr;
}
std::span<const PointLight> StageParticlePresentation::lights() const {
    return m_data && !m_data->lifetime.expired() ? std::span<const PointLight>{m_data->lights}
                                                 : std::span<const PointLight>{};
}
bool StageParticlePresentation::draw(Phase phase, RenderDevice& device, const Mat4& clip,
                                     const CameraFrame& camera, f32 alpha) const {
    if (!m_data || m_data->lifetime.expired() || !std::isfinite(alpha) || alpha < -1 || alpha > 1 ||
        phase > Phase::Effects) {
        return false;
    }
    for (const auto& row : m_data->rows) {
        if (row.phase == phase) {
            row.draw(device, clip, camera, alpha);
        }
    }
    return true;
}
bool StageParticlePresentation::drawDecorations(Phase phase, RenderDevice& device, const Mat4& clip,
                                                const WorldLighting& lighting,
                                                const CameraFrame& camera, f32 alpha) const {
    if (!m_data || m_data->lifetime.expired() || !std::isfinite(alpha) || alpha < -1 || alpha > 1 ||
        phase > Phase::Effects) {
        return false;
    }
    if (phase == Phase::WorldScenery) {
        const f32 fraction = FallingPiece::presentationFraction(
            m_data->fallingRemainder, m_data->fallingSeconds, m_data->worldAdvanced ? alpha : -1);
        for (const auto& entry : m_data->worldMeshes) {
            const Mat4 placement = entry.falling
                                       ? itemPlacement(entry.falling->presentedPosition(fraction),
                                                       entry.falling->presentedRotation(fraction))
                                       : entry.placement;
            entry.model.draw(device, clip, placement, lighting);
        }
    }
    for (const auto& entry : m_data->decorations) {
        std::optional<TreeModel::Pass> pass;
        if (entry.phase == Phase::WorldFixtures && phase == Phase::WorldFixtures) {
            pass = TreeModel::Pass::All;
        } else if (entry.phase == Phase::Fixtures) {
            if (phase == Phase::SolidFixtures) {
                pass = TreeModel::Pass::DepthWriting;
            } else if (phase == Phase::Fixtures) {
                pass = TreeModel::Pass::Effects;
            }
        }
        if (pass) {
            entry.figure->draw(device, clip, lighting, 1, 1, entry.cameraFacing ? &camera : nullptr,
                               *pass, entry.interpolated ? alpha : -1);
        }
    }
    return true;
}
} // namespace gdl::game
