#include <algorithm>
#include <exception>
#include <format>

#include "engine/audio/DreamcastStream.h"
#include "engine/audio/N64LegendsStream.h"
#include "engine/core/Strings.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "formats/LegendsDreamcastTexture.h"
#include "formats/LegendsMap.h"
#include "formats/LegendsN64Texture.h"
#include "game/app/Gauntlet.h"
#include "game/characters/LegendsVisit.h"
#include "game/console/ConsoleCommands.h"
#include "game/scenes/LegendsLevel.h"
#include "game/scenes/LegendsMapCatalog.h"
#include "game/scenes/LegendsMusic.h"
#include "game/scenes/LegendsN64Music.h"

namespace gdl::game {
namespace {
std::unique_ptr<SoundPlayer> openLegendsMusic(const std::filesystem::path& levelDirectory,
                                              LegendsPlatform platform, AudioMixer& mixer,
                                              const GameConfig& config, DevConsole& console) {
    try {
        std::shared_ptr<StreamSource> soundtrack;
        std::string soundtrackName;
        if (platform == LegendsPlatform::N64) {
            auto directory = levelDirectory.lexically_normal();
            if (directory.filename().empty()) {
                directory = directory.parent_path();
            }
            if (const auto song = legendsN64SongForLevel(paths::utf8(directory.filename()))) {
                auto stream = std::make_shared<N64LegendsStream>();
                if (stream->open(directory.parent_path().parent_path() / "audio", *song)) {
                    soundtrack = std::move(stream);
                    soundtrackName = std::format("N64 song {}", *song);
                } else {
                    console.print(
                        "Native N64 music requires the complete, unchanged US audio bank.");
                }
            } else {
                console.print("No native soundtrack binding was found for this N64 directory.");
            }
        } else if (const auto music = dreamcastLegendsMusic(levelDirectory)) {
            auto stream = std::make_shared<DreamcastStream>();
            if (stream->open(*music)) {
                soundtrack = std::move(stream);
                soundtrackName = paths::utf8(music->filename());
            } else {
                console.print("The level's native music stream could not be opened.");
            }
        } else {
            console.print("No verified native soundtrack binding was found for this directory.");
        }
        if (soundtrack) {
            auto sounds = std::make_unique<SoundPlayer>(mixer);
            sounds->setMasterVolume(config.audio.masterVolume);
            sounds->setCategoryVolume(SoundCategory::Music, config.audio.musicVolume);
            sounds->playStream(std::move(soundtrack), true);
            console.print(std::format("Native soundtrack: {}", soundtrackName));
            return sounds;
        }
    } catch (const std::exception& error) {
        console.print(std::format("Music could not start: {}", error.what()));
    }
    return {};
}
} // namespace

std::vector<LegendsMapEntry> Gauntlet::legendsMapsOf(LegendsPlatform platform,
                                                     bool rescan) const {
    auto& found = m_legendsMaps[platform == LegendsPlatform::N64 ? 0 : 1];
    if (rescan || !found.has_value()) {
        found = legendsMaps(platform, m_options.unpackedDirectory, paths::executableDirectory(),
                            std::filesystem::current_path());
    }
    return *found;
}

std::optional<std::pair<LegendsPlatform, std::filesystem::path>>
Gauntlet::findLegendsMap(std::string_view name, std::optional<LegendsPlatform> platform) const {
    for (const LegendsPlatform candidate : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
        if (platform.has_value() && *platform != candidate) {
            continue;
        }
        if (const auto directory = legendsMapPath(legendsMapsOf(candidate, true), name)) {
            return std::pair{candidate, *directory};
        }
    }
    return std::nullopt;
}

void Gauntlet::consoleLegendsClose(DevConsole& console) {
    if (m_play->scene.leaving()) {
        console.print("Wait for the current level transition to finish.");
        return;
    }
    if (!m_legendsPreview.isOpen() && m_legendsOrigin) {
        if (closeLegendsLevel()) {
            console.print("Returned from the Legends level.");
        }
        return;
    }
    if (!m_legendsPreview.isOpen()) {
        console.print("Not in a Legends map.");
        return;
    }
    // Direct render backends do not retain texture leases for queued frames.
    renderDevice().waitIdle();
    commitLegendsProgress();
    m_legendsPreview.close();
    if (m_legendsSounds) {
        m_legendsSounds->stopAll();
        m_legendsSounds.reset();
    }
    m_play->scene.releaseSounds();
    if (const auto* native = dynamic_cast<const LegendsLevel*>(m_play->world.imported())) {
        m_legendsSounds = openLegendsMusic(native->directory(), native->platform(),
                                           m_audio->mixer(), m_config, console);
    }
    window().holdCursor(false);
    resetPlayInput();
    console.print("Returned from the Legends map preview.");
}

void Gauntlet::consoleLegendsOpen(DevConsole& console, LegendsPlatform platform,
                                  const std::filesystem::path& directory, bool playable,
                                  bool completedTravel) {
    const auto* imported = m_play->world.imported();
    const auto nativeDeparture = imported ? imported->departure() : std::nullopt;
    const bool internalTravel =
        completedTravel && imported && imported->travelRequested() && m_play->scene.leaving() &&
        m_play->scene.transition().covering() &&
        (nativeDeparture ? nativeDeparture->finished : m_play->scene.departure().finished());
    if (completedTravel && !internalTravel) {
        console.print("The native departure has not finished.");
        return;
    }
    const bool n64 = platform == LegendsPlatform::N64;
    try {
        // Only finishLegendsTravel supplies the private completed-travel flag.
        // A player console command cannot bypass the pending exit animation.
        if (!m_play->scene.isOpen() || (m_play->scene.leaving() && !internalTravel) ||
            m_journey.has_value()) {
            console.print("Enter an offline level before opening a Legends map.");
            return;
        }
        const AssetLocator files{directory};
        const auto world = files.find(n64 ? "worlds.wad" : "worlds.rom");
        const auto models = files.find(n64 ? "objects.wad" : "objects.rom");
        if (!world || !models) {
            console.print("The directory must contain the native worlds and objects files.");
            return;
        }
        const auto worldBytes = readFile(*world);
        const auto modelBytes = readFile(*models);
        const auto placements = files.find("ipr.rom");
        const auto placementBytes = n64 && placements ? readFile(*placements) : std::vector<u8>{};
        auto map = n64 ? formats::LegendsMap::parseN64(worldBytes, modelBytes, placementBytes)
                       : formats::LegendsMap::parseDreamcast(worldBytes, modelBytes);
        if (n64) {
            const auto dynamic = files.find("texdyn.rom");
            const auto dynamicBytes = dynamic ? readFile(*dynamic) : std::vector<u8>{};
            for (auto& texture : formats::decodeLegendsN64Textures(modelBytes, dynamicBytes)) {
                map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                        texture.hasTransparency, std::move(texture.name),
                                        texture.external, texture.width, texture.height});
            }
        }
        if (!n64) {
            if (const auto pixels = files.find("textures.rom")) {
                map.textures =
                    formats::decodeLegendsDreamcastTextures(modelBytes, readFile(*pixels));
            } else {
                map.warnings.push_back("textures.rom is missing; showing untextured geometry.");
            }
        }
        auto adapter = LegendsSceneAdapter::load(
            renderDevice(), platform, files.root(), map,
            playable ? static_cast<s32>(m_play->scene.party().size()) : 1);
        console.print(std::format("Native presentation: {} fixtures, {} world texture animations.",
                                  adapter->fixtureCount(), adapter->textureAnimationCount()));
        console.print(std::format("{} placed pickups have verified gameplay mappings.",
                                  adapter->pickups().size()));
        for (const auto& warning : adapter->warnings()) {
            console.print(warning);
        }
        if (m_legendsPreview.isOpen()) {
            renderDevice().waitIdle();
            if (!commitLegendsProgress()) {
                return;
            }
        }
        if (playable) {
            auto party = m_play->scene.party();
            if (internalTravel) {
                if (const auto* previous = dynamic_cast<const LegendsLevel*>(imported)) {
                    previous->completeTravel(party);
                }
            }
            const auto selected = std::ranges::find_if(
                party, [](const PartyMember& member) { return !member.fallen; });
            if (selected == party.end()) {
                console.print("Select a character before exploring a Legends map.");
                return;
            }
            ClassDataSet classes;
            classes.load(m_options.unpackedDirectory / "pdata");
            LegendsTraversal entrance;
            entrance.open(map, selected->player, selected->save,
                          classes.stats(selected->save.character));
            Vec3 low = entrance.start(), high = low;
            for (const auto& face : map.collision) {
                for (const auto& vertex : face.vertices) {
                    low = glm::min(low, vertex);
                    high = glm::max(high, vertex);
                }
            }
            WorldLocator start;
            start.kind = LocatorKind::Start;
            start.position = entrance.start();
            start.rotation.y = entrance.actor().yaw() - kPi;
            auto native = std::make_shared<LegendsLevel>(renderDevice(), map, std::move(adapter),
                                                         files.root(), m_options.unpackedDirectory);
            native->initializeParty(party);
            auto next = std::make_unique<PlaySession>();
            LevelRef ref;
            ref.realmId = -1; // native realm IDs must never index the GDL campaign
            ref.realm = std::string(ConsoleCommands::platformName(platform));
            ref.name = paths::utf8(native->directory().filename());
            ref.title = ref.name;
            ref.directory = paths::utf8(files.root());
            if (!next->world.loadImported(renderDevice(), m_options.unpackedDirectory, ref,
                                          WorldLayout::fromDecoded({}, {start}, low, high),
                                          entrance.collision(), native)) {
                console.print("The native map could not be bound to gameplay.");
                return;
            }
            auto nextContext = context();
            nextContext.tower = &next->world;
            PlayOptions options;
            options.position = entrance.start();
            options.yaw = entrance.actor().yaw();
            options.welcome = false;
            options.partyCount = m_partyCountOverride;
            m_play->scene.suspendForImportedLevel();
            if (!next->scene.open(renderDevice(), nextContext, next->world, party, options)) {
                m_play->scene.resumeAfterImportedLevel();
                console.print("The native map's shared gameplay resources could not be loaded.");
                return;
            }
            renderDevice().waitIdle();
            m_legendsPreview.close();
            if (!m_legendsOrigin) {
                m_legendsEntryParty = party;
                m_legendsOrigin = std::move(m_play);
            }
            m_play = std::move(next);
            m_play->scene.releaseSounds();
            m_sounds->setCategoryScale(SoundCategory::Music, m_musicDuck.level());
            m_sounds->setCategoryScale(SoundCategory::Effects, 1);
            setMaxFrameRate(m_config.timing.gameplayFrameRate);
        } else {
            m_legendsPreview.open(renderDevice(), map, std::move(adapter));
            m_play->scene.holdSounds();
        }
        if (m_legendsSounds) {
            m_legendsSounds->stopAll();
            m_legendsSounds.reset();
        }
        m_legendsSounds =
            openLegendsMusic(files.root(), platform, m_audio->mixer(), m_config, console);
        window().stopRumble();
        resetPlayInput();
        console.print(std::format("Legends native map: {} objects.", map.objects.size()));
        if (playable) {
            console.print("Normal party controls, combat, inventory and pause are active.");
            console.print(
                "Escape opens the pause menu. Save normally after returning to keep rewards.");
        } else {
            console.print("Native scene preview. goto <map> without preview walks its collision.");
            console.print(std::string{FreeCamera::kHelp});
        }
        console.print("goto back returns to the level left for it.");
        for (const auto& warning : map.warnings) {
            console.print(warning);
        }
    } catch (const std::exception& error) {
        if (m_play->scene.isOpen() && !m_legendsPreview.isOpen()) {
            m_play->scene.resumeAfterImportedLevel();
        }
        console.print(std::format("Legends map was not opened: {}", error.what()));
    }
}

void Gauntlet::updateLegendsPreview(f64 deltaSeconds) {
    const bool wasOpen = m_console.open();
    m_console.update(deltaSeconds, input(), m_config.menu.console);
    const bool captures = m_console.open() || wasOpen;
    if (m_legendsPreview.isOpen() && !captures && input().wasKeyPressed(Key::Escape)) {
        renderDevice().waitIdle();
        commitLegendsProgress();
        m_legendsPreview.close();
        if (m_legendsSounds) {
            m_legendsSounds->stopAll();
            m_legendsSounds.reset();
        }
        m_play->scene.releaseSounds();
        if (const auto* native = dynamic_cast<const LegendsLevel*>(m_play->world.imported())) {
            m_legendsSounds = openLegendsMusic(native->directory(), native->platform(),
                                               m_audio->mixer(), m_config, m_console);
        }
        resetPlayInput();
    }
    if (m_legendsPreview.playable()) {
        m_legendsPreview.updatePlayable(input(), static_cast<f32>(deltaSeconds), captures,
                                        m_config);
    } else {
        m_legendsPreview.update(input(), static_cast<f32>(deltaSeconds), captures);
    }
    window().holdCursor(m_legendsPreview.isOpen() && !m_legendsPreview.playable() && !captures &&
                        input().isKeyDown(Key::MouseRight));
    m_sounds->setCategoryScale(SoundCategory::Music,
                               m_legendsPreview.isOpen() ? 0.0f : m_musicDuck.level());
    m_sounds->setCategoryScale(SoundCategory::Effects, m_legendsPreview.isOpen() ? 0.0f : 1.0f);
    m_sounds->update();
    if (m_legendsSounds) {
        m_legendsSounds->update();
    }
}

bool Gauntlet::commitLegendsProgress() {
    if (m_legendsOrigin) {
        const auto visiting = m_play->scene.party();
        std::vector<std::pair<s32, CharacterSave>> changes;
        for (const auto& before : m_legendsEntryParty) {
            if (before.fallen) {
                continue; // that seat waited outside the visit and earned no rewards
            }
            const auto after = std::ranges::find_if(visiting, [&](const PartyMember& member) {
                return member.player == before.player;
            });
            auto* live = m_legendsOrigin->scene.liveSave(before.player);
            if (!live) {
                m_console.print("The native visit could not be returned to its original party.");
                return false;
            }
            // A fallen player can leave the local session. That does not prevent
            // the remaining party returning; no rewards are copied for that seat.
            if (after == visiting.end()) {
                continue;
            }
            auto candidate = *live;
            if (!commitLegendsVisit(candidate, before.save, after->save)) {
                m_console.print("The original character changed; its newer state was preserved.");
                return false;
            }
            changes.emplace_back(before.player, std::move(candidate));
        }
        for (auto& [player, save] : changes) {
            *m_legendsOrigin->scene.liveSave(player) = std::move(save);
            m_legendsOrigin->scene.restat(player);
        }
        m_legendsEntryParty = m_legendsOrigin->scene.party();
        return true;
    }
    const auto* traversal = m_legendsPreview.traversal();
    if (!traversal) {
        return true;
    }
    const s32 player = traversal->actor().player();
    auto* live = m_play->scene.liveSave(player);
    if (!live || !m_legendsPreview.transferProgress(*live)) {
        m_console.print("Legends rewards were not applied: the parked character changed during "
                        "this visit. Its newer state was preserved.");
        return false;
    }
    m_play->scene.restat(player);
    return true;
}

bool Gauntlet::closeLegendsLevel(bool keepProgress) {
    if (!m_legendsOrigin || (keepProgress && !commitLegendsProgress())) {
        return false;
    }
    renderDevice().waitIdle();
    m_play = std::move(m_legendsOrigin);
    m_legendsEntryParty.clear();
    if (m_legendsSounds) {
        m_legendsSounds->stopAll();
        m_legendsSounds.reset();
    }
    m_play->scene.resumeAfterImportedLevel();
    m_sounds->setCategoryScale(SoundCategory::Music, m_musicDuck.level());
    m_sounds->setCategoryScale(SoundCategory::Effects, 1);
    resetPlayInput();
    return true;
}

void Gauntlet::finishLegendsTravel() {
    const auto* native = dynamic_cast<const LegendsLevel*>(m_play->world.imported());
    if (!native || !native->destination()) {
        closeLegendsLevel();
        return;
    }
    // Resolve beside the currently loaded extraction, never through an unrelated
    // installed copy. consoleLegendsOpen builds the replacement before swapping.
    const std::filesystem::path path = native->directory().parent_path() / *native->destination();
    const auto generation = m_play->scene.loadGeneration();
    const auto* previous = m_play.get();
    consoleLegendsOpen(m_console, native->platform(), path, true, true);
    if (m_play.get() == previous && m_play->scene.loadGeneration() == generation) {
        m_console.print("Native destination unavailable; returning to the original level.");
        closeLegendsLevel();
    }
}

} // namespace gdl::game
