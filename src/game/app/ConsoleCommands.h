#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/characters/CharacterSave.h"
#include "game/characters/Inventory.h"
#include "game/critters/CombatantKind.h"
#include "game/level/LevelCatalog.h"
#include "game/players/ItemPickup.h"

namespace gdl::game {

/**
 * The developer console's commands as far as they need no scene: reading their arguments,
 * listing the maps, the items, a character's inventory and what a level's layout holds, and
 * opening the tower in a character's record. The application wires them to the party and the
 * level in play. Each command's usage is a few lines, its forms in a column with what each
 * does beside them.
 */
class ConsoleCommands {
public:
    using Arguments = std::span<const std::string>;

    /** What `list` is asked for; a map's name may follow it. */
    enum class Listing : u8 {
        Maps,
        Items,
        Inventory,
        Enemies,
        Critters,
        Generators,
        Objects,
        Unknown
    };
    static Listing listingOf(Arguments arguments);
    /** The map named after the listing ("G1"), when one is. */
    static std::optional<std::string> listedMap(Arguments arguments);
    static std::string listUsage();
    /** Whether a word names a level the way the catalogue does ("E2", "TOWER"). */
    static bool looksLikeMap(std::string_view word);

    /** Where `goto` sends the party: a level by name, a point, or a point in a level. */
    struct Destination {
        std::optional<std::string> level; ///< as the catalogue names it, "G1"; "TOWER" the tower
        std::optional<Vec3> position;
        bool operator==(const Destination&) const = default;
    };
    static std::string gotoUsage();
    /** Reads `goto`'s arguments; nothing, with `error` set, when they make no sense. */
    static std::optional<Destination> parseGoto(Arguments arguments, std::string& error);

    /** What `get` hands over, or tells. */
    struct Gift {
        enum class Kind : u8 { Item, Experience, Level, Info };
        Kind kind = Kind::Item;
        ItemOffer offer;  ///< the item, as if it lay in the level
        s32 amount = 0;   ///< the experience to add or the level to reach
        std::string name; ///< the item's, or the thing `info` is asked of (empty: what is
                          ///< selected)
    };
    static std::string getUsage();
    static constexpr s32 kDefaultKeys = 1;
    static constexpr s32 kDefaultPotions = 1;
    static constexpr s32 kDefaultPotionKind = 1; ///< fire
    static constexpr s32 kDefaultGold = 100;
    static constexpr s32 kDefaultHealth = 100;
    static constexpr f32 kDefaultSeconds = 60.0f; ///< a timed powerup's, unless given
    /** Reads `get`'s arguments; nothing, with `error` set, for an unknown item or amount. */
    static std::optional<Gift> parseGet(Arguments arguments, std::string& error);
    /** What `get` says of what came of `gift`. */
    static std::string report(const Gift& gift, const ItemTaking& taking);

    /** What `spawn` is asked to put before the character. */
    struct Spawn {
        enum class Kind : u8 {
            Enemy,     ///< `count` of the swarm's `enemyKind` at `tier`
            Critter,   ///< one `critter`, a gargoyle in `form`
            Generator, ///< one breeding `enemyKind` at `tier`
            Item,      ///< a pickup of the level in play, by its record's `name`
            Object,    ///< a copy of a level's object, by `name`
            Thing      ///< `name` as a pickup, failing that an object
        };
        Kind kind = Kind::Thing;
        s32 enemyKind = -1;
        bool brood = false; ///< a generator of the temple's or the underworld's mixed kinds
        s32 tier = 1;
        s32 count = 1;
        CombatantKind critter = CombatantKind::Unknown;
        std::string form;               ///< a gargoyle's ("GAR_EAGL")
        std::string name;               ///< a thing's, as the level's records spell it
        std::optional<std::string> map; ///< the level it comes from, else the one in play
    };
    static std::string spawnUsage();
    static constexpr s32 kMostSpawned = 20;
    /** Reads `spawn`'s arguments: a category word (enemy, critter, generator, item, object)
     * may come first, else the name says which it is; a map may come last, or first with an
     * object. Nothing, with `error` set, when they make no sense. */
    static std::optional<Spawn> parseSpawn(Arguments arguments, std::string& error);
    /** Whether the swarm can be asked for `kind`: its tiers, Death and IT; the great ones are
     * critters. */
    static bool spawnableEnemy(s32 kind);

    /** Every item `get` knows, one line each with what it does. */
    static std::vector<std::string> itemLines();
    /** Every realm's levels on a line of their own, by name and title. */
    static std::vector<std::string> mapLines(const LevelCatalog& catalog);
    /** What a character is, carries and has gathered. */
    static std::vector<std::string> inventoryLines(const CharacterSave& save);
    /** The swarm's kinds `spawn` takes, by the codes the levels name them. */
    static std::vector<std::string> enemyKindLines();
    /** The great ones `spawn` takes. */
    static std::vector<std::string> critterLines();
    /** A level's roster: the kinds its generators breed for each class of placement. */
    static std::vector<std::string> rosterLines(std::span<const LevelEnemy> roster);
    /** A layout's generators, by the kind each names, with their tiers. */
    static std::vector<std::string> generatorLines(const WorldLayout& layout);
    /** A layout's placed enemies, by name. */
    static std::vector<std::string> placedEnemyLines(const WorldLayout& layout);
    /** The great ones a layout places, by name. */
    static std::vector<std::string> placedCritterLines(const WorldLayout& layout);
    /** A layout's pickup records, by name and what each is. */
    static std::vector<std::string> pickupLines(const WorldLayout& layout);
    /** A layout's objects: how many, and each distinct name with its count. */
    static std::vector<std::string> objectLines(const WorldLayout& layout);
    /** A layout's generator record named `name`, or null. */
    static const ItemInfo* generatorRecord(const WorldLayout& layout, std::string_view name);
    /** Whether a layout has an object or a pickup record named `name`, in any case. */
    static bool hasObject(const WorldLayout& layout, std::string_view name);
    static bool hasPickup(const WorldLayout& layout, std::string_view name);
    /** The console's name for a held powerup, or its kind and flags for one it has none for. */
    static std::string powerupName(const PowerupSlot& slot);
    /** The console's name for a potion's kind. */
    static std::string_view potionName(s32 kind);
    /** What a pickup record's subtype is: "keys", "gold", "powerup"... */
    static std::string_view pickupKindName(s32 subtype);

    /** Opens the tower in a character's record: every level of every realm beaten, every
     * runestone and every boss's shard held, every crystal gate met and every gate's opening
     * already announced, nothing left to present. The legend items are not touched. */
    static void unlockTower(CharacterSave& save);
};

} // namespace gdl::game
