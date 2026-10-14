#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/LevelFixturesCheckpoint.h"
#include "game/screens/OrdinaryPickupPhase.h"
#include "game/screens/PartyHelpAdmission.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PartyPickups.h"
#include "game/screens/PlayerRuntimeCheckpoint.h"
#include "game/world/LevelWorldCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = OrdinaryPickupPhase;

std::filesystem::path stage(std::string_view tag) {
    const auto directory = test::sampleLevel(tag);
    std::string trees = R"({"trees":[)";
    for (const auto* name : {"GOLD", "JUNK", "KEYS", "FOOD", "POTION", "UNKNOWN", "RUNE", "CRYSTAL",
                             "POWERUP", "CHEST"}) {
        if (trees.back() != '[') {
            trees += ',';
        }
        trees += std::format(R"({{"name":"{}","nodes":[
            {{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}},
            {{"name":"NULL1","parent":0,"position":[0,1,0]}}],"sequences":[
            {{"name":"CLOSED","frames":2,"rate":30}},
            {{"name":"OPENING","frames":3,"rate":30}},
            {{"name":"OPEN","frames":2,"rate":30}}]}})",
                             name);
    }
    trees += "]}";
    writeTextFile(directory / "animations.json", trees);
    writeTextFile(directory / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4}],
      "bounds":{"min":[-40,-20,-40],"max":[40,20,40]},
      "itemInfos":[
        {"type":1,"subtype":1,"name":"GOLD","radius":1,"height":1,"value":25},
        {"type":1,"subtype":1,"name":"JUNK","radius":1,"height":1,"value":5},
        {"type":1,"subtype":2,"name":"KEYS","radius":1,"height":1,"value":3},
        {"type":1,"subtype":3,"name":"FOOD","radius":1,"height":1,"value":50},
        {"type":1,"subtype":4,"name":"POTION","radius":1,"height":1,"value":1,"properties":2},
        {"type":1,"subtype":17,"name":"UNKNOWN","radius":1,"height":1},
        {"type":1,"subtype":10,"name":"RUNE","radius":1,"height":1,"value":0},
        {"type":1,"subtype":15,"name":"CRYSTAL","radius":1,"height":1,"value":0},
        {"type":1,"subtype":9,"name":"POWERUP","radius":1,"height":1,"properties":2,
         "activeOn":10},
        {"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2,"activeType":0}],
      "itemInstances":[{"info":9,"position":[15,0,0],"params":[0,0,0,0,0,0,0,0,0,0,0,0]}]})");
    writeTextFile(directory / "collision.json", R"({"objects":[{"object":0,
      "normals":[0,1,0,0,1,0],
      "vertices":[-40,0,-40,-40,0,40,40,0,40,-40,0,-40,40,0,40,40,0,-40]}]})");
    return directory;
}

struct Fixture {
    struct State {
        LevelWorldCheckpoint::State world;
        LevelFixturesCheckpoint::State fixtures;
        std::array<PlayerRuntimeCheckpoint::State, 2> party;
        bool operator==(const State&) const = default;
    };
    test::FakeRenderDevice device;
    AudioMixer mixer{48000};
    SoundPlayer sounds{mixer};
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    ClassDataSet classes;
    PartyHud hud;
    PartyPickups pickups;
    std::array<PlayerRuntime, 2> party;
    std::array<std::unique_ptr<PlayerRuntimeCheckpoint>, 2> owners;
    LevelWorldCheckpoint worldState{world};
    LevelFixturesCheckpoint fixtureState{fixtures};
    usize challengeCalls = 0;
    usize newLessons = 0;
    std::vector<PartyMotion::Action> actions;

    explicit Fixture(std::string_view tag, bool figures = false, bool secret = false) {
        LevelRef level{.realm = "TEST", .realmId = 0, .name = "TEST", .directory = "."};
        if (secret) {
            level.realmId = LevelRef::kSecretRealm;
        }
        REQUIRE(world.load(device, stage(tag), level));
        world.setPlayerCount(2);
        fixtures.bind({device, world, weapons, effects, audio});
        fixtures.setPlayerCount(2);
        REQUIRE(fixtures.chests().size() == 1);
        REQUIRE(worldState.bindResources(device));
        REQUIRE(fixtureState.bindResources());
        std::filesystem::path root;
        if (figures) {
            root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
            REQUIRE(classes.load(root / "PDATA"));
        }
        constexpr std::array kSeats{3, 1};
        for (usize index = 0; index < party.size(); ++index) {
            auto& player = party[index];
            CharacterSave save;
            save.autoAttack = false;
            save.progress().health = 500;
            save.helpSeen = {17,
                             8,
                             HelpMessages::kChestNeedsKey,
                             HelpMessages::kKeysFull,
                             HelpMessages::kHealthFull,
                             HelpMessages::kPotionsFull};
            // Native profile loading and help acknowledgements keep this sorted;
            // HelpMessages::wanted deliberately queries it with binary_search.
            std::ranges::sort(save.helpSeen);
            const auto* stats = figures ? classes.stats(save.character) : nullptr;
            player.actor.spawn(kSeats[index], save, stats, {index == 0 ? -3.0f : 3.0f, 0, 0}, 0);
            player.actor.settle(world.collision());
            player.entrySave = player.actor.save();
            if (figures) {
                player.figure = PlayerFigure::load(device, root, save, false);
                REQUIRE(player.figure);
            }
            owners[index] = std::make_unique<PlayerRuntimeCheckpoint>(player, stats);
        }
        for (usize index = 0; index < party.size(); ++index) {
            for (const s32 lesson : party[index].actor.save().helpSeen) {
                CAPTURE(index, lesson);
                REQUIRE(PartyHelpAdmission::suppressed(lesson, index, party));
            }
        }
    }
    ~Fixture() {
        fixtures.clear();
        effects.clear();
        audio.close();
    }
    usize place(std::string_view name, usize player = 0) {
        REQUIRE(world.placeItem(device, name, party[player].actor.position()));
        return world.placedItems().size() - 1;
    }
    std::unique_ptr<State> state() const {
        auto result = std::make_unique<State>();
        auto w = worldState.checkpoint();
        auto f = fixtureState.checkpoint();
        REQUIRE(w);
        REQUIRE(f);
        result->world = std::move(*w);
        result->fixtures = std::move(*f);
        for (usize index = 0; index < owners.size(); ++index) {
            auto player = owners[index]->checkpoint();
            REQUIRE(player);
            result->party[index] = std::move(*player);
        }
        return result;
    }
    void restore(const State& state) {
        auto w = worldState.prepareRestore(state.world);
        auto f = fixtureState.prepareRestore(state.fixtures);
        auto p0 = owners[0]->prepareRestore(state.party[0]);
        auto p1 = owners[1]->prepareRestore(state.party[1]);
        REQUIRE(w);
        REQUIRE(f);
        REQUIRE(p0);
        REQUIRE(p1);
        REQUIRE(worldState.canCommit(*w));
        REQUIRE(fixtureState.canCommit(*f));
        REQUIRE(owners[0]->canCommit(*p0));
        REQUIRE(owners[1]->canCommit(*p1));
        REQUIRE(worldState.commitRestore(std::move(*w)));
        REQUIRE(fixtureState.commitRestore(std::move(*f)));
        REQUIRE(owners[0]->commitRestore(std::move(*p0)));
        REQUIRE(owners[1]->commitRestore(std::move(*p1)));
    }
    void collect() {
        pickups.collect(device, party,
                        {.world = world,
                         .fixtures = fixtures,
                         .hud = hud,
                         .audio = audio,
                         .classes = classes,
                         .sounds = &sounds,
                         .help =
                             [&](s32 id, usize index) {
                                 const bool wanted =
                                     !PartyHelpAdmission::suppressed(id, index, party);
                                 newLessons += wanted ? 1 : 0;
                                 return wanted;
                             },
                         .challengeCoin = [&](usize) { ++challengeCalls; }});
    }
    Phase::Result replay() {
        usize audible = 0;
        const SoundPlayer::Capture capture(&sounds, [&](const auto&) { ++audible; });
        auto result = Phase::advance(party, world, fixtures, classes, worldState);
        CHECK(audible == 0);
        CHECK(hud.pickups().cards().empty());
        CHECK(challengeCalls == 0);
        CHECK(newLessons == 0);
        return result;
    }
    void step(s32 ticks, bool held = false, bool moving = true) {
        std::array<PlayInput, 4> inputs{};
        if (moving) {
            inputs[3].move = {{1, 0}, 0.3f};
            inputs[1].move = {{0, 1}, 0.3f};
        }
        const f32 seconds = static_cast<f32>(ticks) / 60;
        world.capturePresentation();
        fixtures.capturePresentation();
        world.update(seconds);
        PartyMotion::step(
            party, inputs, held, 0, ticks, seconds, world.collision(),
            {.perform = [&](usize, PartyMotion::Action action) { actions.push_back(action); },
             .select = [](usize, const SelectorInput&, s32) {},
             .advanceTurbo = [](usize, s32, f32) {},
             .thrownImpact = [](usize, f32) { FAIL("unexpected thrown-body contact"); }});
    }
    void openChest() {
        party[0].actor.place({15, 0, 0});
        const LevelFixtures::Events events{
            .help = [](s32, usize) { return false; },
            .card = [](s32, std::string_view) { FAIL("unexpected gold-chest event"); },
            .releaseEnemy = [](s32, const Vec3&, s32) { return false; }};
        for (s32 step = 0; step < 60 && fixtures.chests().chest(0).state != Chests::kOpen; ++step) {
            world.update(1.0f / 60);
            fixtures.update(1, 1.0f / 60, party, events);
        }
        REQUIRE(fixtures.chests().chest(0).state == Chests::kOpen);
        REQUIRE(fixtures.chests().chest(0).held >= 0);
        REQUIRE(
            world.placedItems().item(static_cast<usize>(fixtures.chests().chest(0).held)).opener ==
            3);
        party[0].actor.place({-20, 0, 0});
        party[1].actor.place({15, 0, 0});
    }
};

TEST_CASE("ordinary currency collection matches native first-owner inventory and output order",
          "[ordinary-pickup-phase]") {
    const auto* name = GENERATE("GOLD", "JUNK", "KEYS");
    const s32 capacity = GENERATE(0, 1, 2);
    CAPTURE(name, capacity);
    auto native = std::make_unique<Fixture>("currency-native");
    auto replay = std::make_unique<Fixture>("currency-replay");
    for (auto* fixture : {native.get(), replay.get()}) {
        fixture->party[1].actor.place(fixture->party[0].actor.position());
        auto& save = fixture->party[0].actor.save();
        save.gold = capacity == 0 ? 99999 : capacity == 1 ? 99998 : 0;
        save.progress().inventory.keys = capacity == 0   ? Inventory::kMostKeys
                                         : capacity == 1 ? Inventory::kMostKeys - 1
                                                         : 0;
        fixture->place(name);
        fixture->place(name);
    }
    const auto before = replay->state();
    REQUIRE(*before == *native->state());
    const auto created = replay->device.texturesCreated;
    const auto uploaded = replay->device.textureUpdates;
    native->collect();
    REQUIRE(native->newLessons == 0);
    const auto result = replay->replay();
    CAPTURE(result.refusal, result.refusedItem);
    REQUIRE(result.advanced);
    CHECK(*replay->state() == *native->state());
    const auto& cards = native->hud.pickups().cards();
    CHECK(result.collected == cards.size());
    REQUIRE(result.outputs.size() == cards.size() * 2);
    for (usize index = 0; index < cards.size(); ++index) {
        CHECK(result.outputs[index * 2] ==
              Phase::Output{Phase::Output::Kind::Card, index, 3, cards[index].texture});
        CHECK(result.outputs[index * 2 + 1] ==
              Phase::Output{Phase::Output::Kind::NamedSound, index, 3,
                            std::string_view{name} == "KEYS" ? "S_PICKUPKEY" : "S_PICKUPMAGIC"});
    }
    CHECK(replay->party[1].actor.save().gold == 0);
    CHECK(replay->party[1].actor.save().progress().inventory.keys == 0);
    if (capacity == 1 && std::string_view{name} == "KEYS") {
        CHECK(replay->world.placedItems().item(0).value == 2);
        CHECK_FALSE(replay->world.placedItems().item(0).taken);
        CHECK(replay->world.placedItems().item(1).value == 3);
    }
    if (capacity == 1 && std::string_view{name} == "GOLD") {
        CHECK(replay->party[0].actor.save().gold == 99999);
        CHECK(replay->party[0].actor.save().progress().lifetime.goldFound == 25);
    }
    auto accepted = replay->state();
    replay->restore(*before);
    CHECK(replay->replay().outputs == result.outputs);
    CHECK(*replay->state() == *accepted);
    CHECK(replay->device.texturesCreated == created);
    CHECK(replay->device.textureUpdates == uploaded);
}

TEST_CASE("late currency lesson or unsupported pickup refusal rewinds all earlier collections",
          "[ordinary-pickup-phase]") {
    const bool lesson = GENERATE(false, true);
    auto fixture = std::make_unique<Fixture>("currency-refusal");
    fixture->place("JUNK");
    fixture->place(lesson ? "GOLD" : "FOOD");
    if (lesson) {
        fixture->party[1].actor.save().helpSeen.clear();
    } else {
        fixture->party[0].actor.save().progress().health = 300;
    }
    auto before = fixture->state();
    auto result = fixture->replay();
    CHECK_FALSE(result.advanced);
    CHECK(result.refusal == (lesson ? Phase::Refusal::Lesson : Phase::Refusal::Item));
    CHECK(result.refusedItem == std::optional<usize>{1});
    CHECK(result.outputs.empty());
    CHECK(result.collected == 0);
    CHECK(fixture->world.placedItems().item(0).taken);
    CHECK(fixture->party[0].gesture == PlayerDeed::Pick);
    CHECK(fixture->party[0].actor.save().gold > before->party[0].save.gold);
    fixture->restore(*before);
    CHECK(*fixture->state() == *before);
}

TEST_CASE("ordinary collection retains harmless native refusals but excludes special realms",
          "[ordinary-pickup-phase]") {
    const bool secret = GENERATE(false, true);
    auto native = std::make_unique<Fixture>("currency-noop-native", false, secret);
    auto replay = std::make_unique<Fixture>("currency-noop-replay", false, secret);
    for (auto* fixture : {native.get(), replay.get()}) {
        fixture->party[0].actor.save().progress().inventory.potions.assign(Inventory::kMostPotions,
                                                                           2);
        fixture->place("POTION");
        fixture->place("FOOD");
        fixture->place("UNKNOWN");
    }
    const auto before = replay->state();
    const auto result = replay->replay();
    CAPTURE(result.refusal, result.refusedItem);
    CHECK(result.outputs.empty());
    CHECK(result.collected == 0);
    if (secret) {
        CHECK_FALSE(result.advanced);
        CHECK(result.refusal == Phase::Refusal::Domain);
    } else {
        REQUIRE(result.advanced);
        native->collect();
        CHECK(native->newLessons == 0);
        CHECK(*replay->state() == *native->state());
    }
    CHECK(*replay->state() == *before);
}

TEST_CASE("native pickup gestures continue at full motion pace after correction",
          "[ordinary-pickup-phase][assets]") {
    const s32 ticks = GENERATE(1, 2);
    auto native = std::make_unique<Fixture>("currency-pose-native", true);
    auto replay = std::make_unique<Fixture>("currency-pose-replay", true);
    for (auto* fixture : {native.get(), replay.get()}) {
        for (s32 step = 0; step < 120 / ticks; ++step) {
            fixture->step(ticks, false, false);
        }
        // The native gesture is a one-update request, not a queued animation.
        // Start it from Ready, where the native transition accepts it, then
        // exercise movement throughout its authored pickup sequence below.
        REQUIRE(fixture->party[0].figure->animator().action() == PlayerAnimator::Action::Ready);
        fixture->place("JUNK", 0);
        fixture->place("KEYS", 1);
    }
    native->collect();
    REQUIRE(native->newLessons == 0);
    const auto result = replay->replay();
    CAPTURE(result.refusal, result.refusedItem);
    REQUIRE(result.advanced);
    REQUIRE(replay->party[0].gesture == PlayerDeed::Pick);
    REQUIRE(replay->party[1].gesture == PlayerDeed::Pick);
    REQUIRE(*replay->state() == *native->state());
    bool picking = false;
    bool completed = false;
    const Vec3 start = replay->party[0].actor.position();
    for (s32 step = 0; step < 120 / ticks; ++step) {
        CAPTURE(ticks, step);
        const bool held = step < 3;
        native->step(ticks, held);
        replay->step(ticks, held);
        auto expected = native->state();
        CHECK(*replay->state() == *expected);
        if (held) {
            CHECK(replay->party[0].gesture == PlayerDeed::Pick);
        }
        const auto action = replay->party[0].figure->animator().action();
        picking |= action == PlayerAnimator::Action::Pick;
        completed |= picking && action != PlayerAnimator::Action::Pick;
        if (step % 7 == 0) {
            replay->restore(*expected);
        }
    }
    CHECK(picking);
    CHECK(completed);
    CHECK(replay->party[0].actor.position() != start);
    CHECK(replay->actions == native->actions);
}

TEST_CASE("ordinary chest currency keeps historical opener routing and native cleanup",
          "[ordinary-pickup-phase][assets]") {
    auto native = std::make_unique<Fixture>("currency-chest-native", true);
    auto replay = std::make_unique<Fixture>("currency-chest-replay", true);
    native->openChest();
    replay->openChest();
    const auto before = replay->state();
    REQUIRE(*before == *native->state());
    const usize item = static_cast<usize>(replay->fixtures.chests().chest(0).held);
    native->collect();
    REQUIRE(native->newLessons == 0);
    const auto result = replay->replay();
    CAPTURE(result.refusal, result.refusedItem);
    REQUIRE(result.advanced);
    REQUIRE(result.collected == 1);
    const std::vector<Phase::Output> expected{
        {Phase::Output::Kind::CharacterBark, item, 3, "S_WARSTEAL"},
        {Phase::Output::Kind::Card, item, 1, "GOLD"},
        {Phase::Output::Kind::NamedSound, item, 1, "S_PICKUPMAGIC"}};
    CHECK(result.outputs == expected);
    CHECK(*replay->state() == *native->state());
    CHECK(replay->world.placedItems().item(item).taken);
    CHECK(replay->fixtures.chests().chest(0).gone);
    CHECK(replay->party[1].gesture == PlayerDeed::Pick);
    replay->restore(*before);
    CHECK_FALSE(replay->world.placedItems().item(item).taken);
    CHECK_FALSE(replay->fixtures.chests().chest(0).gone);
    CHECK(replay->replay().outputs == expected);
    CHECK(*replay->state() == *native->state());
}

TEST_CASE("ordinary collection rejects unbound or foreign persistent owners before mutation",
          "[ordinary-pickup-phase]") {
    const bool foreign = GENERATE(false, true);
    auto fixture = std::make_unique<Fixture>("currency-binding");
    auto other = std::make_unique<Fixture>("currency-other");
    fixture->place("GOLD");
    auto before = fixture->state();
    auto otherBefore = other->state();
    if (!foreign) {
        fixture->worldState.clearResources();
    }
    const auto result =
        Phase::advance(fixture->party, fixture->world, fixture->fixtures, fixture->classes,
                       foreign ? other->worldState : fixture->worldState);
    CHECK_FALSE(result.advanced);
    CHECK(result.refusal == Phase::Refusal::Binding);
    CHECK(result.outputs.empty());
    CHECK(*other->state() == *otherBefore);
    if (!foreign) {
        REQUIRE(fixture->worldState.bindResources(fixture->device));
    }
    CHECK(*fixture->state() == *before);
}

TEST_CASE("shared currency dispatch preserves native lesson order and secret coin policy",
          "[ordinary-pickup-phase]") {
    using Event = PartyPickups::CurrencyEvent;
    const bool secret = GENERATE(false, true);
    const bool full = GENERATE(false, true);
    const auto* name = GENERATE("GOLD", "KEYS");
    CAPTURE(secret, full, name);
    auto native = std::make_unique<Fixture>("currency-dispatch-native", false, secret);
    auto staged = std::make_unique<Fixture>("currency-dispatch-staged", false, secret);
    for (auto* fixture : {native.get(), staged.get()}) {
        if (full) {
            fixture->party[0].actor.save().gold = 99999;
            fixture->party[0].actor.save().progress().inventory.keys = Inventory::kMostKeys;
        }
        fixture->place(name);
    }
    const auto visitors =
        PartyPickups::collectors(staged->party, staged->world, staged->fixtures.chests());
    std::vector<Event> events;
    const auto taken = staged->worldState.collectItems(visitors, [&](const Pickup& pickup) {
        return PartyPickups::takeCurrency(pickup, staged->party, staged->world, staged->fixtures,
                                          staged->classes, [&](const Event& event) {
                                              events.push_back(event);
                                              return true;
                                          });
    });
    REQUIRE(taken);
    native->collect();
    CHECK(*staged->state() == *native->state());
    const bool gold = std::string_view{name} == "GOLD";
    const bool collected = !full || (gold && secret);
    CHECK(taken->size() == (collected ? 1 : 0));
    std::vector<Event::Kind> kinds;
    for (const auto& event : events) {
        CHECK(event.item == 0);
        CHECK(event.recipient == 0);
        kinds.push_back(event.kind);
    }
    if (!collected) {
        if (gold) {
            CHECK(kinds.empty());
        } else {
            REQUIRE(kinds == std::vector{Event::Kind::Help});
            CHECK(events.front().lesson == HelpMessages::kKeysFull);
        }
        return;
    }
    std::vector<Event::Kind> expected;
    if (gold) {
        expected.push_back(Event::Kind::ChallengeCoin);
    }
    if (!gold || !secret) {
        expected.push_back(Event::Kind::Help);
    }
    expected.push_back(Event::Kind::Card);
    expected.push_back(Event::Kind::NamedSound);
    CHECK(kinds == expected);
    REQUIRE(native->hud.pickups().cards().size() == 1);
    CHECK(events[events.size() - 2].name == native->hud.pickups().cards().front().texture);
    if (secret && gold) {
        CHECK(events[events.size() - 2].name == "COINHUD");
        CHECK(events.back().name == PickupVoices::bonusGold(3, 25));
        CHECK(native->challengeCalls == 1);
        CHECK(staged->party[0].actor.save().progress().lifetime.goldFound == (full ? 0 : 25));
    }
}

TEST_CASE("prepared collection refuses bursts after exact earlier mutations and restores them",
          "[ordinary-pickup-phase]") {
    auto fixture = std::make_unique<Fixture>("currency-burst-refusal");
    fixture->place("JUNK");
    fixture->place("RUNE");
    const auto before = fixture->state();
    const auto created = fixture->device.texturesCreated;
    const auto uploaded = fixture->device.textureUpdates;
    const auto visitors =
        PartyPickups::collectors(fixture->party, fixture->world, fixture->fixtures.chests());
    const auto result = fixture->worldState.collectItems(
        visitors, [](const Pickup&) { return PickupDecision{.left = 0}; });
    CHECK_FALSE(result);
    CHECK(fixture->world.placedItems().item(0).taken);
    CHECK(fixture->world.placedItems().item(1).taken);
    CHECK(fixture->device.texturesCreated == created);
    CHECK(fixture->device.textureUpdates == uploaded);
    fixture->restore(*before);
    CHECK(*fixture->state() == *before);
}

} // namespace
