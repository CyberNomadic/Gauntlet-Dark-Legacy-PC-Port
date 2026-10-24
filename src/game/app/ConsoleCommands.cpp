#include "game/app/ConsoleCommands.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <format>
#include <map>
#include <set>
#include <system_error>

#include "game/characters/ClassData.h"
#include "game/characters/LevelRecord.h"
#include "game/characters/Progression.h"
#include "game/characters/Relics.h"
#include "game/enemies/DeathRules.h"
#include "game/enemies/EnemyKinds.h"
#include "game/level/LevelTriggers.h"
#include "game/level/TowerAccess.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

namespace {
constexpr s32 kDefaultCharges = 10;
constexpr f32 kForGood = -1.0f; ///< a powerup's strength under none lasts
constexpr s32 kMostTier = 3;
constexpr std::string_view kGargoylePrefix = "GAR_";

struct Potion {
    std::string_view name;
    s32 kind;
};
/** The kinds the shop sells, by the colour each burst takes. */
constexpr std::array<Potion, 4> kPotions{
    {{"fire", 1}, {"electric", 2}, {"lightning", 3}, {"acid", 4}}};

struct Powerup {
    std::string_view name;
    std::string_view summary;
    s32 kind;
    u32 flags;
    bool charged; ///< spent a charge at a time rather than timed
    f32 charge;   ///< a stat boost, for one that has one
};
constexpr std::array<Powerup, 28> kPowerups{{
    {"levitate", "held above the ground", powerup::kSpecial, powerup::kLevitation, false, 0},
    {"invulnerable", "takes no harm", powerup::kArmor, powerup::kInvulnerable, false, 0},
    {"goldinvulnerable", "takes no harm, gilded", powerup::kArmor, powerup::kGoldInvulnerable,
     false, 0},
    {"antideath", "a halo Death cannot take", powerup::kArmor, DeathRules::kProtection, false, 0},
    {"reflectshield", "turns missiles back", powerup::kArmor, powerup::kReflectShield, false, 0},
    {"fireshield", "burns what it is against", powerup::kArmor, powerup::kFireShield, false, 0},
    {"lightningshield", "shocks what it is against", powerup::kArmor, powerup::kLightningShield,
     false, 0},
    {"supershot", "one huge shot a charge", powerup::kWeapon, powerup::kSuperShot, true, 0},
    {"threeway", "three shots at once", powerup::kWeapon, powerup::kThreeWayShot, false, 0},
    {"fiveway", "five shots at once", powerup::kWeapon, powerup::kFiveWayShot, false, 0},
    {"rapidfire", "shoots faster", powerup::kWeapon, powerup::kRapidFire, false, 0},
    {"reflect", "shots that bounce off the walls", powerup::kWeapon, powerup::kReflect, false, 0},
    {"thunderhammer", "a hammer blow of lightning a charge", powerup::kWeapon,
     powerup::kThunderHammer, true, 0},
    {"firebreath", "breathes fire a charge", powerup::kSpecial, powerup::kFireBreath, true, 0},
    {"acidbreath", "breathes acid a charge", powerup::kSpecial, powerup::kAcidBreath, true, 0},
    {"lightningbreath", "breathes lightning a charge", powerup::kSpecial, powerup::kLightningBreath,
     true, 0},
    {"phoenix", "a fire-spitting companion", powerup::kSpecial, powerup::kPhoenix, false, 0},
    {"growth", "grows the character", powerup::kSpecial, powerup::kGrowth, false, 0},
    {"shrink", "shrinks the swarm", powerup::kSpecial, powerup::kEnemyShrink, false, 0},
    {"invisible", "unseen by the enemies", powerup::kSpecial, powerup::kInvisible, false, 0},
    {"xray", "sees inside the nearest closed chest", powerup::kSpecial, powerup::kXRay, false, 0},
    {"stoptime", "halts ordinary opponents and hazards", powerup::kSpecial, powerup::kStopTime,
     false, 0},
    {"turbo", "fills the turbo meter at once", powerup::kSpecial, powerup::kTurbo, false, 0},
    {"speed", "runs faster", powerup::kSpecial, powerup::kSpeedBoost, false, 4},
    {"mikey", "a decoy the swarm chases", powerup::kSpecial, powerup::kMikey, false, 0},
    {"handofdeath", "returns melee harm to the striker", powerup::kSpecial, powerup::kHandOfDeath,
     false, 0},
    {"healthvamp", "melee harm heals the character", powerup::kSpecial, powerup::kHealthVamp, false,
     0},
    {"pojo", "Pojo speaks for the character", powerup::kSpecial, powerup::kPojo, false, 0},
}};

struct Critter {
    std::string_view name;
    CombatantKind kind;
};
constexpr std::array<Critter, 3> kCritters{{{"golem", CombatantKind::Golem},
                                            {"general", CombatantKind::General},
                                            {"gargoyle", CombatantKind::Gargoyle}}};
constexpr std::array<std::string_view, 3> kGargoyleForms{"EAGL", "LION", "SERP"};

std::string lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

std::string upper(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return result;
}

std::optional<s32> integer(std::string_view text) {
    s32 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() ? std::optional{value}
                                                                    : std::nullopt;
}

std::optional<f32> number(std::string_view text) {
    f32 value = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() ? std::optional{value}
                                                                    : std::nullopt;
}

std::optional<Vec3> point(ConsoleCommands::Arguments arguments) {
    if (arguments.size() != 3) {
        return std::nullopt;
    }
    const auto x = number(arguments[0]);
    const auto y = number(arguments[1]);
    const auto z = number(arguments[2]);
    if (!x || !y || !z) {
        return std::nullopt;
    }
    return Vec3{*x, *y, *z};
}

const Powerup* powerupNamed(std::string_view name) {
    const auto found = std::ranges::find(kPowerups, name, &Powerup::name);
    return found != kPowerups.end() ? &*found : nullptr;
}

const Potion* potionNamed(std::string_view name) {
    const auto found = std::ranges::find(kPotions, name, &Potion::name);
    return found != kPotions.end() ? &*found : nullptr;
}

/** A positive count from the one optional argument, else `fallback`; nothing on nonsense. */
std::optional<s32> countOf(ConsoleCommands::Arguments rest, s32 fallback, std::string& error) {
    if (rest.empty()) {
        return fallback;
    }
    const auto value = rest.size() == 1 ? integer(rest[0]) : std::nullopt;
    if (!value.has_value() || *value <= 0) {
        error = std::string(ConsoleCommands::kGetUsage);
        return std::nullopt;
    }
    return value;
}

/** The name as a count of it reads: one key, two keys. */
std::string counted(const ConsoleCommands::Gift& gift, s32 count) {
    if (gift.name == "keys" || gift.name == "potions") {
        return count == 1 ? gift.name.substr(0, gift.name.size() - 1) : gift.name;
    }
    return gift.name;
}

/** `name x3` for a count over one, else the name alone. */
std::string tallied(std::string_view name, s32 count) {
    return count > 1 ? std::format("{} x{}", name, count) : std::string(name);
}

/** The names of a map joined ", ", each with its count. */
std::string joined(const std::map<std::string, s32>& counts) {
    std::string line;
    for (const auto& [name, count] : counts) {
        line += line.empty() ? "" : ", ";
        line += tallied(name, count);
    }
    return line;
}

/** The layout's record of an instance, or null for one with none. */
const ItemInfo* recordOf(const WorldLayout& layout, const ItemInstance& instance) {
    const auto& infos = layout.itemInfos();
    return instance.info >= 0 && static_cast<usize>(instance.info) < infos.size()
               ? &infos[static_cast<usize>(instance.info)]
               : nullptr;
}

bool critterName(std::string_view name) {
    return name == "GOLEM" || name == "GENERAL" || name.starts_with(kGargoylePrefix) ||
           name == "GAR";
}

bool sameIgnoringCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

/** A tier from an optional argument, else one; nothing when it is no tier. */
std::optional<s32> tierOf(std::optional<std::string_view> argument, std::string& error) {
    if (!argument.has_value()) {
        return 1;
    }
    const auto tier = integer(*argument);
    if (!tier.has_value() || *tier < 1 || *tier > kMostTier) {
        error = std::format("A tier is 1 to {}", kMostTier);
        return std::nullopt;
    }
    return tier;
}
} // namespace

ConsoleCommands::Listing ConsoleCommands::listingOf(Arguments arguments) {
    if (arguments.empty()) {
        return Listing::Unknown;
    }
    const std::string what = lower(arguments[0]);
    if (what == "maps") {
        return Listing::Maps;
    }
    if (what == "items") {
        return Listing::Items;
    }
    if (what == "inventory") {
        return Listing::Inventory;
    }
    if (what == "enemies") {
        return Listing::Enemies;
    }
    if (what == "critters") {
        return Listing::Critters;
    }
    if (what == "generators") {
        return Listing::Generators;
    }
    if (what == "objects") {
        return Listing::Objects;
    }
    return Listing::Unknown;
}

std::optional<std::string> ConsoleCommands::listedMap(Arguments arguments) {
    return arguments.size() >= 2 ? std::optional{upper(arguments[1])} : std::nullopt;
}

std::optional<ConsoleCommands::Destination> ConsoleCommands::parseGoto(Arguments arguments,
                                                                       std::string& error) {
    error = std::string(kGotoUsage);
    if (arguments.size() == 1) {
        return Destination{.level = upper(arguments[0])};
    }
    if (arguments.size() == 3) {
        if (const auto at = point(arguments)) {
            return Destination{.position = at};
        }
        return std::nullopt;
    }
    if (arguments.size() == 4) {
        if (const auto at = point(arguments.subspan(1))) {
            return Destination{.level = upper(arguments[0]), .position = at};
        }
    }
    return std::nullopt;
}

std::optional<ConsoleCommands::Gift> ConsoleCommands::parseGet(Arguments arguments,
                                                               std::string& error) {
    if (arguments.empty()) {
        error = std::string(kGetUsage);
        return std::nullopt;
    }
    const std::string name = lower(arguments[0]);
    const Arguments rest = arguments.subspan(1);
    Gift gift;
    if (name == "exp" || name == "experience") {
        const auto points = rest.size() == 1 ? integer(rest[0]) : std::nullopt;
        if (!points.has_value() || *points <= 0) {
            error = "get exp <points>: how much to add";
            return std::nullopt;
        }
        gift.kind = Gift::Kind::Experience;
        gift.amount = *points;
        gift.name = "exp";
        return gift;
    }
    if (name == "level") {
        const auto level = rest.size() == 1 ? integer(rest[0]) : std::nullopt;
        if (!level.has_value() || *level < 1 || *level > kMaxLevel) {
            error = std::format("get level <level>: 1 to {}", kMaxLevel);
            return std::nullopt;
        }
        gift.kind = Gift::Kind::Level;
        gift.amount = *level;
        gift.name = "level";
        return gift;
    }
    if (name == "key" || name == "keys") {
        const auto count = countOf(rest, kDefaultKeys, error);
        if (!count) {
            return std::nullopt;
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Keys), *count};
        gift.name = "keys";
        return gift;
    }
    if (name == "potion" || name == "potions") {
        s32 count = kDefaultPotions;
        s32 kind = kDefaultPotionKind;
        for (const std::string& argument : rest) {
            if (const auto value = integer(argument); value.has_value() && *value > 0) {
                count = *value;
            } else if (const Potion* potion = potionNamed(lower(argument))) {
                kind = potion->kind;
            } else {
                error = std::format("No potion kind named {}; fire, electric, lightning or acid",
                                    argument);
                return std::nullopt;
            }
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Potion), count, static_cast<u32>(kind)};
        gift.name = "potions";
        return gift;
    }
    if (name == "gold") {
        const auto count = countOf(rest, kDefaultGold, error);
        if (!count) {
            return std::nullopt;
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Gold), *count};
        gift.name = "gold";
        return gift;
    }
    if (name == "health" || name == "food") {
        const auto count = countOf(rest, kDefaultHealth, error);
        if (!count) {
            return std::nullopt;
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Food), *count};
        gift.name = "health";
        return gift;
    }
    const Powerup* powerup = powerupNamed(name);
    if (powerup == nullptr) {
        error = std::format("No item named {}; list items names them", arguments[0]);
        return std::nullopt;
    }
    gift.name = std::string(powerup->name);
    if (powerup->charged) {
        const auto charges = countOf(rest, kDefaultCharges, error);
        if (!charges) {
            return std::nullopt;
        }
        gift.offer = ItemOffer{powerup->kind, *charges, powerup->flags, kForGood};
        return gift;
    }
    f32 seconds = kDefaultSeconds;
    if (!rest.empty()) {
        const auto given = rest.size() == 1 ? number(rest[0]) : std::nullopt;
        if (!given.has_value() || *given == 0.0f) {
            error =
                std::format("get {} [seconds]: how long for; under nought for good", powerup->name);
            return std::nullopt;
        }
        seconds = *given < 0.0f ? kForGood : *given;
    }
    gift.offer =
        ItemOffer{powerup->kind, static_cast<s32>(powerup->charge), powerup->flags, seconds};
    return gift;
}

std::string ConsoleCommands::report(const Gift& gift, const ItemTaking& taking) {
    const Powerup* powerup = powerupNamed(gift.name);
    switch (taking.outcome) {
    case ItemTaking::Outcome::Taken:
    case ItemTaking::Outcome::Shown:
        if (powerup == nullptr) {
            return std::format("Got {} {}", taking.count, counted(gift, taking.count));
        }
        if (powerup->charged) {
            return std::format("Got {}, {} charges", gift.name, gift.offer.amount);
        }
        if (gift.offer.strength < 0.0f) {
            return std::format("Got {} for good", gift.name);
        }
        return std::format("Got {} for {:.0f} seconds", gift.name, gift.offer.strength);
    case ItemTaking::Outcome::PartTaken:
        return std::format("Got {} {}; no room for {} more", taking.count,
                           counted(gift, taking.count), taking.left);
    case ItemTaking::Outcome::HealthFull: return "Health is full already";
    case ItemTaking::Outcome::GoldFull:
    case ItemTaking::Outcome::KeysFull:
    case ItemTaking::Outcome::PotionsFull: return std::format("No room for more {}", gift.name);
    case ItemTaking::Outcome::AlreadyHeld: return std::format("Already holding {}", gift.name);
    case ItemTaking::Outcome::NotCarried: break;
    }
    return std::format("Cannot carry {}", gift.name);
}

bool ConsoleCommands::spawnableEnemy(s32 kind) {
    return (kind >= 0 && kind < kSwarmKindCount) || kind == kDeathKind || kind == kItKind;
}

std::optional<ConsoleCommands::Spawn> ConsoleCommands::parseSpawn(Arguments arguments,
                                                                  std::string& error) {
    if (arguments.empty()) {
        error = std::string(kSpawnUsage);
        return std::nullopt;
    }
    const std::string name = upper(arguments[0]);
    const Arguments rest = arguments.subspan(1);
    const auto argument = [&](usize index) {
        return index < rest.size() ? std::optional<std::string_view>{rest[index]} : std::nullopt;
    };
    Spawn spawn;
    if (const auto kind = enemyKindOf(name); kind.has_value() && spawnableEnemy(*kind)) {
        spawn.kind = Spawn::Kind::Enemy;
        spawn.enemyKind = *kind;
        spawn.name = name;
        const auto tier = tierOf(argument(0), error);
        if (!tier.has_value()) {
            return std::nullopt;
        }
        spawn.tier = *tier;
        if (const auto count = argument(1)) {
            const auto value = integer(*count);
            if (!value.has_value() || *value < 1 || *value > kMostSpawned) {
                error = std::format("A count is 1 to {}", kMostSpawned);
                return std::nullopt;
            }
            spawn.count = *value;
        }
        if (rest.size() > 2) {
            error = std::string(kSpawnUsage);
            return std::nullopt;
        }
        return spawn;
    }
    const std::string lowered = lower(arguments[0]);
    if (const auto critter = std::ranges::find(kCritters, lowered, &Critter::name);
        critter != kCritters.end()) {
        spawn.kind = Spawn::Kind::Critter;
        spawn.critter = critter->kind;
        spawn.name = std::string(critter->name);
        if (critter->kind == CombatantKind::Gargoyle) {
            const std::string form = upper(argument(0).value_or(kGargoyleForms.front()));
            if (std::ranges::find(kGargoyleForms, form) == kGargoyleForms.end()) {
                error = "A gargoyle is an eagl, a lion or a serp";
                return std::nullopt;
            }
            spawn.form = std::string(kGargoylePrefix) + form;
        } else if (!rest.empty()) {
            error = std::string(kSpawnUsage);
            return std::nullopt;
        }
        return spawn;
    }
    if (lowered == "generator") {
        const auto bred = argument(0);
        const auto kind = bred.has_value() ? enemyKindOf(upper(*bred)) : std::nullopt;
        if (!kind.has_value() || !spawnableEnemy(*kind)) {
            error = "spawn generator <enemy> [tier]; list enemies names the kinds";
            return std::nullopt;
        }
        spawn.kind = Spawn::Kind::Generator;
        spawn.enemyKind = *kind;
        spawn.name = upper(*bred);
        const auto tier = tierOf(argument(1), error);
        if (!tier.has_value()) {
            return std::nullopt;
        }
        spawn.tier = *tier;
        return spawn;
    }
    if (!rest.empty()) {
        error = std::string(kSpawnUsage);
        return std::nullopt;
    }
    spawn.kind = Spawn::Kind::Thing;
    spawn.name = name;
    return spawn;
}

std::vector<std::string> ConsoleCommands::itemLines() {
    constexpr s32 kNameColumn = 18;
    std::vector<std::string> lines;
    const auto add = [&](std::string_view name, std::string_view what) {
        lines.push_back(std::format("{:<{}} {}", name, kNameColumn, what));
    };
    add("key [n]", std::format("keys for the doors ({})", kDefaultKeys));
    add("potion [n] [kind]", std::format("potions: fire, electric, lightning or acid ({} {})",
                                         kDefaultPotions, potionName(kDefaultPotionKind)));
    add("gold [n]", std::format("gold for the shop ({})", kDefaultGold));
    add("health [n]", std::format("health, as food gives it ({})", kDefaultHealth));
    add("exp <points>", "experience to add");
    add("level <level>", std::format("the level to be, 1 to {}", kMaxLevel));
    for (const Powerup& powerup : kPowerups) {
        if (powerup.charged) {
            add(std::format("{} [n]", powerup.name),
                std::format("{} ({} charges)", powerup.summary, kDefaultCharges));
        } else {
            add(std::format("{} [s]", powerup.name),
                std::format("{} ({:.0f} s; under nought for good)", powerup.summary,
                            kDefaultSeconds));
        }
    }
    return lines;
}

std::vector<std::string> ConsoleCommands::mapLines(const LevelCatalog& catalog) {
    std::vector<std::string> lines;
    for (const LevelCatalog::Realm& realm : catalog.realms()) {
        const char letter =
            realm.prefix.empty()
                ? '?'
                : static_cast<char>(std::toupper(static_cast<unsigned char>(realm.prefix.back())));
        std::string line = std::format("{} {} ({}):", letter, lower(realm.file), realm.id);
        for (usize i = 0; i < realm.levels.size(); ++i) {
            const std::string_view title =
                i < realm.titles.size() ? std::string_view(realm.titles[i]) : std::string_view{};
            line += std::format("{} {} {}", i == 0 ? "" : ",", realm.levels[i], title);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> ConsoleCommands::enemyKindLines() {
    std::string names;
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        if (spawnableEnemy(kind)) {
            names += names.empty() ? "" : ", ";
            names += enemyKind(kind).name;
        }
    }
    return {std::format("Enemies (spawn <enemy> [tier 1-{}] [count]): {}", kMostTier, names)};
}

std::vector<std::string> ConsoleCommands::critterLines() {
    std::string forms;
    for (const std::string_view form : kGargoyleForms) {
        forms += forms.empty() ? "" : "|";
        forms += lower(form);
    }
    return {std::format("Critters (spawn <critter>): golem, general, gargoyle [{}]", forms)};
}

std::vector<std::string> ConsoleCommands::rosterLines(std::span<const LevelEnemy> roster) {
    constexpr std::array<std::string_view, 5> kClasses{"", "small", "medium", "large",
                                                       "medium other"};
    std::string line;
    for (const LevelEnemy& enemy : roster) {
        if (enemy.kind < 0 || enemy.kind >= kEnemyKindCount) {
            continue;
        }
        const std::string_view label =
            enemy.subtype >= 0 && static_cast<usize>(enemy.subtype) < kClasses.size()
                ? kClasses[static_cast<usize>(enemy.subtype)]
                : std::string_view{};
        line += line.empty() ? "" : ", ";
        line += label.empty() ? std::string(enemyKind(enemy.kind).name)
                              : std::format("{} {}", label, enemyKind(enemy.kind).name);
    }
    return {line.empty() ? std::string("Roster: none") : std::format("Roster: {}", line)};
}

std::vector<std::string> ConsoleCommands::generatorLines(const WorldLayout& layout) {
    std::map<std::string, std::pair<s32, std::set<s32>>> kinds; // count and tiers by name
    for (const ItemInstance& instance : layout.itemInstances()) {
        const ItemInfo* info = recordOf(layout, instance);
        if (info == nullptr || info->type != ItemInfo::kGenerator) {
            continue;
        }
        auto& [count, tiers] = kinds[info->name];
        ++count;
        tiers.insert(std::max(static_cast<s32>(instance.params[0]), 1));
    }
    std::string line;
    for (const auto& [name, record] : kinds) {
        std::string tiers;
        for (const s32 tier : record.second) {
            tiers += tiers.empty() ? "" : ", ";
            tiers += std::to_string(tier);
        }
        line += line.empty() ? "" : "; ";
        line += std::format("{} (tier {})", tallied(name, record.first), tiers);
    }
    return {line.empty() ? std::string("Generators: none") : std::format("Generators: {}", line)};
}

std::vector<std::string> ConsoleCommands::placedEnemyLines(const WorldLayout& layout) {
    std::map<std::string, s32> counts;
    for (const ItemInstance& instance : layout.itemInstances()) {
        const ItemInfo* info = recordOf(layout, instance);
        if (info != nullptr && info->type == ItemInfo::kPlacedEnemy && !critterName(info->name)) {
            ++counts[info->name];
        }
    }
    return {counts.empty() ? std::string("Placed enemies: none")
                           : std::format("Placed enemies: {}", joined(counts))};
}

std::vector<std::string> ConsoleCommands::placedCritterLines(const WorldLayout& layout) {
    std::map<std::string, s32> counts;
    for (const ItemInstance& instance : layout.itemInstances()) {
        const ItemInfo* info = recordOf(layout, instance);
        if (info != nullptr && info->type == ItemInfo::kPlacedEnemy && critterName(info->name)) {
            ++counts[info->name];
        }
    }
    return {counts.empty() ? std::string("Critters placed: none")
                           : std::format("Critters placed: {}", joined(counts))};
}

std::string_view ConsoleCommands::pickupKindName(s32 subtype) {
    switch (static_cast<ItemKind>(subtype)) {
    case ItemKind::Gold: return "gold";
    case ItemKind::Keys: return "keys";
    case ItemKind::Food: return "food";
    case ItemKind::Potion: return "potion";
    case ItemKind::WeaponPowerup: return "weapon powerup";
    case ItemKind::ArmorPowerup: return "armor powerup";
    case ItemKind::SpeedPowerup: return "speed powerup";
    case ItemKind::MagicPowerup: return "magic powerup";
    case ItemKind::SpecialPowerup: return "special powerup";
    case ItemKind::Runestone: return "runestone";
    case ItemKind::Legend: return "legend item";
    case ItemKind::Scroll: return "scroll";
    case ItemKind::Crystal: return "crystal";
    case ItemKind::GargoyleKey: return "gargoyle piece";
    }
    return "pickup";
}

std::vector<std::string> ConsoleCommands::pickupLines(const WorldLayout& layout) {
    std::string line;
    std::set<std::string> seen;
    for (const ItemInfo& info : layout.itemInfos()) {
        if (info.type != ItemInfo::kPowerup || !seen.insert(info.name).second) {
            continue;
        }
        line += line.empty() ? "" : ", ";
        line += std::format("{} ({})", info.name, pickupKindName(info.subtype));
    }
    return {line.empty() ? std::string("Pickups: none")
                         : std::format("Pickups (spawn <name>): {}", line)};
}

std::vector<std::string> ConsoleCommands::objectLines(const WorldLayout& layout) {
    std::map<std::string, s32> counts;
    for (const WorldObject& object : layout.objects()) {
        ++counts[object.name];
    }
    if (counts.empty()) {
        return {"Objects: none"};
    }
    return {std::format("Objects ({}, {} distinct): {}", layout.objects().size(), counts.size(),
                        joined(counts))};
}

bool ConsoleCommands::hasObject(const WorldLayout& layout, std::string_view name) {
    return std::ranges::any_of(layout.objects(), [&](const WorldObject& object) {
        return sameIgnoringCase(object.name, name);
    });
}

bool ConsoleCommands::hasPickup(const WorldLayout& layout, std::string_view name) {
    return std::ranges::any_of(layout.itemInfos(), [&](const ItemInfo& info) {
        return info.type == ItemInfo::kPowerup && sameIgnoringCase(info.name, name);
    });
}

std::string_view ConsoleCommands::potionName(s32 kind) {
    const auto found = std::ranges::find(kPotions, kind, &Potion::kind);
    return found != kPotions.end() ? found->name : kPotions.front().name;
}

std::string ConsoleCommands::powerupName(const PowerupSlot& slot) {
    std::string names;
    for (const Powerup& powerup : kPowerups) {
        if (powerup.kind == slot.kind && (slot.flags & powerup.flags) == powerup.flags) {
            names += names.empty() ? "" : "+";
            names += powerup.name;
        }
    }
    return names.empty() ? std::format("kind {} flags {:#x}", slot.kind, slot.flags) : names;
}

std::vector<std::string> ConsoleCommands::inventoryLines(const CharacterSave& save) {
    const ClassProgress& progress = save.progress();
    const s32 level = experienceLevel(progress.experience);
    std::vector<std::string> lines;
    lines.push_back(std::format("{} the {} at level {} ({} experience), health {}/{}, {} gold",
                                save.name, classCode(save.character), level, progress.experience,
                                save.health(), mostHealth(level), save.gold));
    std::string carried =
        std::format("Keys {}/{}, potions {}/{}", progress.inventory.keys, Inventory::kMostKeys,
                    progress.inventory.potions.size(), Inventory::kMostPotions);
    for (usize i = 0; i < progress.inventory.potions.size(); ++i) {
        carried +=
            std::format("{} {}", i == 0 ? ":" : ",", potionName(progress.inventory.potions[i]));
    }
    lines.push_back(std::move(carried));
    std::string powerups;
    for (const PowerupSlot& slot : progress.inventory.powerups) {
        if (!slot.held()) {
            continue;
        }
        powerups += powerups.empty() ? "Powerups: " : ", ";
        powerups += powerupName(slot);
        powerups += slot.strength < 0.0f ? std::string(" for good")
                                         : std::format(" {:.0f} s", slot.strength);
        if (slot.charge != 0.0f) {
            powerups += std::format(" x{:.0f}", slot.charge);
        }
        if (!slot.on) {
            powerups += " (off)";
        }
    }
    lines.push_back(powerups.empty() ? "No powerups" : std::move(powerups));
    const Relics& relics = progress.relics;
    lines.push_back(std::format(
        "Runestones {}/{}, boss shards {}, legend items {}, gargoyle pieces {}/{} {}/{} {}/{}",
        relics.runeCount(), Relics::kRuneCount, std::popcount(relics.shards),
        std::popcount(relics.legends), relics.gargoylePieces[0], Relics::kGargoyleNeeded[0],
        relics.gargoylePieces[1], Relics::kGargoyleNeeded[1], relics.gargoylePieces[2],
        Relics::kGargoyleNeeded[2]));
    std::string crystals;
    for (usize gate = 0; gate < progress.crystals.size(); ++gate) {
        if (progress.crystals[gate] != 0) {
            crystals += std::format("{}gate {}: {}", crystals.empty() ? "Crystals: " : ", ", gate,
                                    progress.crystals[gate]);
        }
    }
    lines.push_back(crystals.empty() ? "Crystals: none" : std::move(crystals));
    return lines;
}

void ConsoleCommands::unlockTower(CharacterSave& save) {
    ClassProgress& progress = save.progress();
    for (s32 realm = 0; realm < static_cast<s32>(LevelRecord::kRealmCount); ++realm) {
        for (s32 level = 0; level < LevelRecord::kLevelsPerRealm; ++level) {
            progress.levels.recordBeaten(realm, level);
        }
    }
    for (s32 rune = 0; rune < Relics::kRuneCount; ++rune) {
        progress.relics.addRune(rune);
    }
    // The bosses' shards by their realms' places in the tower's order: the eight realms',
    // the temple's and the underworld's (TowerAccess::kBattlefieldShards).
    for (s32 order = 1; (TowerAccess::kBattlefieldShards >> order) != 0; ++order) {
        if ((TowerAccess::kBattlefieldShards & (1U << order)) != 0) {
            progress.relics.addShard(order);
        }
    }
    progress.relics.pendingRunes = 0;
    progress.relics.pendingShards = 0;
    progress.relics.pendingCeremonies = 0;
    for (const s32 gate : TowerAccess::kWorldGates) {
        if (gate != 0) {
            progress.crystals[static_cast<usize>(gate)] = std::max(
                progress.crystals[static_cast<usize>(gate)], LevelTriggers::crystalsNeeded(gate));
        }
    }
    progress.unlocked = (1U << static_cast<u32>(kRealmCount)) - 1U;
}

} // namespace gdl::game
