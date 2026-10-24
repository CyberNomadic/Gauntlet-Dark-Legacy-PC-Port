#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Log.h"
#include "engine/math/Math.h"

#include "game/app/ConsoleCommands.h"
#include "game/app/Gauntlet.h"
#include "game/characters/Progression.h"
#include "game/critters/CombatantKind.h"
#include "game/enemies/Enemies.h"
#include "game/menu/MenuInput.h"
#include "game/players/ItemPickup.h"

namespace gdl::game {

void Gauntlet::setUpConsole() {
    if (!m_console.load(renderDevice(), m_options.unpackedDirectory)) {
        log::warn("Developer console: the 8x8 font is missing; its screen will be blank");
    }
    m_console.print(std::format("Gauntlet Dark Legacy {}", m_buildLabel.text()));
    m_console.print("Type help for the commands.");
    m_console.addCommand("debug_info", "shows or hides the level debug overlay",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             m_levelDebug.toggle();
                             console.print(m_levelDebug.visible() ? "Level debug overlay on"
                                                                  : "Level debug overlay off");
                             if (m_levelDebug.visible() && m_play->scene.isOpen()) {
                                 for (const std::string& line : levelDebugLines()) {
                                     log::info("Level debug: {}", line);
                                 }
                             }
                         });
    m_console.addCommand("list", "maps, items or inventory: what there is",
                         [this](DevConsole& console, DevConsole::Arguments arguments) {
                             consoleList(console, arguments);
                         });
    m_console.addCommand("goto", "<map> [x y z] or x y z: brings the party there",
                         [this](DevConsole& console, DevConsole::Arguments arguments) {
                             consoleGoto(console, arguments);
                         });
    m_console.addCommand("unlock_tower", "opens every wing and portal of the tower",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleUnlockTower(console);
                         });
    m_console.addCommand("get", "<item> [amount], exp <points> or level <level>: for you",
                         [this](DevConsole& console, DevConsole::Arguments arguments) {
                             consoleGet(console, arguments);
                         });
    m_console.addCommand("spawn",
                         "stands enemies, great ones, generators, pickups or objects before you; "
                         "spawn alone says how",
                         [this](DevConsole& console, DevConsole::Arguments arguments) {
                             consoleSpawn(console, arguments);
                         });
    m_console.addCommand("suicide", "fells everyone standing",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleSuicide(console);
                         });
    m_console.addCommand("notarget", "the enemies stop seeing you, or see you again",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleToggleCheat(console, &PlayScene::ConsoleCheats::noTarget,
                                                "The enemies no longer see you",
                                                "The enemies see you again");
                         });
    m_console.addCommand("god", "nothing harms you, or everything does again",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleToggleCheat(console, &PlayScene::ConsoleCheats::god,
                                                "God mode on", "God mode off");
                         });
    m_console.addCommand("hidehud", "hides or shows every HUD element",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleHideHud(console);
                         });
    m_console.addCommand("freecam", "lets the camera off the party to fly; you stand still",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleFreeCam(console);
                         });
}

void Gauntlet::consoleToggleCheat(DevConsole& console, bool PlayScene::ConsoleCheats::*flag,
                                  std::string_view on, std::string_view off) {
    const s32 player = consolePlayer();
    if (player < 0) {
        console.print("Nobody is in.");
        return;
    }
    bool& granted = m_consoleCheats[static_cast<usize>(player)].*flag;
    granted = !granted;
    console.print(granted ? on : off);
}

void Gauntlet::consoleHideHud(DevConsole& console) {
    m_hudHidden = !m_hudHidden;
    console.print(m_hudHidden ? "HUD hidden" : "HUD shown");
}

void Gauntlet::consoleFreeCam(DevConsole& console) {
    if (m_freeCamera.active()) {
        m_freeCamera.stop();
        console.print("Free camera off");
        return;
    }
    m_freeCamera.start(m_play->scene.viewCamera());
    console.print(std::format("Free camera on: {}", FreeCamera::kHelp));
}

void Gauntlet::applyConsoleState(f64 deltaSeconds, bool consoleOpen) {
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        m_play->scene.setCheats(player, m_consoleCheats[static_cast<usize>(player)]);
    }
    m_play->scene.setHudHidden(m_hudHidden);
    if (m_freeCamera.active() && !consoleOpen) {
        m_freeCamera.update(input(), static_cast<f32>(deltaSeconds));
    }
    m_play->scene.setViewOverride(m_freeCamera.active() ? std::optional{m_freeCamera.camera()}
                                                        : std::nullopt);
}

/** The player the console speaks for: the one at the keyboard, else the first in. */
s32 Gauntlet::consolePlayer() {
    s32 first = -1;
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (m_play->scene.actor(player) == nullptr) {
            continue;
        }
        if (first < 0) {
            first = player;
        }
        if (playerInputSource(input(), m_config, player).keyboard) {
            return player;
        }
    }
    return first;
}

bool Gauntlet::consoleLevel(DevConsole& console, const std::optional<std::string>& map,
                            ConsoleLevel& level) {
    if (!map.has_value()) {
        level.name = m_play->world.ref().name;
        level.layout = &m_play->world.layout();
        if (const LevelInfo* info = m_play->world.level()) {
            level.roster = info->enemies;
        }
        return true;
    }
    const std::optional<LevelRef> ref =
        *map == "TOWER" ? std::optional{LevelRef::tower()} : m_levels.byName(*map);
    if (!ref.has_value()) {
        console.print(std::format("No map named {}; list maps names them", *map));
        return false;
    }
    if (!level.loaded.load(m_options.unpackedDirectory / ref->directory)) {
        console.print(std::format("{} is not unpacked", ref->name));
        return false;
    }
    level.name = ref->name;
    level.layout = &level.loaded;
    if (level.data.load(m_options.unpackedDirectory / ref->worldDataFile())) {
        if (const LevelInfo* info = level.data.level(ref->name)) {
            level.roster = info->enemies;
        }
    }
    return true;
}

void Gauntlet::consoleList(DevConsole& console, DevConsole::Arguments arguments) {
    using Listing = ConsoleCommands::Listing;
    const auto say = [&](const std::vector<std::string>& lines) {
        for (const std::string& line : lines) {
            console.print(line);
        }
    };
    const Listing listing = ConsoleCommands::listingOf(arguments);
    const std::optional<std::string> map = ConsoleCommands::listedMap(arguments);
    switch (listing) {
    case Listing::Maps: say(ConsoleCommands::mapLines(m_levels)); return;
    case Listing::Inventory:
        if (const CharacterSave* save = m_play->scene.liveSave(consolePlayer())) {
            say(ConsoleCommands::inventoryLines(*save));
        } else {
            console.print("Nobody is in.");
        }
        return;
    case Listing::Unknown: console.print(ConsoleCommands::listUsage()); return;
    case Listing::Items:
        if (!map.has_value()) {
            say(ConsoleCommands::itemLines());
        }
        break;
    case Listing::Enemies:
        if (!map.has_value()) {
            say(ConsoleCommands::enemyKindLines());
        }
        break;
    case Listing::Critters: say(ConsoleCommands::critterLines()); break;
    case Listing::Generators:
    case Listing::Objects: break;
    }
    ConsoleLevel level;
    if (!consoleLevel(console, map, level)) {
        return;
    }
    console.print(std::format("In {}:", level.name));
    switch (listing) {
    case Listing::Items: say(ConsoleCommands::pickupLines(*level.layout)); break;
    case Listing::Enemies:
        say(ConsoleCommands::rosterLines(level.roster));
        say(ConsoleCommands::generatorLines(*level.layout));
        say(ConsoleCommands::placedEnemyLines(*level.layout));
        break;
    case Listing::Critters: say(ConsoleCommands::placedCritterLines(*level.layout)); break;
    case Listing::Generators:
        say(ConsoleCommands::generatorLines(*level.layout));
        console.print("spawn generator <enemy> [tier] stands another");
        break;
    case Listing::Objects: say(ConsoleCommands::objectLines(*level.layout)); break;
    case Listing::Maps:
    case Listing::Inventory:
    case Listing::Unknown: break;
    }
}

void Gauntlet::consoleBorrowVoices(DevConsole& console, const LevelRef& level) {
    WorldData data;
    const LevelInfo* info = data.load(m_options.unpackedDirectory / level.worldDataFile())
                                ? data.level(level.name)
                                : nullptr;
    if (info == nullptr ||
        !m_play->scene.borrowOpponentSounds(*info, data.audio(info->audioIndex))) {
        console.print(std::format("({} lends no voices)", level.name));
    }
}

void Gauntlet::consoleSpawn(DevConsole& console, DevConsole::Arguments arguments) {
    constexpr f32 kAhead = 6.0f; ///< units before the character
    constexpr f32 kApart = 3.0f; ///< between several
    constexpr f32 kProbeAbove = 3.0f;
    constexpr f32 kProbeBelow = 10.0f;
    const PlayerActor* actor = m_play->scene.actor(consolePlayer());
    if (actor == nullptr) {
        console.print("Nobody is in to spawn before.");
        return;
    }
    std::string error;
    const auto spawn = ConsoleCommands::parseSpawn(arguments, error);
    if (!spawn.has_value()) {
        console.print(error);
        return;
    }
    // The map named, when one is: where a visitor's voices, costume or object come from.
    std::optional<LevelRef> from;
    if (spawn->map.has_value()) {
        from = *spawn->map == "TOWER" ? std::optional{LevelRef::tower()}
                                      : m_levels.byName(*spawn->map);
        if (!from.has_value()) {
            console.print(std::format("No map named {}; list maps names them", *spawn->map));
            return;
        }
    }
    const Vec3 facing = actor->facing();
    const f32 yawToPlayer = actor->yaw() + kPi;
    const auto spotAt = [&](s32 index) {
        Vec3 spot = actor->position() + facing * (kAhead + kApart * static_cast<f32>(index));
        if (const auto floor = m_play->world.collision().floorAt(
                spot + Vec3{0.0f, kProbeAbove, 0.0f}, kProbeAbove, kProbeBelow)) {
            spot.y = floor->y;
        }
        return spot;
    };
    PlayScene& scene = m_play->scene;
    switch (spawn->kind) {
    case ConsoleCommands::Spawn::Kind::Enemy: {
        if (from.has_value()) {
            consoleBorrowVoices(console, *from);
        }
        s32 stood = 0;
        const std::vector<EnemyView> views = scene.opponents().views(scene.participants());
        for (s32 i = 0; i < spawn->count; ++i) {
            if (scene.enemies().loadKind(spawn->enemyKind) &&
                scene.enemies()
                    .spawn(EnemySpawn{.kind = spawn->enemyKind,
                                      .tier = spawn->tier,
                                      .position = spotAt(i),
                                      .direction = -facing,
                                      .placed = true},
                           views)
                    .has_value()) {
                ++stood;
            }
        }
        console.print(
            stood > 0 ? std::format("Spawned {} {} at tier {}", stood, spawn->name, spawn->tier)
                      : std::format("No room for {} here, or its kind is missing", spawn->name));
        break;
    }
    case ConsoleCommands::Spawn::Kind::Critter: {
        const char realm =
            from.has_value() && !from->name.empty()
                ? static_cast<char>(std::toupper(static_cast<unsigned char>(from->name.front())))
                : '\0';
        std::optional<s32> id;
        switch (spawn->critter) {
        case CombatantKind::Golem:
            id = scene.critters().spawnGolem(spotAt(0), yawToPlayer, realm);
            break;
        case CombatantKind::General:
            id = scene.critters().spawnGeneral(spotAt(0), yawToPlayer, 0.0f, realm);
            break;
        case CombatantKind::Gargoyle:
            id = scene.critters().spawnGargoyle(spotAt(0), yawToPlayer, spawn->form, realm);
            break;
        case CombatantKind::Unknown:
        case CombatantKind::Boss: break;
        }
        const std::string& what = spawn->form.empty() ? spawn->name : spawn->form;
        console.print(id.has_value()
                          ? std::format("Spawned a {}", what)
                          : std::format("No room for a {} here, or its kind is missing", what));
        break;
    }
    case ConsoleCommands::Spawn::Kind::Generator: {
        if (from.has_value()) {
            consoleBorrowVoices(console, *from);
        }
        const auto id = scene.generators().spawn(renderDevice(), scene.enemies(), spawn->enemyKind,
                                                 spawn->tier, spotAt(0), yawToPlayer);
        console.print(id.has_value()
                          ? std::format("Stood a tier {} {} generator", spawn->tier, spawn->name)
                          : std::format("No {} generator could stand here", spawn->name));
        break;
    }
    case ConsoleCommands::Spawn::Kind::Item:
        if (from.has_value()) {
            console.print("Pickups come from the level in play alone; name one without a map");
        } else if (m_play->world.placeItem(renderDevice(), spawn->name, spotAt(0))) {
            console.print(std::format("Dropped {}", spawn->name));
        } else {
            console.print(
                std::format("No pickup record named {} here; list items names them", spawn->name));
        }
        break;
    case ConsoleCommands::Spawn::Kind::Object: {
        const LevelRef source = from.has_value() ? *from : m_play->world.ref();
        if (scene.spawnObject(source, spawn->name, spotAt(0), yawToPlayer)) {
            console.print(std::format("Stood {} of {}", spawn->name, source.name));
        } else {
            console.print(std::format("Nothing named {} in {}; list objects {} names what there is",
                                      spawn->name, source.name, source.name));
        }
        break;
    }
    case ConsoleCommands::Spawn::Kind::Thing: {
        if (!from.has_value() && m_play->world.placeItem(renderDevice(), spawn->name, spotAt(0))) {
            console.print(std::format("Dropped {}", spawn->name));
            break;
        }
        const LevelRef source = from.has_value() ? *from : m_play->world.ref();
        if (scene.spawnObject(source, spawn->name, spotAt(0), yawToPlayer)) {
            console.print(std::format("Stood {} of {}", spawn->name, source.name));
        } else if (from.has_value()) {
            console.print(std::format("Nothing named {} in {}; list objects {} names what there is",
                                      spawn->name, from->name, from->name));
        } else {
            console.print(std::format("Nothing named {} here; list items, objects, enemies or "
                                      "critters name what there is",
                                      spawn->name));
        }
        break;
    }
    }
}

void Gauntlet::consoleSuicide(DevConsole& console) {
    if (m_play->world.isTower()) {
        console.print("Nobody falls in the tower.");
        return;
    }
    s32 fallen = 0;
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (m_play->scene.kill(player)) {
            ++fallen;
        }
    }
    console.print(fallen > 0 ? std::format("{} fallen", fallen) : "Nobody was standing.");
}

void Gauntlet::consoleGoto(DevConsole& console, DevConsole::Arguments arguments) {
    std::string error;
    const auto destination = ConsoleCommands::parseGoto(arguments, error);
    if (!destination.has_value()) {
        console.print(error);
        return;
    }
    if (m_play->scene.leaving()) {
        console.print("The party is already on its way.");
        return;
    }
    if (!destination->level.has_value()) {
        const Vec3& at = *destination->position;
        if (m_play->scene.relocate(at)) {
            console.print(std::format("Moved to {:.1f} {:.1f} {:.1f}", at.x, at.y, at.z));
        } else {
            console.print("Nobody is standing to move.");
        }
        return;
    }
    const std::optional<LevelRef> level = *destination->level == "TOWER"
                                              ? std::optional{LevelRef::tower()}
                                              : m_levels.byName(*destination->level);
    if (!level.has_value()) {
        console.print(std::format("No map named {}; list maps names them", *destination->level));
        return;
    }
    console.print(std::format("Going to {} ({})", level->name, level->title));
    consoleTravel(*level, destination->position, std::nullopt);
}

/** Takes the party to `level` through the loading picture, as an exit portal would, standing
 * it at `position` facing `yaw` when given. The scene closes here: the tower update that
 * runs the command must go no further this frame. */
void Gauntlet::consoleTravel(const LevelRef& level, std::optional<Vec3> position,
                             std::optional<f32> yaw) {
    Journey journey;
    journey.destination = level;
    journey.party = m_play->scene.party();
    journey.options.welcome = false;
    journey.options.position = position;
    journey.options.yaw = yaw;
    m_freeCamera.stop(); // its place means nothing in another level
    m_play->scene.close();
    m_loadingPicture.load(renderDevice(), m_options.unpackedDirectory);
    m_loadingPicture.cover();
    m_journey = std::move(journey);
}

void Gauntlet::consoleUnlockTower(DevConsole& console) {
    s32 opened = 0;
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (CharacterSave* save = m_play->scene.liveSave(player)) {
            ConsoleCommands::unlockTower(*save);
            ++opened;
        }
    }
    if (opened == 0) {
        console.print("Nobody is in to open it for.");
        return;
    }
    console.print(
        std::format("The tower stands open to {} character{}.", opened, opened == 1 ? "" : "s"));
    if (m_play->world.isTower()) {
        // The portals read the party's progress as the tower opens: open it again, where
        // the party stands.
        const PlayerActor* actor = m_play->scene.actor(consolePlayer());
        consoleTravel(LevelRef::tower(),
                      actor != nullptr ? std::optional{actor->position()} : std::nullopt,
                      actor != nullptr ? std::optional{actor->yaw()} : std::nullopt);
    }
}

void Gauntlet::consoleGet(DevConsole& console, DevConsole::Arguments arguments) {
    const s32 player = consolePlayer();
    CharacterSave* save = m_play->scene.liveSave(player);
    if (save == nullptr) {
        console.print("Nobody is in to give it to.");
        return;
    }
    std::string error;
    const auto gift = ConsoleCommands::parseGet(arguments, error);
    if (!gift.has_value()) {
        console.print(error);
        return;
    }
    switch (gift->kind) {
    case ConsoleCommands::Gift::Kind::Experience:
        m_play->scene.awardExperience(player, gift->amount, false);
        m_play->scene.restat(player);
        console.print(std::format("{} experience: level {}", gift->amount,
                                  experienceLevel(save->experience())));
        break;
    case ConsoleCommands::Gift::Kind::Level:
        save->progress().experience = levelExperience(gift->amount);
        m_play->scene.restat(player);
        console.print(std::format("Level {}", gift->amount));
        break;
    case ConsoleCommands::Gift::Kind::Item:
        // At one times the strength: the seconds asked for are the seconds given.
        console.print(ConsoleCommands::report(*gift, takeItem(*save, gift->offer, 1.0f, true)));
        break;
    }
}

} // namespace gdl::game
