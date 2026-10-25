#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/assets/ModelSet.h"
#include "engine/assets/ObjectMaterial.h"
#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Log.h"
#include "engine/math/Math.h"

#include "game/app/ConsoleCommands.h"
#include "game/app/Gauntlet.h"
#include "game/characters/ClassData.h"
#include "game/characters/Progression.h"
#include "game/critters/CombatantKind.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/Generators.h"
#include "game/level/PlacedItems.h"
#include "game/level/TowerAccess.h"
#include "game/menu/MenuInput.h"
#include "game/players/ItemPickup.h"
#include "game/players/NoclipMotion.h"
#include "game/scenes/SceneSelection.h"

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
    m_console.addCommand("noclip", "flies you through everything, or sets you down again",
                         [this](DevConsole& console, DevConsole::Arguments /*arguments*/) {
                             consoleNoclip(console);
                         });
}

void Gauntlet::consoleNoclip(DevConsole& console) {
    const s32 player = consolePlayer();
    if (player < 0) {
        console.print("Nobody is in.");
        return;
    }
    bool& flying = m_consoleCheats[static_cast<usize>(player)].noclip;
    flying = !flying;
    m_noclip[static_cast<usize>(player)].reset();
    console.print(flying ? std::format("Noclip on: {}", NoclipMotion::kHelp) : "Noclip off");
}

bool Gauntlet::consoleCovers(Vec2 cursor) {
    // The 2D layer is letterboxed into the window: as wide or as tall as the window
    // allows at the frame's shape, centred. The console spans that frame's width.
    const Extent2D window = renderDevice().framebufferExtent();
    if (window.isZero()) {
        return false;
    }
    const auto frameWidth = static_cast<f32>(m_config.display.frameWidth);
    const auto frameHeight = static_cast<f32>(m_config.display.frameHeight);
    const auto windowWidth = static_cast<f32>(window.width);
    const auto windowHeight = static_cast<f32>(window.height);
    const f32 scale = std::min(windowWidth / frameWidth, windowHeight / frameHeight);
    const f32 width = frameWidth * scale / windowWidth;
    const f32 height = frameHeight * scale / windowHeight;
    const f32 left = (1.0f - width) * 0.5f;
    const f32 top = (1.0f - height) * 0.5f;
    const f32 shown = height * m_console.slide() * static_cast<f32>(DevConsole::kHeight) /
                      static_cast<f32>(m_config.display.virtualHeight);
    return cursor.x >= left && cursor.x <= left + width && cursor.y >= top &&
           cursor.y <= top + shown;
}

void Gauntlet::consoleSelect(std::optional<Vec2> cursor) {
    std::optional<SceneSelection> picked;
    if (cursor.has_value() && m_play->scene.isOpen()) {
        picked = m_play->scene.pick(*cursor);
    }
    if (m_play->scene.isOpen()) {
        m_play->scene.select(picked);
    }
    m_console.setVariable("SELECTED",
                          picked.has_value() ? std::optional{picked->token()} : std::nullopt);
    if (cursor.has_value()) {
        m_console.print(picked.has_value() ? std::format("Selected {}", picked->label())
                                           : "Nothing there to select");
    }
}

std::vector<std::string> Gauntlet::consoleDescribe(const SceneSelection& selection) {
    using Kind = SceneSelection::Kind;
    std::vector<std::string> lines;
    PlayScene& scene = m_play->scene;
    if (!scene.isOpen()) {
        lines.emplace_back("Nothing is in play.");
        return lines;
    }
    const auto place = [](const Vec3& at) {
        return std::format("at {:.1f} {:.1f} {:.1f}", at.x, at.y, at.z);
    };
    const auto gone = [&] {
        lines.push_back(std::format("{} is no longer there", selection.token()));
    };
    const s32 id = selection.id;
    switch (selection.kind) {
    case Kind::Object: {
        const LevelWorld& world = m_play->world;
        const auto& objects = world.layout().objects();
        if (id < 0 || static_cast<usize>(id) >= objects.size()) {
            lines.push_back(
                std::format("{} names no object of {}", selection.token(), world.ref().name));
            break;
        }
        const auto index = static_cast<usize>(id);
        const WorldObject& object = objects[index];
        const WorldScene& view = world.scene();
        lines.push_back(std::format("{} {}: object {} of {}", selection.token(), object.name, id,
                                    world.ref().name));
        const std::string parent =
            object.parent >= 0 && static_cast<usize>(object.parent) < objects.size()
                ? std::format("{} (object {})", objects[static_cast<usize>(object.parent)].name,
                              object.parent)
                : std::string("none");
        lines.push_back(std::format("{}; radius {:.1f}; parent {}",
                                    place(Vec3{view.worldTransform(index)[3]}), object.radius,
                                    parent));
        std::string flags;
        for (const auto& [bit, word] :
             {std::pair{WorldObject::kPrelit, "prelit"}, std::pair{WorldObject::kFloor, "floor"},
              std::pair{WorldObject::kParticles, "particles"},
              std::pair{WorldObject::kAnimated, "animated"},
              std::pair{WorldObject::kReverse, "reverse"}, std::pair{WorldObject::kOnce, "once"}}) {
            if ((object.flags & bit) != 0) {
                flags += std::format(" {}", word);
            }
        }
        const ObjectMaterial material = ObjectMaterial::fromFlags(object.objectFlags);
        std::string look;
        if (material.chrome) {
            look += " chrome";
        }
        if (material.additive) {
            look += " additive";
        }
        if (material.sorted) {
            look += " sorted";
        }
        if (!material.depthWrite) {
            look += " no-depth-write";
        }
        if (!material.depthTest) {
            look += " no-depth-test";
        }
        if (material.facing != 0) {
            look += std::format(" facing-{}", material.facing);
        }
        lines.push_back(std::format("flags {:#x}{}; object flags {:#x}{}", object.flags, flags,
                                    object.objectFlags, look));
        lines.push_back(std::format("{}; {}",
                                    view.moving(index) ? "animated: placed every frame" : "static",
                                    view.objectVisible(index) ? "shown" : "hidden"));
        ModelSet& models = m_play->world.models();
        const TextureSet& textures = m_play->world.textures();
        const auto model = models.find(object.name);
        if (!model.has_value()) {
            lines.emplace_back("no mesh");
            break;
        }
        const ModelSetEntry& entry = models.entry(*model);
        try {
            const Mesh& mesh = models.mesh(*model);
            std::string names;
            for (const MeshPart& part : mesh.parts) {
                const std::string name = part.texture < textures.size()
                                             ? textures.entry(part.texture).name
                                             : std::format("texture {}", part.texture);
                if (names.find(name) == std::string::npos) {
                    names += (names.empty() ? "" : ", ") + name;
                }
            }
            lines.push_back(std::format("mesh: {} triangles, {} part{}: {}", entry.triangles,
                                        mesh.parts.size(), mesh.parts.size() == 1 ? "" : "s",
                                        names));
        } catch (const std::exception&) {
            lines.push_back(std::format("mesh: {} triangles, not decoded", entry.triangles));
        }
        break;
    }
    case Kind::Enemy: {
        const Enemies& swarm = scene.enemies();
        if (id < 0 || id >= Enemies::kMost || !swarm.alive(id)) {
            gone();
            break;
        }
        const EnemyKind& kind = enemyKind(swarm.kindOf(id));
        lines.push_back(std::format("{} {}: tier {}, health {:.0f}{}", selection.token(), kind.name,
                                    swarm.tierOf(id), swarm.healthOf(id),
                                    swarm.dying(id) ? ", dying" : ""));
        lines.push_back(std::format("{}; radius {:.1f}, height {:.1f}", place(swarm.positionOf(id)),
                                    swarm.radiusOf(id), swarm.heightOf(id)));
        break;
    }
    case Kind::Critter: {
        const Critters& critters = scene.critters();
        if (id < 0 || static_cast<usize>(id) >= critters.count() || !critters.alive(id)) {
            gone();
            break;
        }
        const std::string form = critters.formOf(id);
        lines.push_back(std::format(
            "{} {}{}: health {:.0f}", selection.token(), combatantKindName(critters.kindOf(id)),
            form.empty() ? "" : std::format(" {}", form), critters.healthOf(id)));
        lines.push_back(std::format("{}; radius {:.1f}", place(critters.positionOf(id)),
                                    critters.radiusOf(id)));
        break;
    }
    case Kind::Generator: {
        const Generators& generators = scene.generators();
        if (id < 0 || static_cast<usize>(id) >= generators.count()) {
            gone();
            break;
        }
        const s32 kind = generators.kindOf(id);
        lines.push_back(std::format("{} {}: tier {}, state {} of {}, health {:.0f}",
                                    selection.token(), kind >= 0 ? enemyKind(kind).name : "brood",
                                    generators.tierOf(id), generators.stateOf(id),
                                    Generators::kStates, generators.healthOf(id)));
        lines.push_back(place(generators.positionOf(id)));
        break;
    }
    case Kind::Pickup: {
        const PlacedItems& items = m_play->world.placedItems();
        if (id < 0 || static_cast<usize>(id) >= items.size()) {
            gone();
            break;
        }
        const PlacedItems::Item& item = items.item(static_cast<usize>(id));
        lines.push_back(std::format("{} {}: {}, worth {}{}{}", selection.token(), item.name,
                                    ConsoleCommands::pickupKindName(item.subtype), item.value,
                                    item.taken ? ", taken" : "", item.visible ? "" : ", unseen"));
        lines.push_back(std::format("{}; radius {:.1f}, height {:.1f}; instance {} of record {}",
                                    place(item.position), item.radius, item.height, item.instance,
                                    item.info));
        break;
    }
    case Kind::Player: {
        const PlayerActor* actor = scene.actor(id);
        if (actor == nullptr) {
            gone();
            break;
        }
        const CharacterSave& save = actor->save();
        lines.push_back(std::format("{} {}: {} of level {}, health {}", selection.token(),
                                    save.name, classCode(save.character % kStartingClassCount),
                                    experienceLevel(save.experience()), save.health()));
        lines.push_back(std::format("{}; facing {:.0f} degrees; radius {:.1f}, height {:.1f}",
                                    place(actor->position()), glm::degrees(actor->yaw()),
                                    actor->radius(), actor->height()));
        break;
    }
    case Kind::Spawned: {
        const SpawnedObjects& spawned = scene.spawnedObjects();
        if (id < 0 || static_cast<usize>(id) >= spawned.count()) {
            gone();
            break;
        }
        const SpawnedObjects::View view = spawned.view(static_cast<usize>(id));
        lines.push_back(std::format("{} {}: stood by the console", selection.token(), view.name));
        lines.push_back(std::format("{}; radius {:.1f}", place(view.position), view.radius));
        break;
    }
    }
    return lines;
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
    // While the free camera is turned by the right button, the pointer is hidden where
    // it is and shows there again when the button is let go.
    window().holdCursor(m_freeCamera.active() && !consoleOpen &&
                        input().isKeyDown(Key::MouseRight));
    // Noclip flies a character as the camera looks, the console's keys left alone.
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        NoclipMotion& flight = m_noclip[static_cast<usize>(player)];
        if (!m_consoleCheats[static_cast<usize>(player)].noclip) {
            continue;
        }
        if (consoleOpen) {
            flight.reset();
            continue;
        }
        const MenuInputSource source = playerInputSource(input(), m_config, player);
        const NoclipMotion::Wish wish =
            NoclipMotion::wish(input(), playBindings(m_config, player), source.keyboard, source.pad,
                               m_play->scene.viewCamera());
        const NoclipMotion::Step step = flight.advance(wish, static_cast<f32>(deltaSeconds));
        m_play->scene.noclipMove(player, step.offset, step.yaw);
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
        s32 kind = spawn->enemyKind;
        ItemArchive* realmItems = nullptr;
        const ItemInfo* record = nullptr;
        ConsoleLevel source;
        if (spawn->brood) {
            // A mixed brood is its realm's: its bodies and record come from that realm's
            // level, the one in play when it is one of them.
            const s32 realm = from.has_value() ? from->realmId : m_play->world.ref().realmId;
            if (realm != TowerAccess::kTempleWorld && realm != TowerAccess::kUnderworldWorld) {
                console.print("A mixed brood is the temple's (E) or the underworld's (F); name "
                              "a map of either");
                break;
            }
            kind = realm == TowerAccess::kTempleWorld ? Generators::kTempleBrood
                                                      : Generators::kHellBrood;
            if (from.has_value()) {
                realmItems = scene.borrowItems(*from);
                if (realmItems == nullptr) {
                    console.print(std::format("{} lends no item archive", from->name));
                    break;
                }
                if (consoleLevel(console, spawn->map, source)) {
                    record = ConsoleCommands::generatorRecord(*source.layout, "SPECIAL");
                }
            }
        }
        const auto id = scene.generators().spawn(renderDevice(), scene.enemies(), kind, spawn->tier,
                                                 spotAt(0), yawToPlayer, realmItems, record);
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
    std::string error;
    const auto gift = ConsoleCommands::parseGet(arguments, error);
    if (!gift.has_value()) {
        console.print(error);
        return;
    }
    if (gift->kind == ConsoleCommands::Gift::Kind::Info) {
        const std::optional<SceneSelection> what =
            gift->name.empty() ? m_play->scene.selection() : SceneSelection::parse(gift->name);
        if (!what.has_value()) {
            console.print(gift->name.empty()
                              ? "Nothing is selected: click something in the view while the "
                                "console is down, then get info $SELECTED"
                              : std::format("{} names nothing; $SELECTED or a token such as "
                                            "object#12 or enemy#3 does",
                                            gift->name));
            return;
        }
        for (const std::string& line : consoleDescribe(*what)) {
            console.print(line);
        }
        return;
    }
    const s32 player = consolePlayer();
    CharacterSave* save = m_play->scene.liveSave(player);
    if (save == nullptr) {
        console.print("Nobody is in to give it to.");
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
