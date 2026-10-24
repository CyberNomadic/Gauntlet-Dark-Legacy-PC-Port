#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/characters/CharacterSave.h"
#include "game/characters/Inventory.h"
#include "game/level/LevelCatalog.h"
#include "game/players/ItemPickup.h"

namespace gdl::game {

/**
 * The developer console's commands as far as they need no scene: reading their arguments,
 * listing the maps, the items and a character's inventory, and opening the tower in a
 * character's record. The application wires them to the party and the level in play.
 */
class ConsoleCommands {
public:
    using Arguments = std::span<const std::string>;

    /** What `list` is asked for. */
    enum class Listing : u8 { Maps, Items, Inventory, Unknown };
    static Listing listingOf(Arguments arguments);
    static constexpr std::string_view kListUsage = "list maps, list items or list inventory";

    /** Where `goto` sends the party: a level by name, a point, or a point in a level. */
    struct Destination {
        std::optional<std::string> level; ///< as the catalogue names it, "G1"; "TOWER" the tower
        std::optional<Vec3> position;
        bool operator==(const Destination&) const = default;
    };
    static constexpr std::string_view kGotoUsage = "goto <map> [x y z], or goto x y z";
    /** Reads `goto`'s arguments; nothing, with `error` set, when they make no sense. */
    static std::optional<Destination> parseGoto(Arguments arguments, std::string& error);

    /** What `get` hands over. */
    struct Gift {
        enum class Kind : u8 { Item, Experience, Level };
        Kind kind = Kind::Item;
        ItemOffer offer; ///< the item, as if it lay in the level
        s32 amount = 0;  ///< the experience to add or the level to reach
        std::string name;
    };
    static constexpr std::string_view kGetUsage =
        "get <item> [amount], get exp <points> or get level <level>; list items names the items";
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

    /** Every item `get` knows, one line each with what it does. */
    static std::vector<std::string> itemLines();
    /** Every realm's levels on a line of their own, by name and title. */
    static std::vector<std::string> mapLines(const LevelCatalog& catalog);
    /** What a character is, carries and has gathered. */
    static std::vector<std::string> inventoryLines(const CharacterSave& save);
    /** The console's name for a held powerup, or its kind and flags for one it has none for. */
    static std::string powerupName(const PowerupSlot& slot);
    /** The console's name for a potion's kind. */
    static std::string_view potionName(s32 kind);

    /** Opens the tower in a character's record: every level of every realm beaten, every
     * runestone and every boss's shard held, every crystal gate met and every gate's opening
     * already announced, nothing left to present. The legend items are not touched. */
    static void unlockTower(CharacterSave& save);
};

} // namespace gdl::game
