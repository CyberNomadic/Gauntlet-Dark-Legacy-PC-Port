#include <algorithm>
#include <cctype>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/app/ConsoleCommands.h"
#include "game/characters/Progression.h"
#include "game/enemies/EnemyKinds.h"
#include "game/level/LevelCatalog.h"
#include "game/level/TowerAccess.h"
#include "game/players/ItemPickup.h"
#include "game/players/PowerupEffects.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Args = std::vector<std::string>;
using Kind = ConsoleCommands::Gift::Kind;

bool mentions(std::span<const std::string> lines, std::string_view text) {
    return std::ranges::any_of(lines, [&](const std::string& line) { return line.contains(text); });
}

TEST_CASE("list knows maps, items and inventory in any case", "[dev-console][commands]") {
    CHECK(ConsoleCommands::listingOf(Args{"maps"}) == ConsoleCommands::Listing::Maps);
    CHECK(ConsoleCommands::listingOf(Args{"ITEMS"}) == ConsoleCommands::Listing::Items);
    CHECK(ConsoleCommands::listingOf(Args{"Inventory"}) == ConsoleCommands::Listing::Inventory);
    CHECK(ConsoleCommands::listingOf(Args{}) == ConsoleCommands::Listing::Unknown);
    CHECK(ConsoleCommands::listingOf(Args{"shops"}) == ConsoleCommands::Listing::Unknown);
}

TEST_CASE("goto takes a map, a point, or a point in a map", "[dev-console][commands]") {
    std::string error;
    auto level = ConsoleCommands::parseGoto(Args{"g1"}, error);
    REQUIRE(level.has_value());
    CHECK(level->level == "G1");
    CHECK_FALSE(level->position.has_value());
    auto point = ConsoleCommands::parseGoto(Args{"10", "2.5", "-3"}, error);
    REQUIRE(point.has_value());
    CHECK_FALSE(point->level.has_value());
    REQUIRE(point->position.has_value());
    CHECK(point->position->x == Approx(10.0f));
    CHECK(point->position->y == Approx(2.5f));
    CHECK(point->position->z == Approx(-3.0f));
    auto both = ConsoleCommands::parseGoto(Args{"A1", "1", "2", "3"}, error);
    REQUIRE(both.has_value());
    CHECK(both->level == "A1");
    CHECK(both->position == Vec3{1, 2, 3});
    auto tower = ConsoleCommands::parseGoto(Args{"tower"}, error);
    REQUIRE(tower.has_value());
    CHECK(tower->level == "TOWER");
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{}, error).has_value());
    CHECK(error == ConsoleCommands::kGotoUsage);
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"a", "b"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"1", "2"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"G1", "1", "x", "3"}, error).has_value());
}

TEST_CASE("get reads the item, its amount and a potion's kind", "[dev-console][commands]") {
    std::string error;
    const auto get = [&](Args args) {
        const auto gift = ConsoleCommands::parseGet(args, error);
        REQUIRE(gift.has_value());
        return *gift;
    };
    auto key = get({"key"});
    CHECK(key.kind == Kind::Item);
    CHECK(key.offer.kind == static_cast<s32>(ItemKind::Keys));
    CHECK(key.offer.amount == ConsoleCommands::kDefaultKeys);
    CHECK(get({"KEYS", "3"}).offer.amount == 3);
    auto potion = get({"potion"});
    CHECK(potion.offer.kind == static_cast<s32>(ItemKind::Potion));
    CHECK(potion.offer.amount == 1);
    CHECK(potion.offer.flags == static_cast<u32>(ConsoleCommands::kDefaultPotionKind));
    auto acid = get({"potions", "2", "acid"});
    CHECK(acid.offer.amount == 2);
    CHECK(acid.offer.flags == 4);
    auto lightning = get({"potion", "lightning"});
    CHECK(lightning.offer.amount == 1);
    CHECK(lightning.offer.flags == 3);
    CHECK(get({"potion", "electric", "5"}).offer.flags == 2);
    auto gold = get({"gold"});
    CHECK(gold.offer.kind == static_cast<s32>(ItemKind::Gold));
    CHECK(gold.offer.amount == ConsoleCommands::kDefaultGold);
    auto health = get({"health", "50"});
    CHECK(health.offer.kind == static_cast<s32>(ItemKind::Food));
    CHECK(health.offer.amount == 50);
    auto wings = get({"levitate"});
    CHECK(wings.offer.kind == powerup::kSpecial);
    CHECK(wings.offer.flags == powerup::kLevitation);
    CHECK(wings.offer.strength == Approx(ConsoleCommands::kDefaultSeconds));
    CHECK(wings.name == "levitate");
    CHECK(get({"LEVITATE", "-1"}).offer.strength == Approx(-1.0f));
    CHECK(get({"invulnerable", "90"}).offer.strength == Approx(90.0f));
    auto shots = get({"supershot"});
    CHECK(shots.offer.kind == powerup::kWeapon);
    CHECK(shots.offer.flags == powerup::kSuperShot);
    CHECK(shots.offer.amount > 0); // charges: one huge shot each
    CHECK(get({"supershot", "5"}).offer.amount == 5);
    auto halo = get({"antideath"});
    CHECK(halo.offer.kind == powerup::kArmor);
    CHECK(halo.offer.flags == 0x80000);
    auto exp = get({"exp", "500"});
    CHECK(exp.kind == Kind::Experience);
    CHECK(exp.amount == 500);
    CHECK(get({"experience", "7"}).amount == 7);
    auto level = get({"level", "42"});
    CHECK(level.kind == Kind::Level);
    CHECK(level.amount == 42);
    CHECK_FALSE(ConsoleCommands::parseGet(Args{}, error).has_value());
    CHECK(error == ConsoleCommands::kGetUsage);
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"sword"}, error).has_value());
    CHECK(error.contains("sword"));
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"exp"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"exp", "-5"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"level", "0"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"level", "100"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"key", "x"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"key", "0"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseGet(Args{"potion", "slime"}, error).has_value());
}

TEST_CASE("get says what came of the gift", "[dev-console][commands]") {
    std::string error;
    const auto keys = ConsoleCommands::parseGet(Args{"keys", "3"}, error);
    REQUIRE(keys.has_value());
    ItemTaking taking;
    taking.outcome = ItemTaking::Outcome::Taken;
    taking.count = 3;
    CHECK(ConsoleCommands::report(*keys, taking) == "Got 3 keys");
    taking.outcome = ItemTaking::Outcome::PartTaken;
    taking.count = 2;
    taking.left = 1;
    CHECK(ConsoleCommands::report(*keys, taking) == "Got 2 keys; no room for 1 more");
    taking.outcome = ItemTaking::Outcome::KeysFull;
    CHECK(ConsoleCommands::report(*keys, taking) == "No room for more keys");
    const auto wings = ConsoleCommands::parseGet(Args{"levitate", "-1"}, error);
    REQUIRE(wings.has_value());
    taking.outcome = ItemTaking::Outcome::Taken;
    CHECK(ConsoleCommands::report(*wings, taking) == "Got levitate for good");
    const auto timed = ConsoleCommands::parseGet(Args{"levitate", "45"}, error);
    REQUIRE(timed.has_value());
    CHECK(ConsoleCommands::report(*timed, taking) == "Got levitate for 45 seconds");
    taking.outcome = ItemTaking::Outcome::NotCarried;
    CHECK(ConsoleCommands::report(*wings, taking) == "Cannot carry levitate");
}

TEST_CASE("the item list names everything get knows", "[dev-console][commands]") {
    const auto lines = ConsoleCommands::itemLines();
    CHECK(lines.size() >= 30);
    for (const auto* const name : {"key", "potion", "gold", "health", "exp", "level", "levitate",
                                   "antideath", "supershot", "fiveway", "pojo", "fire, electric"}) {
        CHECK(mentions(lines, name));
    }
    std::string error;
    for (const std::string& line : lines) {
        // Every powerup line starts with a name get accepts.
        const std::string name = line.substr(0, line.find(' '));
        if (name == "exp" || name == "level") {
            CHECK(ConsoleCommands::parseGet(Args{name, "5"}, error).has_value());
        } else {
            CHECK(ConsoleCommands::parseGet(Args{name}, error).has_value());
        }
    }
}

TEST_CASE("held powerups and potions are named the way get names them", "[dev-console][commands]") {
    const PowerupSlot wings{
        .strength = 30.0f, .kind = powerup::kSpecial, .flags = powerup::kLevitation};
    CHECK(ConsoleCommands::powerupName(wings) == "levitate");
    const PowerupSlot giant{.strength = 30.0f,
                            .kind = powerup::kSpecial,
                            .flags = powerup::kGrowth | powerup::kEnemyShrink};
    CHECK(ConsoleCommands::powerupName(giant) == "growth+shrink");
    const PowerupSlot odd{.strength = 30.0f, .kind = powerup::kMagic, .flags = 0x12345};
    CHECK(ConsoleCommands::powerupName(odd).contains("kind 8"));
    CHECK(ConsoleCommands::potionName(0) == "fire");
    CHECK(ConsoleCommands::potionName(1) == "fire");
    CHECK(ConsoleCommands::potionName(2) == "electric");
    CHECK(ConsoleCommands::potionName(3) == "lightning");
    CHECK(ConsoleCommands::potionName(4) == "acid");
    CHECK(ConsoleCommands::potionName(9) == "fire");
}

TEST_CASE("the inventory lines tell what a character is, carries and has gathered",
          "[dev-console][commands]") {
    CharacterSave save;
    save.name = "AB";
    save.character = 1;
    save.gold = 55;
    ClassProgress& progress = save.progress();
    progress.experience = levelExperience(7);
    progress.health = 321;
    progress.inventory.keys = 2;
    progress.inventory.potions = {1, 4};
    progress.inventory.addPowerup(powerup::kSpecial, powerup::kLevitation, 0.0f, 30.0f);
    progress.inventory.addPowerup(powerup::kWeapon, powerup::kSuperShot, 4.0f, -1.0f);
    progress.relics.addRune(0);
    progress.relics.addRune(5);
    progress.relics.addShard(3);
    progress.relics.addLegend(2);
    progress.relics.gargoylePieces = {1, 2, 3};
    progress.crystals[3] = 4;
    const auto lines = ConsoleCommands::inventoryLines(save);
    for (const auto* const text :
         {"AB", "level 7", "321", "55", "Keys 2", "fire", "acid", "levitate", "30 s", "supershot",
          "for good", "x4", "2/13", "1/12", "2/20", "3/28"}) {
        CHECK(mentions(lines, text));
    }
    CharacterSave bare;
    bare.name = "CD";
    CHECK(mentions(ConsoleCommands::inventoryLines(bare), "No powerups"));
}

TEST_CASE("unlock_tower opens every wing and portal to the character", "[dev-console][commands]") {
    CharacterSave save;
    save.progress().relics.pendingRunes = 0x3;
    ConsoleCommands::unlockTower(save);
    const TowerAccess access(std::span<const CharacterSave>(&save, 1));
    for (s32 world = 1; world <= 11; ++world) {
        CHECK(access.worldOpen(world));
        for (s32 gate = 0; gate < LevelRecord::kLevelsPerRealm; ++gate) {
            CHECK(access.portalOpen(world, gate));
        }
    }
    CHECK(access.worldOpen(TowerAccess::kTowerWorld));
    CHECK(access.liftsOpen());
    const Relics& relics = save.progress().relics;
    CHECK(relics.runeCount() == Relics::kRuneCount);
    CHECK(relics.pendingRunes == 0);
    CHECK(relics.pendingShards == 0);
    CHECK(relics.legends == 0); // the bosses' items are left to be found
}

TEST_CASE("the map list names every level of every realm", "[dev-console][commands][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto lines = ConsoleCommands::mapLines(catalog);
    CHECK(lines.size() == catalog.realms().size());
    CHECK(mentions(lines, "G1"));
    CHECK(mentions(lines, "A1"));
    for (const LevelCatalog::Realm& realm : catalog.realms()) {
        for (const std::string& name : realm.levels) {
            CHECK(mentions(lines, name));
        }
    }
}

TEST_CASE("list knows what a level holds and the map named after it", "[dev-console][commands]") {
    CHECK(ConsoleCommands::listingOf(Args{"enemies"}) == ConsoleCommands::Listing::Enemies);
    CHECK(ConsoleCommands::listingOf(Args{"Critters"}) == ConsoleCommands::Listing::Critters);
    CHECK(ConsoleCommands::listingOf(Args{"generators"}) == ConsoleCommands::Listing::Generators);
    CHECK(ConsoleCommands::listingOf(Args{"OBJECTS", "g1"}) == ConsoleCommands::Listing::Objects);
    CHECK(ConsoleCommands::listedMap(Args{"objects", "g1"}) == "G1");
    CHECK_FALSE(ConsoleCommands::listedMap(Args{"objects"}).has_value());
}

TEST_CASE("spawn reads an enemy with its tier and count, a critter, a generator or a thing",
          "[dev-console][commands]") {
    using Kind = ConsoleCommands::Spawn::Kind;
    std::string error;
    const auto spawn = [&](Args args) {
        const auto request = ConsoleCommands::parseSpawn(args, error);
        REQUIRE(request.has_value());
        return *request;
    };
    auto grunt = spawn({"gru"});
    CHECK(grunt.kind == Kind::Enemy);
    CHECK(grunt.enemyKind == kGruntKind);
    CHECK(grunt.tier == 1);
    CHECK(grunt.count == 1);
    CHECK(grunt.name == "GRU");
    auto crowd = spawn({"GRU", "3", "5"});
    CHECK(crowd.tier == 3);
    CHECK(crowd.count == 5);
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"gru", "4"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"gru", "1", "0"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"gru", "1", "2", "x"}, error).has_value());
    auto golem = spawn({"golem"});
    CHECK(golem.kind == Kind::Critter);
    CHECK(golem.critter == CombatantKind::Golem);
    CHECK(golem.form.empty());
    CHECK(spawn({"gargoyle"}).form == "GAR_EAGL");
    CHECK(spawn({"Gargoyle", "lion"}).form == "GAR_LION");
    CHECK(spawn({"general"}).critter == CombatantKind::General);
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"gargoyle", "dog"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"general", "x"}, error).has_value());
    auto generator = spawn({"generator", "rat", "2"});
    CHECK(generator.kind == Kind::Generator);
    CHECK(generator.enemyKind == kRatKind);
    CHECK(generator.tier == 2);
    CHECK(generator.name == "RAT");
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"generator"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"generator", "golem"}, error).has_value());
    auto key = spawn({"key"});
    CHECK(key.kind == Kind::Thing);
    CHECK(key.name == "KEY");
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{"key", "x"}, error).has_value());
    CHECK_FALSE(ConsoleCommands::parseSpawn(Args{}, error).has_value());
    CHECK(error == ConsoleCommands::kSpawnUsage);
    CHECK(ConsoleCommands::spawnableEnemy(kGruntKind));
    CHECK(ConsoleCommands::spawnableEnemy(kDeathKind));
    CHECK(ConsoleCommands::spawnableEnemy(kItKind));
    CHECK_FALSE(ConsoleCommands::spawnableEnemy(kGolemEnemyKind));
    CHECK_FALSE(ConsoleCommands::spawnableEnemy(kGargoyleEnemyKind));
    CHECK_FALSE(ConsoleCommands::spawnableEnemy(kGeneralEnemyKind));
    CHECK_FALSE(ConsoleCommands::spawnableEnemy(kEnemyKindCount));
}

TEST_CASE("the enemy, critter and roster lines name what spawn takes", "[dev-console][commands]") {
    const auto enemies = ConsoleCommands::enemyKindLines();
    REQUIRE(enemies.size() == 1);
    CHECK(enemies[0].contains("GRU"));
    CHECK(enemies[0].contains("DEATH"));
    CHECK_FALSE(enemies[0].contains("GOLEM"));
    CHECK_FALSE(enemies[0].contains("NONE"));
    const auto critters = ConsoleCommands::critterLines();
    REQUIRE(critters.size() == 1);
    CHECK(critters[0].contains("golem"));
    CHECK(critters[0].contains("eagl|lion|serp"));
    const std::vector<LevelEnemy> roster{LevelEnemy{.kind = kGruntKind, .subtype = 2},
                                         LevelEnemy{.kind = kRatKind, .subtype = 1}};
    const auto lines = ConsoleCommands::rosterLines(roster);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].contains("medium GRU"));
    CHECK(lines[0].contains("small RAT"));
    CHECK(ConsoleCommands::rosterLines({})[0] == "Roster: none");
    CHECK(ConsoleCommands::pickupKindName(2) == "keys");
    CHECK(ConsoleCommands::pickupKindName(99) == "pickup");
}

TEST_CASE("the layout lines count a level's generators, placed enemies, pickups and objects",
          "[dev-console][commands][assets]") {
    const auto level = test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(level));
    const auto generators = ConsoleCommands::generatorLines(layout);
    REQUIRE(generators.size() == 1);
    CHECK(generators[0].contains("GRU"));
    CHECK(generators[0].contains("tier"));
    CHECK(ConsoleCommands::placedEnemyLines(layout)[0].starts_with("Placed enemies: "));
    CHECK(ConsoleCommands::placedCritterLines(layout)[0].starts_with("Critters placed: "));
    const auto pickups = ConsoleCommands::pickupLines(layout);
    REQUIRE(pickups.size() == 1);
    CHECK(pickups[0].starts_with("Pickups (spawn <name>): "));
    CHECK(pickups[0].contains("(keys)"));
    const auto objects = ConsoleCommands::objectLines(layout);
    REQUIRE(objects.size() == 1);
    CHECK(objects[0].starts_with(std::format("Objects ({}, ", layout.objects().size())));
    CHECK(objects[0].contains(" distinct): "));
    REQUIRE_FALSE(layout.objects().empty());
    std::string lowered = layout.objects().front().name;
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    CHECK(ConsoleCommands::hasObject(layout, lowered));
    CHECK_FALSE(ConsoleCommands::hasObject(layout, "NOTHING_OF_THE_KIND"));
    const auto pickup = std::ranges::find_if(
        layout.itemInfos(), [](const ItemInfo& info) { return info.type == ItemInfo::kPowerup; });
    REQUIRE(pickup != layout.itemInfos().end());
    CHECK(ConsoleCommands::hasPickup(layout, pickup->name));
    CHECK_FALSE(ConsoleCommands::hasPickup(layout, "NOTHING_OF_THE_KIND"));
}

} // namespace
