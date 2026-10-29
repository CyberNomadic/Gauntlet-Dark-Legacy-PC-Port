#include "game/scenes/LegendsLevel.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "formats/LegendsN64Texture.h"

namespace gdl::game {
namespace {
Vec3 partyFeet(std::span<const CameraSubject> subjects) {
    Vec3 sum{0};
    for (const auto& subject : subjects) {
        sum += subject.feet;
    }
    return subjects.empty() ? sum : sum / static_cast<f32>(subjects.size());
}
} // namespace

LegendsLevel::LegendsLevel(RenderDevice& device, const formats::LegendsMap& map,
                           std::unique_ptr<LegendsSceneAdapter> adapter,
                           const std::filesystem::path& directory,
                           const std::filesystem::path& characterRoot)
    : m_directory(directory.lexically_normal()) {
    if (m_directory.filename().empty()) {
        m_directory = m_directory.parent_path();
    }
    if (!adapter) {
        throw std::invalid_argument("Legends level requires native resources");
    }
    m_world.open(device, map, std::move(adapter));
    if (!map.starts.empty()) {
        m_playerStart = map.starts.front().position;
    }
    m_classes.load(characterRoot / "pdata");
    if (platform() == LegendsPlatform::N64) {
        const auto nativeRoot = m_directory.parent_path().parent_path();
        m_sounds.openN64(nativeRoot);
        if (const auto program = AssetLocator{nativeRoot}.find("_program/game.bin")) {
            const auto bytes = readFile(*program);
            m_trapDamageScale =
                formats::legendsN64TrapScale(bytes, paths::utf8(m_directory.filename()))
                    .value_or(1);
            if (const auto overlay = AssetLocator{nativeRoot}.find("_program/Boss.bin")) {
                m_boss =
                    legendsN64Boss(paths::utf8(m_directory.filename()), bytes, readFile(*overlay));
            }
        }
        // e0072b6c loads CHROMESILVER from the native static archive;
        // e004e1a8/e0030584 replace every departing character's material with it.
        const AssetLocator files{m_directory.parent_path().parent_path() / "static"};
        if (const auto objects = files.find("objects.wad")) {
            const auto textures = formats::decodeLegendsN64Textures(readFile(*objects));
            const auto found = std::ranges::find(textures, std::string{"CHROMESILVER"},
                                                 &formats::LegendsN64Texture::name);
            if (found != textures.end() && !found->external && !found->image.pixels.empty()) {
                const TextureDesc description{
                    .width = found->image.width,
                    .height = found->image.height,
                    .wrap = found->clampU ? TextureWrap::ClampToEdge : TextureWrap::Repeat,
                    .wrapV = found->clampV ? TextureWrap::ClampToEdge : TextureWrap::Repeat,
                    .generateMipmaps = true};
                m_departureSkin = device.createTexture(description, found->image.pixels);
            }
        }
    }
}

void LegendsLevel::capturePresentation() {
    m_previousDepartureSeconds = m_departureSeconds;
    m_world.adapter()->capturePresentation();
}

void LegendsLevel::update(f32 seconds, bool timeStopped) {
    if (m_bossDefeated && m_boss && !m_destination && std::isfinite(seconds) && seconds > 0) {
        m_bossDepartureSeconds += seconds;
        if (m_bossDepartureSeconds >= m_boss->departureSeconds) {
            m_destination = m_boss->destination;
        }
    }
    if (m_destination && platform() == LegendsPlatform::N64 && std::isfinite(seconds) &&
        seconds > 0) {
        m_departureSeconds = std::min(m_departureSeconds + seconds,
                                      static_cast<f64>(kLegendsPortalDepartureSeconds));
    }
    // Scenery clocks are independent of Stop Time's enemy freeze, as in GDL.
    m_world.adapter()->update(seconds);
    m_world.adapter()->updateTraps(seconds, timeStopped);
    if (m_collision) {
        m_world.adapter()->updateBossRewards(seconds, *m_collision);
    }
    for (auto& blast : m_blasts) {
        blast.step(seconds);
    }
    std::erase_if(m_blasts, [](const auto& blast) { return !blast.alive(); });
}

void LegendsLevel::bindCollision(WorldCollision& collision) const {
    m_collision = &collision;
    m_world.adapter()->bindCollision(collision);
}

void LegendsLevel::syncCollision(WorldCollision& collision) const {
    m_world.adapter()->syncCollision(collision);
}

std::optional<ImportedDeparture> LegendsLevel::departure(f32 alpha) const {
    if (!m_destination || platform() != LegendsPlatform::N64) {
        return std::nullopt;
    }
    const f64 elapsed = alpha < 0 || !std::isfinite(alpha)
                            ? m_departureSeconds
                            : std::lerp(m_previousDepartureSeconds, m_departureSeconds,
                                        static_cast<f64>(std::clamp(alpha, 0.0f, 1.0f)));
    const auto sample = legendsPortalDeparture(static_cast<f32>(elapsed));
    return ImportedDeparture{sample.yaw, sample.scale, m_departureSkin.get(), sample.finished};
}

void LegendsLevel::draw(RenderDevice& device, const Mat4& clip, const WorldCamera& view,
                        TreeModel::Pass pass, f32 alpha) const {
    m_world.drawWorld(device, clip, view, pass, alpha);
}

void LegendsLevel::resetCamera(std::span<const CameraSubject> subjects) {
    if (const auto& config = m_world.adapter()->cameraConfig(); config && !subjects.empty()) {
        m_camera.emplace();
        m_camera->reset(*config, partyFeet(subjects));
    }
}

void LegendsLevel::followCamera(std::span<const CameraSubject> subjects, f32 seconds) {
    if (m_destination && platform() == LegendsPlatform::N64) {
        return; // The native spin changes neither player feet nor its camera target.
    }
    if (m_camera && !subjects.empty()) {
        m_cameraTickSeconds = seconds;
        m_camera->update(partyFeet(subjects), seconds);
    }
}

std::optional<WorldCamera> LegendsLevel::camera(f32 alpha) const {
    if (!m_camera) {
        return {};
    }
    return alpha < 0 || (m_destination && platform() == LegendsPlatform::N64)
               ? m_camera->camera()
               : m_camera->sample(std::clamp(alpha, 0.0f, 1.0f) * m_cameraTickSeconds);
}

std::optional<Vec3> LegendsLevel::cameraAttention() const {
    return m_camera ? std::optional<Vec3>{m_camera->attention()} : std::nullopt;
}

Vec3 LegendsLevel::resolveMovement(const Vec3& from, const Vec3& to, f32 radius, f32 height) const {
    return m_world.adapter()->resolveMovement(from, to, radius, height);
}

void LegendsLevel::initializeParty(std::span<const PartyMember> party) {
    std::vector<const CharacterSave*> saves;
    for (const auto& member : party) {
        if (!member.fallen) {
            saves.push_back(&member.save);
        }
    }
    m_world.adapter()->synchronizeProgress(saves);
}

void LegendsLevel::completeTravel(std::span<PartyMember> party) const {
    const auto completed = departure();
    if (!m_destination || !completed || !completed->finished || adapter().isTower()) {
        return;
    }
    // Native e006bd0c/e006bd84 records the departing stage for the living party,
    // never the destination. This is called only by the app's completed travel
    // route; developer map commands neither call it nor manufacture completion.
    const auto identity = LegendsProgress::mapIdentity(paths::utf8(m_directory.filename()));
    if (!identity) {
        return;
    }
    for (auto& member : party) {
        if (!member.fallen && member.player >= 0 && member.player < 4 &&
            member.save.character >= 0 &&
            static_cast<usize>(member.save.character) < member.save.classes.size()) {
            member.save.progress().legendsProgress(platform()).recordCompleted(*identity);
        }
    }
}

void LegendsLevel::updateParty(std::span<PlayerActor*> players, f32 seconds,
                               bool activatePowerups) {
    if (m_destination) {
        return;
    }
    std::vector<LegendsPortalVisitor> visitors;
    std::vector<CharacterSave*> recipients;
    std::vector<const CharacterSave*> progress;
    for (auto* actor : players) {
        if (actor) {
            recipients.push_back(&actor->save());
            progress.push_back(&actor->save());
        }
    }
    m_world.adapter()->synchronizeProgress(progress);
    bool readScroll = false;
    for (auto* actor : players) {
        if (!actor) {
            continue;
        }
        // The character's native effect duration is applied by the shared powerup
        // pipeline; pickup validation itself stays in the native-map item adapter.
        const auto* stats = m_classes.stats(actor->save().character);
        if (!readScroll) {
            m_world.adapter()->collect(
                *actor, stats ? stats->powerupTime : 1, activatePowerups,
                [&](const LegendsPickup& pickup, const ItemTaking& taking) {
                    readScroll |= !pickup.scrollPages.empty();
                    m_pickups.push_back({actor->player(), pickup.award ? ItemOffer{} : pickup.offer,
                                         pickup.name, taking, pickup.scrollPages});
                },
                recipients);
        }
        visitors.push_back({static_cast<u8>(actor->player()), actor->position(), true,
                            actor->radius(), actor->height()});
    }
    m_world.adapter()->synchronizeProgress(progress);
    m_visitors = visitors;
    if (!readScroll) {
        m_world.adapter()->touchTraps(visitors);
        m_world.adapter()->touchSwitches(visitors);
        m_destination = m_world.adapter()->tickPortals(visitors, seconds);
    }
}

std::vector<ImportedPickup> LegendsLevel::takePickups() {
    return std::exchange(m_pickups, {});
}

std::span<const ImportedGenerator> LegendsLevel::importedGenerators() const {
    return m_world.adapter()->generators();
}

void LegendsLevel::updateGenerators(std::span<const ImportedGeneratorState> states) {
    m_world.adapter()->updateGenerators(states);
}

std::vector<ImportedTarget> LegendsLevel::targets() const {
    std::vector<ImportedTarget> result;
    for (const auto& target : m_world.adapter()->containerTargets()) {
        result.push_back({target.id, target.base, target.radius, target.height, target.breakable,
                          !target.breakable});
    }
    if (m_boss) {
        for (const auto& target : adapter().arenaTargets()) {
            if (!target.active || target.index >= m_boss->combat.arenaRocks.size()) {
                continue;
            }
            const auto& definition = m_boss->combat.arenaRocks[target.index];
            result.push_back({kImportedArenaBase + static_cast<s32>(target.index),
                              Vec3{target.placement[3]}, definition.radius, definition.height, true,
                              true});
        }
    }
    return result;
}

bool LegendsLevel::strike(s32 id, f32 power, u32 flags, s32 player) {
    if (id >= kImportedArenaBase) {
        return m_world.adapter()->strikeArenaRock(static_cast<usize>(id - kImportedArenaBase),
                                                  power);
    }
    return m_world.adapter()->strikeContainer(id, power, flags, player);
}

std::vector<Obstacle> LegendsLevel::obstacles() const {
    auto result = m_world.adapter()->containerObstacles();
    const auto arena = adapter().arenaObstacles();
    result.insert(result.end(), arena.begin(), arena.end());
    return result;
}

} // namespace gdl::game
