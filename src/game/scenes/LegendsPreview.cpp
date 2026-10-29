#include "game/scenes/LegendsPreview.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

#include "engine/core/Error.h"

#include "game/characters/LegendsVisit.h"
#include "game/config/ControlProfiles.h"
#include "game/party/PartyMotion.h"

namespace gdl::game {

namespace {
constexpr f64 kTickSeconds = 1.0 / 60.0;
constexpr f32 kFollowDistance = 32.0f;
constexpr f32 kFollowPitch = kPi * 0.25f;
bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

void LegendsPreview::open(RenderDevice& device, const formats::LegendsMap& map,
                          std::unique_ptr<LegendsSceneAdapter> adapter) {
    std::vector<Surface> surfaces;
    for (const auto& source : map.textures) {
        Surface surface;
        if (!source.image.pixels.empty()) {
            const auto& image = source.image;
            if (image.width == 0 || image.height == 0 ||
                u64{image.width} * image.height * 4 != image.pixels.size()) {
                throw FormatError("Legends preview: invalid texture dimensions");
            }
            for (usize i = 3; i < image.pixels.size(); i += 4) {
                surface.hasTransparency |= image.pixels[i] < 255;
                surface.softAlpha |= image.pixels[i] > 0 && image.pixels[i] < 255;
            }
            if (!adapter) {
                const TextureDesc description{
                    .width = image.width,
                    .height = image.height,
                    .wrap = source.clampU ? TextureWrap::ClampToEdge : TextureWrap::Repeat,
                    .wrapV = source.clampV ? TextureWrap::ClampToEdge : TextureWrap::Repeat,
                    .generateMipmaps = true};
                surface.texture = device.createTexture(description, image.pixels);
            }
        }
        surfaces.push_back(std::move(surface));
    }
    // Sharing a texture does not make two native objects one animation target.
    std::map<std::pair<usize, u32>, ImmediateBatch> geometry;
    Vec3 low{std::numeric_limits<f32>::max()};
    Vec3 high{std::numeric_limits<f32>::lowest()};
    for (usize objectIndex = 0; objectIndex < map.objects.size(); ++objectIndex) {
        const auto& object = map.objects[objectIndex];
        for (const auto& part : object.mesh.parts) {
            if (part.indices.size() % 3 != 0) {
                throw FormatError("Legends preview: incomplete triangle");
            }
            auto [entry, inserted] = geometry.try_emplace(std::pair{objectIndex, part.texture});
            auto& batch = entry->second;
            if (inserted) {
                batch.begin(PrimitiveTopology::TriangleList);
            }
            Vec2 uvScale{1.0f};
            const Texture* texture = adapter ? adapter->texture(part.texture).texture : nullptr;
            if (!texture && part.texture < surfaces.size()) {
                texture = surfaces[part.texture].texture.get();
            }
            if (texture) {
                uvScale = Vec2{1.0f / texture->width(), 1.0f / texture->height()};
            }
            if (part.texture < map.textures.size()) {
                const auto& source = map.textures[part.texture];
                if (source.width != 0 && source.height != 0) {
                    uvScale = Vec2{1.0f / source.width, 1.0f / source.height};
                }
            }
            for (const auto index : part.indices) {
                if (index >= object.mesh.vertices.size()) {
                    throw FormatError("Legends preview: invalid vertex index");
                }
                const auto& vertex = object.mesh.vertices[index];
                const Vec3 position{object.transform * Vec4{vertex.position, 1.0f}};
                if (!finite(position) || !std::isfinite(vertex.uv.x) ||
                    !std::isfinite(vertex.uv.y)) {
                    throw FormatError("Legends preview: non-finite vertex");
                }
                low = glm::min(low, position);
                high = glm::max(high, position);
                batch.vertex(position, vertex.color.withAlpha(255), vertex.uv * uvScale);
            }
        }
    }
    std::vector<Batch> batches;
    usize triangleCount = 0;
    for (auto& [identity, batch] : geometry) {
        batch.end();
        if (batch.empty()) {
            continue;
        }
        Vec3 batchLow{std::numeric_limits<f32>::max()};
        Vec3 batchHigh{std::numeric_limits<f32>::lowest()};
        for (const auto& vertex : batch.triangles()) {
            batchLow = glm::min(batchLow, vertex.position);
            batchHigh = glm::max(batchHigh, vertex.position);
        }
        triangleCount += batch.vertexCount() / 3;
        batches.push_back(
            {std::move(batch), identity.second, identity.first, (batchLow + batchHigh) * 0.5f});
        auto& built = batches.back();
        const auto& object = map.objects[identity.first];
        built.presentation = LegendsWorldPresentation::n64(object.nativeDrawFlags);
        if (built.presentation.facing == 1) {
            // Geometry remains world-baked. Rebase only the native facing
            // objects around their own authored anchor, never the map origin.
            built.authoredPlacement = object.transform;
            const f32 determinant = glm::determinant(object.transform);
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8f) {
                throw FormatError("Legends preview: singular facing-object placement");
            }
            built.inversePlacement = glm::inverse(object.transform);
        }
    }
    if (batches.empty()) {
        throw FormatError("Legends preview: no supported geometry");
    }
    const Vec3 centre = (low + high) * 0.5f;
    const Vec3 span = high - low;
    const f32 distance = std::max({span.x, span.y, span.z, 20.0f});
    if (!finite(centre) || !finite(span) || !std::isfinite(distance * 8.0f)) {
        throw FormatError("Legends preview: world bounds exceed the camera range");
    }
    WorldCamera camera;
    camera.position = centre + Vec3{0, distance * 0.65f, -distance * 0.65f};
    camera.pitch = kPi * 0.25f;
    m_far = std::max(WorldCamera::kFar, distance * 8.0f);
    m_batches = std::move(batches);
    m_surfaces = std::move(surfaces);
    m_adapter = std::move(adapter);
    m_triangleCount = triangleCount;
    m_camera.start(camera);
    m_status.reset();
    m_figure.reset();
    m_traversal.reset();
    m_nativeCamera.reset();
    m_hasClip = false;
}

void LegendsPreview::openPlayable(RenderDevice& device, const formats::LegendsMap& map,
                                  const std::filesystem::path& characterRoot,
                                  const PartyMember& member,
                                  std::unique_ptr<LegendsSceneAdapter> adapter) {
    // Fallen party records contain their restored entry save, not the parked
    // actor's live save. Such a visit could never safely return its rewards.
    if (member.fallen) {
        throw FormatError("Legends traversal: select a living character before entering");
    }
    ClassDataSet classes;
    classes.load(characterRoot / "pdata");
    auto traversal = std::make_unique<LegendsTraversal>();
    traversal->open(map, member.player, member.save, classes.stats(member.save.character));
    std::optional<LegendsCamera> nativeCamera;
    if (adapter && adapter->cameraConfig()) {
        nativeCamera.emplace();
        nativeCamera->reset(*adapter->cameraConfig(), traversal->actor().position());
    }
    auto figure = PlayerFigure::load(device, characterRoot, member.save, false);
    if (!figure) {
        throw FormatError("Legends traversal: selected character artwork could not be loaded");
    }
    auto status = std::make_unique<StatusBoxPainter>();
    if (status->load(device, characterRoot, nullptr)) {
        status->preloadStatus();
    }
    open(device, map, std::move(adapter));
    m_traversal = std::move(traversal);
    m_figure = std::move(figure);
    m_status = std::move(status);
    m_before = member.save;
    const auto* stats = classes.stats(member.save.character);
    m_powerupTime = stats ? stats->powerupTime : 1;
    m_followCamera = WorldCamera{};
    m_followCamera.pitch = kFollowPitch;
    m_previousPosition = m_traversal->actor().position();
    m_previousYaw = m_traversal->actor().yaw();
    m_followCamera.position =
        m_traversal->actor().followPoint() - m_followCamera.forward() * kFollowDistance;
    m_nativeCamera = std::move(nativeCamera);
    if (m_nativeCamera) {
        m_followCamera = m_nativeCamera->camera();
    }
    m_elapsed = 0;
    m_cursorInput = CursorInput{};
}

void LegendsPreview::close() {
    m_status.reset();
    m_figure.reset();
    m_traversal.reset();
    m_nativeCamera.reset();
    m_camera.stop();
    m_batches.clear();
    m_surfaces.clear();
    m_adapter.reset();
    m_triangleCount = 0;
    m_elapsed = 0;
    m_hasClip = false;
}

bool LegendsPreview::transferProgress(CharacterSave& live) {
    if (!m_traversal || !commitLegendsVisit(live, m_before, m_traversal->actor().save())) {
        return false;
    }
    m_before = live;
    return true;
}

void LegendsPreview::update(const Input& input, f32 seconds, bool consoleCapturesInput) {
    if (!consoleCapturesInput && !playable()) {
        m_camera.update(input, seconds);
        if (m_adapter) {
            m_adapter->update(seconds);
        }
    }
}

void LegendsPreview::updatePlayable(const Input& input, f32 seconds, bool consoleCapturesInput,
                                    const GameConfig& config) {
    if (!playable() || !std::isfinite(seconds) || seconds < 0) {
        return;
    }
    if (consoleCapturesInput) {
        // Preserve the current render fraction and both pose endpoints. Resetting
        // these on a partial frame snaps the camera backward and the actor forward.
        return;
    }
    const s32 player = m_traversal->actor().player();
    const auto source = controlDevices(config, input)[static_cast<usize>(player)];
    const auto& bindings = playBindings(config, player);
    const auto move = readMoveInput(input, bindings, source.keyboard, source.pad);
    std::optional<Vec3> aim;
    if (m_cursorInput.update(input, bindings, source.keyboard, source.pad) && m_hasClip) {
        aim = cursorAimDirection(Vec2{input.pointer().x, input.pointer().y}, m_lastClip,
                                 m_traversal->actor().position());
    }
    const bool freeMove = source.keyboard && config.controls[static_cast<usize>(player)].scheme ==
                                                 KeyboardScheme::Robotron;
    m_elapsed += std::min(seconds, 0.25f);
    while (m_elapsed + 1.0e-8 >= kTickSeconds) {
        m_elapsed = std::max(0.0, m_elapsed - kTickSeconds);
        m_previousPosition = m_traversal->actor().position();
        m_previousYaw = m_traversal->actor().yaw();
        m_figure->capturePresentation();
        if (m_adapter) {
            m_adapter->capturePresentation();
            m_adapter->update(static_cast<f32>(kTickSeconds));
        }
        const f32 cameraYaw = m_nativeCamera ? m_nativeCamera->camera().yaw : m_followCamera.yaw;
        const PlayerActor before = m_traversal->actor();
        m_traversal->step(move, cameraYaw, static_cast<f32>(kTickSeconds), aim, freeMove);
        if (m_adapter) {
            auto& actor = m_traversal->actor();
            const auto resolved = m_adapter->resolveMovement(m_previousPosition, actor.position(),
                                                             actor.radius(), actor.height());
            if (resolved != actor.position()) {
                m_traversal->correctSceneryStep(before, resolved, static_cast<f32>(kTickSeconds));
            }
        }
        if (m_nativeCamera) {
            m_nativeCamera->update(m_traversal->actor().position(), static_cast<f32>(kTickSeconds));
        }
        if (m_adapter) {
            m_adapter->collect(m_traversal->actor(), m_powerupTime,
                               config.combat.autoActivateItems);
        }
        const auto& actor = m_traversal->actor();
        const Vec3 displacement = actor.position() - m_previousPosition;
        const bool moved = glm::length(Vec2{displacement.x, displacement.z}) > 0.0001f;
        const f32 heading = std::atan2(displacement.x, displacement.z);
        m_figure->setStrafe(aim && moved ? PartyMotion::strafeWayOf(heading, actor.yaw())
                                         : StrafeWay::None);
        m_figure->animate(actor.moving() ? move.magnitude : 0.0f, 1,
                          static_cast<f32>(kTickSeconds));
    }
    const auto& actor = m_traversal->actor();
    const f32 blend = static_cast<f32>(m_elapsed / kTickSeconds);
    const Vec3 position = glm::mix(m_previousPosition, actor.position(), blend);
    if (m_nativeCamera) {
        m_followCamera = m_nativeCamera->sample(static_cast<f32>(m_elapsed));
    } else {
        m_followCamera.position = position + (actor.followPoint() - actor.position()) -
                                  m_followCamera.forward() * kFollowDistance;
    }
}

void LegendsPreview::drawWorld(RenderDevice& device, const Mat4& transform, const WorldCamera& view,
                               TreeModel::Pass pass, f32 presentationAlpha) const {
    const auto surfaceFor = [&](const Batch& batch) -> const Surface* {
        return batch.texture < m_surfaces.size() ? &m_surfaces[batch.texture] : nullptr;
    };
    const auto draw = [&](const Batch& batch) {
        const auto* surface = surfaceFor(batch);
        const auto adapted =
            m_adapter ? m_adapter->texture(batch.texture) : LegendsSceneAdapter::TextureView{};
        const bool soft = adapted.texture ? adapted.softAlpha : surface && surface->softAlpha;
        const bool alpha =
            adapted.texture ? adapted.hasTransparency : surface && surface->hasTransparency;
        DrawState state;
        state.blend = soft || batch.presentation.forceAlpha ? BlendMode::Alpha : BlendMode::Opaque;
        state.depthWrite = !soft && batch.presentation.depthWrite;
        state.depthTest = batch.presentation.depthTest;
        state.alphaTest = alpha ? DrawState::kTranslucentAlphaTest : 0;
        state.uvOffset = adapted.offset;
        state.uvScale = adapted.scale;
        state.mipmaps = true;
        const Vec3 offset =
            m_adapter ? m_adapter->worldObjectOffset(batch.object, presentationAlpha) : Vec3{0};
        const Mat4 movingClip = glm::translate(transform, offset);
        device.draw(batch.geometry,
                    adapted.texture               ? *adapted.texture
                    : surface && surface->texture ? *surface->texture
                                                  : device.whiteTexture(),
                    batch.presentation.facing == 1
                        ? movingClip * batch.presentation.placement(batch.authoredPlacement, view) *
                              batch.inversePlacement
                        : movingClip,
                    state);
    };
    std::vector<const Batch*> translucent;
    for (const auto& batch : m_batches) {
        if (batch.presentation.hidden ||
            (m_adapter && !m_adapter->worldObjectVisible(batch.object))) {
            continue;
        }
        const auto* surface = surfaceFor(batch);
        const auto adapted =
            m_adapter ? m_adapter->texture(batch.texture) : LegendsSceneAdapter::TextureView{};
        if (batch.presentation.sortable || batch.presentation.forceAlpha ||
            (adapted.texture ? adapted.softAlpha : surface && surface->softAlpha)) {
            translucent.push_back(&batch);
        } else if (pass != TreeModel::Pass::Blended) {
            draw(batch);
        }
    }
    const auto frame = CameraFrame::of(view);
    if (m_adapter && pass != TreeModel::Pass::Blended) {
        m_adapter->draw(device, transform, frame, TreeModel::Pass::Opaque, presentationAlpha);
    }
    // Keep translucent native scenery after players, shadows and opaque fixtures.
    if (pass != TreeModel::Pass::Opaque) {
        const Vec3 forward = view.forward();
        std::stable_sort(
            translucent.begin(), translucent.end(), [&](const Batch* a, const Batch* b) {
                const auto centre = [&](const Batch* batch) {
                    return batch->centre +
                           (m_adapter
                                ? m_adapter->worldObjectOffset(batch->object, presentationAlpha)
                                : Vec3{0});
                };
                return glm::dot(centre(a) - view.position, forward) >
                       glm::dot(centre(b) - view.position, forward);
            });
        for (const auto* batch : translucent) {
            draw(*batch);
        }
        if (m_adapter) {
            m_adapter->draw(device, transform, frame, TreeModel::Pass::Blended, presentationAlpha);
        }
    }
}

void LegendsPreview::render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth,
                            f32 frameHeight, f32 horizontalFov) const {
    if (!isOpen() || !std::isfinite(frameWidth) || !std::isfinite(frameHeight) || frameWidth <= 0 ||
        frameHeight <= 0 || !std::isfinite(horizontalFov) || horizontalFov <= 0 ||
        horizontalFov >= kPi) {
        return;
    }
    Mat4 perspective = WorldCamera::projection(horizontalFov, frameWidth / frameHeight);
    perspective[2][2] =
        WorldCamera::kDepthRange * WorldCamera::kNear / (WorldCamera::kNear - m_far);
    perspective[3][2] =
        WorldCamera::kDepthRange * WorldCamera::kNear * m_far / (m_far - WorldCamera::kNear);
    const Mat4 transform = frameProjection * WorldCamera::frameMapping(frameWidth, frameHeight) *
                           perspective * camera().view();
    m_lastClip = transform;
    m_hasClip = true;
    const auto& view = camera();
    const auto frame = CameraFrame::of(view);
    const f32 presentationAlpha = playable() ? static_cast<f32>(m_elapsed / kTickSeconds) : -1;
    drawWorld(device, transform, view, TreeModel::Pass::Opaque, presentationAlpha);
    if (playable()) {
        const auto& actor = m_traversal->actor();
        const f32 blend = static_cast<f32>(m_elapsed / kTickSeconds);
        const Vec3 position = glm::mix(m_previousPosition, actor.position(), blend);
        const f32 yaw = m_previousYaw + std::remainder(actor.yaw() - m_previousYaw, kTwoPi) * blend;
        const Mat4 base = glm::rotate(glm::translate(Mat4{1}, position), yaw, Vec3{0, 1, 0});
        const Mat4 body = PlayerFigure::bodyPlacement(base, actor.save(), PowerupEffects{});
        const WorldLighting lighting;
        if (const auto floor = m_traversal->collision().floorAt(position, 1.0f, 10.0f)) {
            m_figure->drawShadow(device, transform, view.position,
                                 Vec3{position.x, floor->y, position.z}, floor->normal, lighting,
                                 1);
        }
        m_figure->draw(device, transform, body, lighting, 1, false, &frame, blend);
    }
    drawWorld(device, transform, view, TreeModel::Pass::Blended, presentationAlpha);
    if (playable() && m_status && m_status->loaded()) {
        Canvas canvas;
        canvas.begin(
            device, makeVirtualScreenTransform(frameProjection, 512, 384, frameWidth, frameHeight));
        const auto& save = m_traversal->actor().save();
        const auto& inventory = save.progress().inventory;
        StatusBoxView status;
        status.mode = StatusBoxView::Mode::Status;
        status.active = true;
        status.classIndex = save.character;
        status.color = save.color;
        status.name = save.name;
        status.level = experienceLevel(save.experience());
        status.gold = save.gold;
        status.health = save.health();
        status.keys = inventory.keys;
        status.potions = static_cast<s32>(inventory.potions.size());
        status.potionKind = inventory.nextPotion();
        m_status->draw(canvas, 0, status, true);
        canvas.end();
    }
}

} // namespace gdl::game
