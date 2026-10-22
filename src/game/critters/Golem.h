#pragma once
#include "game/critters/CombatantDefinition.h"
namespace gdl::game {
/** Realm-costumed heavy fighter, with reduced knockback. */
struct Golem {
    static CombatantDefinition definition(char realm = 'G');
};
} // namespace gdl::game
