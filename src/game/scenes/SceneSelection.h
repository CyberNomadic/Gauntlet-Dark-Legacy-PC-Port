#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * A thing picked in the view for the developer console: what kind it is and which of its
 * kind, with the name it goes by. The console hands it on as a token, "object#12" or
 * "enemy#3", which commands take as an argument through $SELECTED.
 */
struct SceneSelection {
    enum class Kind : u8 {
        Object,
        Enemy,
        Critter,
        Generator,
        Pickup,
        Player,
        Spawned,
        Switch, ///< a trigger's pad, by the level's trigger index
        Gate,   ///< a door a key opens, by the gates' index
        Chest   ///< by the chests' index
    };

    Kind kind = Kind::Object;
    s32 id = -1;      ///< the layout's object index, the swarm's slot, the pickup's index...
    std::string name; ///< an object's or pickup's record name, an enemy's kind, a player's name

    bool operator==(const SceneSelection&) const = default;

    /** The kind's word in a token: "object", "enemy"... */
    static std::string_view kindName(Kind kind);
    /** The token: the kind's word, a hash and the index, "generator#2". */
    std::string token() const;
    /** What the console says of it: the token and the name, "object#12 K4XS#66". */
    std::string label() const;
    /** Reads a token, in any case; nothing for a word that is not one. The name is left
     * empty: the scene knows it. */
    static std::optional<SceneSelection> parse(std::string_view token);
};

} // namespace gdl::game
