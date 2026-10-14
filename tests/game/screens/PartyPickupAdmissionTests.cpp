#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/MessageTable.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "game/players/ItemPickup.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/PartyPickups.h"
#include "game/screens/PlayerRuntimeCheckpoint.h"
#include "game/world/LevelWorldCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path pickupStage() {
    const auto root = test::sampleLevel("party-pickup-admission");
    std::string trees = R"({"trees":[)";
    for (const auto* name : {"GOLD", "KEYS", "POTION", "FOOD", "BADFOOD", "UNKNOWN", "RUNE",
                             "LEGEND", "PIECE", "CRYSTAL", "POWERUP", "SCROLL"}) {
        if (trees.back() != '[') {
            trees += ',';
        }
        trees += std::format(
            R"({{"name":"{}","nodes":[{{"name":"ROOT","object":"WALL","parent":-1,
                "position":[0,0,0]}}],"sequences":[{{"name":"READY","frames":8,"rate":30}}]}})",
            name);
    }
    trees += "]}";
    writeTextFile(root / "animations.json", trees);
    writeTextFile(root / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4}],
      "bounds":{"min":[-40,-20,-40],"max":[40,20,40]},
      "itemInfos":[
        {"type":1,"subtype":1,"name":"GOLD","radius":1,"height":1,"value":25},
        {"type":1,"subtype":2,"name":"KEYS","radius":1,"height":1,"value":3},
        {"type":1,"subtype":4,"name":"POTION","radius":1,"height":1,"value":2,"properties":2},
        {"type":1,"subtype":3,"name":"FOOD","radius":1,"height":1,"value":50},
        {"type":1,"subtype":3,"name":"BADFOOD","radius":1,"height":1,"value":-20},
        {"type":1,"subtype":17,"name":"UNKNOWN","radius":1,"height":1},
        {"type":1,"subtype":10,"name":"RUNE","radius":1,"height":1,"value":0},
        {"type":1,"subtype":13,"name":"LEGEND","radius":1,"height":1,"value":1},
        {"type":1,"subtype":16,"name":"PIECE","radius":1,"height":1,"value":0},
        {"type":1,"subtype":15,"name":"CRYSTAL","radius":1,"height":1,"value":0},
        {"type":1,"subtype":9,"name":"POWERUP","radius":1,"height":1,"properties":2,
         "activeOn":10},
        {"type":1,"subtype":14,"name":"SCROLL","radius":1,"height":1,"value":1}],
      "itemInstances":[]})");
    writeTextFile(root / "collision.json", R"({"objects":[{"object":0,
      "normals":[0,1,0,0,1,0],
      "vertices":[-40,0,-40,-40,0,40,40,0,40,-40,0,-40,40,0,40,40,0,-40]}]})");
    writeTextFile(root / "help.json", R"({"fonts":["font32"],"messages":[
      {"name":"FULLOFKEYS","font":0,"scale":1,"lines":["keys fixture"]},
      {"name":"FULLOFBOMBS","font":0,"scale":1,"lines":["potions fixture"]},
      {"name":"HEALTHFULL","font":0,"scale":1,"lines":["health fixture"]},
      {"name":"ALREADYHAVERUNE","font":0,"scale":1,"lines":["rune fixture"]}]})");
    return root;
}

struct Fixture {
    struct State {
        LevelWorldCheckpoint::State world;
        std::array<PlayerRuntimeCheckpoint::State, 2> party;
        bool operator==(const State&) const = default;
    };
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelWorldCheckpoint worldState{world};
    LevelFixtures fixtures;
    MessageTable strings;
    PartyHud hud;
    LevelSoundscape audio;
    ClassDataSet classes;
    std::array<PlayerRuntime, 2> party;
    std::array<std::unique_ptr<PlayerRuntimeCheckpoint>, 2> owners;
    PartyPickups pickups;
    usize posted = 0;
    usize external = 0;

    explicit Fixture(bool secret = false) {
        const auto root = pickupStage();
        LevelRef level{.realm = "TEST", .realmId = 0, .name = "TEST", .directory = "."};
        if (secret) {
            level.realmId = LevelRef::kSecretRealm;
        }
        REQUIRE(world.load(device, root, level));
        world.setPlayerCount(2);
        REQUIRE(worldState.bindResources(device));
        REQUIRE(strings.load(root / "help.json"));
        hud.help().setTexts(&strings);
        for (usize index = 0; index < party.size(); ++index) {
            auto& player = party[index];
            CharacterSave save;
            save.progress().health = mostHealth(1);
            player.actor.spawn(index == 0 ? 3 : 1, save, nullptr, {0, 0, 0}, 0);
            player.actor.settle(world.collision());
            player.entrySave = player.actor.save();
            owners[index] = std::make_unique<PlayerRuntimeCheckpoint>(player, nullptr);
        }
    }
    usize place(std::string_view name) {
        REQUIRE(world.placeItem(device, name, party.front().actor.position()));
        const usize index = world.placedItems().size() - 1;
        REQUIRE(world.placedItems().item(index).takeable());
        REQUIRE(PartyPickups::touchesPickup(party, world, fixtures.chests()));
        return index;
    }
    bool admitted() const {
        return PartyPickups::collectionUnchanged(party, world, fixtures.chests(), classes);
    }
    std::unique_ptr<State> state() const {
        auto w = worldState.checkpoint();
        auto p0 = owners[0]->checkpoint();
        auto p1 = owners[1]->checkpoint();
        REQUIRE(w);
        REQUIRE(p0);
        REQUIRE(p1);
        return std::make_unique<State>(std::move(*w), std::array{std::move(*p0), std::move(*p1)});
    }
    void collect() {
        pickups.collect(device, party,
                        {.world = world,
                         .fixtures = fixtures,
                         .hud = hud,
                         .audio = audio,
                         .classes = classes,
                         .help =
                             [&](s32 id, usize index) {
                                 const bool shown = hud.postHelp(id, index, party, audio);
                                 posted += shown ? 1 : 0;
                                 return shown;
                             },
                         .openMessage =
                             [&](std::string_view, usize) {
                                 ++external;
                                 return true;
                             },
                         .challengeCoin = [&](usize) { ++external; }});
    }
    void checkNoOp() {
        const auto before = state();
        const auto textures = device.texturesCreated;
        const auto uploads = device.textureUpdates;
        for (s32 repeat = 0; repeat < 3; ++repeat) {
            REQUIRE(admitted());
            CHECK(*state() == *before);
            collect(); // the actual live collector, not the admission helper
            CHECK(*state() == *before);
            CHECK(hud.pickups().cards().empty());
            CHECK_FALSE(hud.help().showing());
            CHECK(posted == 0);
            CHECK(external == 0);
        }
        CHECK(device.texturesCreated == textures);
        CHECK(device.textureUpdates == uploads);
    }
};

TEST_CASE("pickup admission retains native first collector order and complete no-op state",
          "[pickup-admission][pickups]") {
    const bool unknown = GENERATE(false, true);
    auto fixture = std::make_unique<Fixture>();
    fixture->party[0].actor.save().gold = 99999;
    const usize item = fixture->place(unknown ? "UNKNOWN" : "GOLD");
    const auto visitors =
        PartyPickups::collectors(fixture->party, fixture->world, fixture->fixtures.chests());
    const auto offer = fixture->world.placedItems().pickupFor(item, visitors);
    REQUIRE(offer);
    CHECK(offer->collector == 0);
    CHECK(fixture->party[0].actor.player() == 3); // party order, not input slot order
    CHECK(offer->item == item);
    CHECK(offer->subtype == (unknown ? 17 : 1));
    CHECK(offer->amount == (unknown ? 0 : 25));
    CHECK_FALSE(fixture->world.placedItems().pickupFor(item + 1, visitors));
    fixture->checkNoOp();
    if (!unknown) {
        // The second touching player could take this gold, but native collect
        // does not retry after the first refusal. Changing the first does take it.
        CHECK(fixture->party[1].actor.save().gold == 0);
        fixture->party[0].actor.save().gold = 0;
        const auto before = fixture->state();
        CHECK_FALSE(fixture->admitted());
        CHECK(*fixture->state() == *before);
        fixture->collect();
        CHECK(fixture->party[0].actor.save().gold == 25);
        CHECK(fixture->party[1].actor.save().gold == 0);
        CHECK(fixture->world.placedItems().item(item).taken);
    }
}

TEST_CASE("every native refused pickup branch preserves the complete character save",
          "[pickup-admission][pickups]") {
    const s32 branch = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
    CAPTURE(branch);
    auto fixture = std::make_unique<Fixture>();
    auto& save = fixture->party[0].actor.save();
    save.gold = 99999;
    save.progress().inventory.keys = Inventory::kMostKeys;
    save.progress().inventory.potions.assign(Inventory::kMostPotions, 2);
    REQUIRE(save.progress().relics.addRune(0));
    save.progress().relics.gargoylePieces[0] = -1;
    ItemOffer offer{.kind = -1, .amount = 1};
    auto expected = ItemTaking::Outcome::NotCarried;
    switch (branch) {
    case 0:
        offer.kind = static_cast<s32>(ItemKind::Gold);
        expected = ItemTaking::Outcome::GoldFull;
        break;
    case 1:
        offer.kind = static_cast<s32>(ItemKind::Keys);
        expected = ItemTaking::Outcome::KeysFull;
        break;
    case 2:
        offer.kind = static_cast<s32>(ItemKind::Potion);
        expected = ItemTaking::Outcome::PotionsFull;
        break;
    case 3:
        offer.kind = static_cast<s32>(ItemKind::Food);
        expected = ItemTaking::Outcome::HealthFull;
        break;
    case 4:
        offer.kind = static_cast<s32>(ItemKind::Runestone);
        offer.amount = 0;
        expected = ItemTaking::Outcome::AlreadyHeld;
        break;
    case 5: break; // unknown kind
    case 6:
        offer.kind = static_cast<s32>(ItemKind::Legend);
        offer.amount = -1;
        break;
    case 7:
        offer.kind = static_cast<s32>(ItemKind::GargoyleKey);
        offer.amount = -1;
        break;
    case 8:
        offer.kind = static_cast<s32>(ItemKind::GargoyleKey);
        offer.amount = 0; // already-congratulated completion record
        break;
    }
    const auto before = fixture->state();
    const auto taking = takeItem(save, offer, 1.0f, false);
    CHECK(taking.outcome == expected);
    CHECK_FALSE(taking.took());
    CHECK(*fixture->state() == *before);
}

TEST_CASE("capacity pickup refusal requires the exact native help history policy",
          "[pickup-admission][pickups][help]") {
    const auto kind = GENERATE(ItemKind::Keys, ItemKind::Potion, ItemKind::Food);
    const s32 history = GENERATE(0, 1, 2, 3);
    auto fixture = std::make_unique<Fixture>();
    auto& save = fixture->party[0].actor.save();
    save.progress().inventory.keys = Inventory::kMostKeys;
    save.progress().inventory.potions.assign(Inventory::kMostPotions, 2);
    const char* name = "FOOD";
    s32 help = HelpMessages::kHealthFull;
    if (kind == ItemKind::Keys) {
        name = "KEYS";
        help = HelpMessages::kKeysFull;
    } else if (kind == ItemKind::Potion) {
        name = "POTION";
        help = HelpMessages::kPotionsFull;
    }
    if (history > 0) {
        save.helpSeen = {help};
    }
    if (history == 2) {
        fixture->party[1].actor.save().helpSeen = {help};
    } else if (history == 3) {
        fixture->party[1].life = PlayerLife::InTower;
    }
    fixture->place(name);
    const bool expected = history == 2 || history == 3 || (history == 1 && kind == ItemKind::Food);
    const auto before = fixture->state();
    CHECK(fixture->admitted() == expected);
    CHECK(*fixture->state() == *before);
    if (expected) {
        fixture->checkNoOp();
    } else {
        fixture->collect();
        CHECK(fixture->posted == 1);
        CHECK(fixture->hud.help().showing());
        CHECK(fixture->hud.pickups().cards().empty());
    }
}

TEST_CASE("unchanged inventory alone does not admit pickup feedback",
          "[pickup-admission][pickups]") {
    const auto* name = GENERATE("LEGEND", "PIECE", "RUNE", "CRYSTAL", "POWERUP", "SCROLL",
                                "BADFOOD", "KEYS", "POTION");
    CAPTURE(name);
    auto fixture = std::make_unique<Fixture>();
    auto& save = fixture->party[0].actor.save();
    REQUIRE(save.progress().relics.addLegend(1));
    REQUIRE(save.progress().relics.addRune(0));
    save.progress().relics.gargoylePieces[0] = Relics::kGargoyleNeeded[0];
    save.progress().inventory.keys = Inventory::kMostKeys - 1;
    save.progress().inventory.potions.assign(Inventory::kMostPotions - 1, 2);
    for (auto& player : fixture->party) {
        player.actor.save().helpSeen = {HelpMessages::kAlreadyHaveRune};
    }
    fixture->place(name);
    const auto before = fixture->state();
    const auto textures = fixture->device.texturesCreated;
    CHECK_FALSE(fixture->admitted());
    CHECK(*fixture->state() == *before);
    CHECK(fixture->device.texturesCreated == textures);
    if (std::string_view{name} == "PIECE") {
        // A negative, already-congratulated count is genuinely NotCarried.
        save.progress().relics.gargoylePieces[0] = -1;
        fixture->checkNoOp();
    } else if (std::string_view{name} == "RUNE") {
        fixture->collect();
        CHECK(fixture->posted == 1); // Always, not suppressed by recorded history
    }
}

TEST_CASE("a full wallet still takes a native challenge coin",
          "[pickup-admission][pickups][secret]") {
    auto fixture = std::make_unique<Fixture>(true);
    fixture->party[0].actor.save().gold = 99999;
    const usize item = fixture->place("GOLD");
    const auto before = fixture->state();
    CHECK_FALSE(fixture->admitted());
    CHECK(*fixture->state() == *before);
    fixture->collect();
    CHECK(fixture->party[0].actor.save().gold == 99999);
    CHECK(fixture->party[0].actor.save().progress().lifetime.goldFound == 0);
    CHECK(fixture->world.placedItems().item(item).taken);
    CHECK(fixture->external == 1);
    REQUIRE(fixture->hud.pickups().cards().size() == 1);
    CHECK(fixture->hud.pickups().cards()[0].texture == "COINHUD");
}

TEST_CASE("pickup admission revisits first collector after life and departure changes",
          "[pickup-admission][pickups]") {
    const bool departed = GENERATE(false, true);
    auto fixture = std::make_unique<Fixture>();
    fixture->party[0].actor.save().gold = 99999;
    const usize item = fixture->place("GOLD");
    fixture->checkNoOp();
    if (departed) {
        fixture->party[0].departed = true;
    } else {
        fixture->party[0].life = PlayerLife::InTower;
    }
    CHECK_FALSE(fixture->admitted());
    const auto visitors =
        PartyPickups::collectors(fixture->party, fixture->world, fixture->fixtures.chests());
    const auto offer = fixture->world.placedItems().pickupFor(item, visitors);
    REQUIRE(offer);
    CHECK(offer->collector == 1);
    fixture->collect();
    CHECK(fixture->party[1].actor.save().gold == 25);
    CHECK(fixture->world.placedItems().item(item).taken);
}
} // namespace
