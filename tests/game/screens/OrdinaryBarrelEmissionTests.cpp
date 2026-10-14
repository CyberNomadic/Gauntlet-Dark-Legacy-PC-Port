#include <array>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/LevelFixturesCheckpoint.h"
#include "game/screens/OrdinaryBarrelEmission.h"
#include "game/screens/PartyHelpAdmission.h"
#include "game/screens/PartyPickups.h"
#include "game/screens/PlayerRuntime.h"
#include "game/world/LevelWorldCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Emission = OrdinaryBarrelEmission;
using Kind = BreakableStrike::Kind;

std::filesystem::path stage(std::string_view tag, s32 contents, s32 count, bool moving) {
    const auto directory = test::sampleLevel(tag);
    writeTextFile(directory / "animations.json", R"({
      "particles":[{"preset":2,"enables":16929,"texture":"STONE",
        "particleLife":[0.2,0],"rate":[30,30,30,30]}],
      "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":2}],
      "trees":[
       {"name":"BARREL","nodes":[
         {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"WHOLE","frames":2,"rate":30},
          {"name":"BREAK","frames":30,"rate":30},
          {"name":"BROKEN","frames":0}]},
       {"name":"COIN","nodes":[
         {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
         {"name":"EMPTY","parent":0,"position":[0,1,0]},
         {"name":"SPARK","particle":0,"parent":1,"position":[0,1,0]}],
        "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
       {"name":"KEY","nodes":[
         {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
         {"name":"EMPTY","parent":0,"position":[0,1,0]},
         {"name":"SPARK","particle":0,"parent":1,"position":[0,1,0]}],
        "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]}]})");
    const auto movement = moving ? R"([{"object":0,"frames":60,"state":257,"start":0,
                                   "track":{"flags":32,"frames":[0,59],"values":[0,3]}}])"
                                 : "[]";
    writeTextFile(directory / "world.json",
                  std::format(R"({{
          "objects":[{{"name":"FLOOR","position":[0,0,0],"flags":{}}}],
          "bounds":{{"min":[-80,-20,-80],"max":[80,20,80]}},
          "animations":{},
          "itemInfos":[
            {{"type":2,"subtype":43,"name":"BARREL","radius":1,"height":3,"hitPoints":3,"armor":1}},
            {{"type":10,"subtype":43,"name":"BARREL","radius":1,"height":3,"hitPoints":3,"armor":1}},
            {{"type":10,"subtype":45,"name":"BARREL","radius":1,"height":3,"hitPoints":3,"armor":1}},
            {{"type":10,"subtype":44,"name":"BARREL","radius":1,"height":3,"hitPoints":3,"armor":1}},
            {{"type":-1,"choices":[5,6]}},
            {{"type":1,"subtype":1,"name":"COIN","radius":1,"height":1,"value":25}},
            {{"type":1,"subtype":2,"name":"KEY","radius":1,"height":1,"value":3}},
            {{"type":1,"subtype":1,"name":"MISSING","radius":1,"height":1,"value":8}},
            {{"type":4,"name":"DEATH","radius":1,"height":2}},
            {{"type":-1,"choices":[-1]}}],
          "itemInstances":[
            {{"info":0,"position":[0,0,0],"params":[{},0,0,0,{},0,0,0,0,0,0,0]}},
            {{"info":1,"position":[10,0,0]}},
            {{"info":2,"position":[20,0,0]}},
            {{"info":3,"position":[30,0,0]}},
            {{"info":5,"position":[60,0,60]}}]}})",
                              moving ? 4100 : 4, movement, contents, count));
    writeTextFile(directory / "collision.json", R"({"objects":[{"object":0,
      "normals":[0,1,0,0,1,0],
      "vertices":[-80,0,-80,-80,0,80,80,0,80,-80,0,-80,80,0,80,80,0,-80]}]})");
    return directory;
}

struct Fixture {
    struct State {
        LevelWorldCheckpoint::State world;
        LevelFixturesCheckpoint::State fixtures;
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
    LevelWorldCheckpoint worldState{world};
    LevelFixturesCheckpoint fixtureState{fixtures};
    std::array<PlayerRuntime, 2> party;
    ClassDataSet classes;
    std::vector<usize> lessons;
    usize enemyReleases = 0;
    bool releaseEnemy = false;

    explicit Fixture(std::string_view tag, s32 contents = 4, s32 count = 0, bool moving = false) {
        const auto directory = stage(tag, contents, count, moving);
        REQUIRE(world.load(device, directory, {.name = "GTEST", .directory = "."}));
        world.setPlayerCount(2);
        audio.open(directory, &sounds, world.audio(), 'G');
        fixtures.bind({device, world, weapons, effects, audio});
        fixtures.setPlayerCount(2);
        REQUIRE(fixtures.barrels().size() == 4);
        REQUIRE(fixtures.barrels().barrel(0).figure.hasFigure());
        REQUIRE(world.placedItems().size() == 1);
        REQUIRE(worldState.bindResources(device));
        REQUIRE(fixtureState.bindResources());
        constexpr std::array kSeats{3, 1};
        for (usize index = 0; index < party.size(); ++index) {
            CharacterSave save;
            save.helpSeen = {HelpMessages::kBarrelsHold};
            party[index].actor.spawn(kSeats[index], save, nullptr,
                                     {index == 0 ? -25.0f : 25.0f, 0, -25}, 0);
        }
    }
    ~Fixture() {
        fixtures.clear();
        effects.clear();
        audio.close();
    }
    LevelFixtures::Events events() {
        return {.help =
                    [&](s32 id, usize index) {
                        CHECK(id == HelpMessages::kBarrelsHold);
                        lessons.push_back(index);
                        return !PartyHelpAdmission::suppressed(id, index, party);
                    },
                .releaseEnemy =
                    [&](s32, const Vec3&, s32) {
                        ++enemyReleases;
                        return releaseEnemy;
                    }};
    }
    void step(s32 ticks) {
        world.capturePresentation();
        fixtures.capturePresentation();
        world.update(static_cast<f32>(ticks) / 60, false);
        fixtures.update(ticks, static_cast<f32>(ticks) / 60, party, events());
    }
    std::unique_ptr<State> state() const {
        auto w = worldState.checkpoint();
        auto f = fixtureState.checkpoint();
        REQUIRE(w);
        REQUIRE(f);
        return std::make_unique<State>(std::move(*w), std::move(*f));
    }
    void restore(const State& value) {
        auto w = worldState.prepareRestore(value.world);
        auto f = fixtureState.prepareRestore(value.fixtures);
        REQUIRE(w);
        REQUIRE(f);
        REQUIRE(worldState.canCommit(*w));
        REQUIRE(fixtureState.canCommit(*f));
        REQUIRE(worldState.commitRestore(std::move(*w)));
        REQUIRE(fixtureState.commitRestore(std::move(*f)));
    }
    Emission::Result replay(usize barrel = 0, f32 power = 100, s32 byPlayer = 1,
                            LevelWorldCheckpoint* borrowed = nullptr) {
        Emission::Result result;
        usize calls = 0;
        usize soundsPlayed = 0;
        const SoundPlayer::Capture capture(&sounds, [&](const auto&) { ++soundsPlayed; });
        const bool accepted = fixtures.strikeBarrel(
            barrel, power, byPlayer, party, events(),
            [&](const BreakableStrike& strike, std::string_view sound) {
                ++calls;
                result = Emission::advance(strike, sound, byPlayer, party, fixtures,
                                           borrowed ? *borrowed : worldState);
                return result.accepted;
            });
        REQUIRE(calls == 1);
        CHECK(accepted == result.accepted);
        CHECK(soundsPlayed == 0);
        CHECK(lessons.empty());
        CHECK(enemyReleases == 0);
        return result;
    }
};

TEST_CASE(
    "ordinary holding barrels birth exact native prepared pickups and continue after correction",
    "[ordinary-barrel-emission][prepared-pickup-birth]") {
    const s32 ticks = GENERATE(1, 2);
    const s32 amount = GENERATE(0, 7);
    const bool moving = GENERATE(false, true);
    CAPTURE(ticks, amount, moving);
    auto native = std::make_unique<Fixture>("barrel-birth-native", 4, amount, moving);
    auto replay = std::make_unique<Fixture>("barrel-birth-replay", 4, amount, moving);
    for (s32 step = 0; step < 5; ++step) {
        native->step(ticks);
        replay->step(ticks);
    }
    auto before = replay->state();
    REQUIRE(*before == *native->state());
    auto undoWorld = replay->worldState.prepareRestore(before->world);
    auto undoFixtures = replay->fixtureState.prepareRestore(before->fixtures);
    REQUIRE(undoWorld);
    REQUIRE(undoFixtures);
    const auto textures = replay->device.texturesCreated;
    const auto uploads = replay->device.textureUpdates;
    native->fixtures.strikeBarrel(0, 100, 1, native->party, native->events());
    const auto result = replay->replay();
    REQUIRE(result.accepted);
    REQUIRE(result.pickup == std::optional<usize>{1});
    REQUIRE(result.cue);
    CHECK(result.cue->name == "S_BARREL_WOODG");
    REQUIRE(native->lessons == std::vector<usize>{1}); // slot 1 is party index 1, not slot 3
    CHECK(native->enemyReleases == 1);                 // native release-first fallback still runs
    REQUIRE(replay->world.placedItems().size() == 2);
    const auto& born = replay->world.placedItems().item(*result.pickup);
    REQUIRE((born.info == 5 || born.info == 6));
    const s32 bornRecord = born.info;
    const s32 recordValue = born.info == 5 ? 25 : 3;
    CHECK(born.value == (amount > 0 ? amount : recordValue));
    CHECK(born.noGrabSeconds == 0);
    CHECK_FALSE(born.thrown);
    CHECK_FALSE(born.contained);
    CHECK_FALSE(born.carried);
    CHECK(born.floor.has_value() == moving);
    const auto birthParticles = born.particles.checkpoint();
    REQUIRE(birthParticles.field.entries.size() == 1);
    CHECK(birthParticles.field.entries.front().emitter.random == 3);
    CHECK(replay->state()->fixtures.barrels.seed != before->fixtures.barrels.seed);
    CHECK(*replay->state() == *native->state());
    CHECK_FALSE(
        PartyPickups::touchesPickup(replay->party, replay->world, replay->fixtures.chests()));

    bool liveParticles = false;
    for (s32 step = 0; step < 120 / ticks; ++step) {
        CAPTURE(step);
        native->step(ticks);
        replay->step(ticks);
        auto expected = native->state();
        CHECK(*replay->state() == *expected);
        liveParticles |= !expected->world.pickups.items[1]
                              .particles.field.entries.front()
                              .emitter.particles.empty();
        if (step % 17 == 0) {
            // Use an independently allocated native owner's image, not a
            // replay-produced approximation, and continue its exact clocks.
            replay->restore(*expected);
        }
    }
    CHECK(liveParticles);
    CHECK(replay->device.texturesCreated == textures);
    CHECK(replay->device.textureUpdates == uploads);

    // A sibling restore above deliberately invalidated the old plans. Fresh
    // preparations can still rewind the entire accepted interval to its birth.
    CHECK_FALSE(replay->worldState.canCommit(*undoWorld));
    CHECK_FALSE(replay->fixtureState.canCommit(*undoFixtures));
    replay->restore(*before);
    const auto repeated = replay->replay();
    REQUIRE(repeated.accepted);
    CHECK(repeated.pickup == result.pickup);
    CHECK(replay->world.placedItems().item(*repeated.pickup).info == bornRecord);
}

TEST_CASE("barrel emission refuses unprepared births and late lessons without leaking outputs",
          "[ordinary-barrel-emission]") {
    enum class Failure { MissingArt, Death, Lesson, ClearedCatalog, ForeignOwner };
    const auto failure = GENERATE(Failure::MissingArt, Failure::Death, Failure::Lesson,
                                  Failure::ClearedCatalog, Failure::ForeignOwner);
    CAPTURE(failure);
    const s32 record = failure == Failure::MissingArt ? 7 : failure == Failure::Death ? 8 : 4;
    auto fixture = std::make_unique<Fixture>("barrel-birth-refusal", record);
    auto foreign = std::make_unique<Fixture>("barrel-birth-foreign");
    if (failure == Failure::Lesson) {
        fixture->party[0].actor.save().helpSeen.clear(); // OnceForAll includes the other seat
    }
    auto before = fixture->state();
    const auto foreignBefore = foreign->state();
    auto w = fixture->worldState.prepareRestore(before->world);
    auto f = fixture->fixtureState.prepareRestore(before->fixtures);
    REQUIRE(w);
    REQUIRE(f);
    if (failure == Failure::ClearedCatalog) {
        fixture->worldState.clearResources();
    }
    const auto created = fixture->device.texturesCreated;
    const auto result = fixture->replay(
        0, 100, 1, failure == Failure::ForeignOwner ? &foreign->worldState : nullptr);
    CHECK_FALSE(result.accepted);
    CHECK_FALSE(result.cue);
    CHECK_FALSE(result.pickup);
    const auto expected = failure == Failure::MissingArt ? Emission::Refusal::Birth
                          : failure == Failure::Death    ? Emission::Refusal::Record
                          : failure == Failure::Lesson   ? Emission::Refusal::Lesson
                                                         : Emission::Refusal::Binding;
    CHECK(result.refusal == expected);
    CHECK(fixture->world.placedItems().size() == (failure == Failure::Lesson ? 2 : 1));
    CHECK(fixture->device.texturesCreated == created);
    CHECK(*foreign->state() == *foreignBefore);
    if (failure == Failure::ClearedCatalog) {
        CHECK_FALSE(fixture->worldState.canCommit(*w));
        REQUIRE(fixture->worldState.bindResources(fixture->device));
        fixture->restore(*before);
    } else {
        REQUIRE(fixture->worldState.canCommit(*w));
        REQUIRE(fixture->fixtureState.canCommit(*f));
        REQUIRE(fixture->worldState.commitRestore(std::move(*w)));
        REQUIRE(fixture->fixtureState.commitRestore(std::move(*f)));
    }
    CHECK(*fixture->state() == *before);
}

TEST_CASE("accepted barrel births do not suppress native same-tick pickup collection",
          "[ordinary-barrel-emission]") {
    auto fixture = std::make_unique<Fixture>("barrel-birth-collection", 5);
    fixture->party[0].actor.place({0, 0, 0});
    auto before = fixture->state();
    auto w = fixture->worldState.prepareRestore(before->world);
    auto f = fixture->fixtureState.prepareRestore(before->fixtures);
    REQUIRE(w);
    REQUIRE(f);
    const auto result = fixture->replay();
    REQUIRE(result.accepted);
    REQUIRE(result.pickup);
    CHECK(PartyPickups::touchesPickup(fixture->party, fixture->world, fixture->fixtures.chests()));
    CHECK_FALSE(PartyPickups::collectionUnchanged(fixture->party, fixture->world,
                                                  fixture->fixtures.chests(), fixture->classes));
    CHECK(fixture->world.placedItems().item(*result.pickup).noGrabSeconds == 0);
    REQUIRE(fixture->worldState.canCommit(*w));
    REQUIRE(fixture->fixtureState.canCommit(*f));
    REQUIRE(fixture->worldState.commitRestore(std::move(*w)));
    REQUIRE(fixture->fixtureState.commitRestore(std::move(*f)));
    CHECK(*fixture->state() == *before);
}

TEST_CASE("ordinary barrel emission retains no-drop and nonbreaking outcomes without hazards",
          "[ordinary-barrel-emission]") {
    const usize barrel = GENERATE(usize{0}, usize{1}, usize{2}, usize{3});
    const bool broken = GENERATE(false, true);
    CAPTURE(barrel, broken);
    auto native = std::make_unique<Fixture>("barrel-empty-native", 9);
    auto replay = std::make_unique<Fixture>("barrel-empty-replay", 9);
    const f32 power = broken ? 100.0f : 1.0f;
    const auto result = replay->replay(barrel, power);
    if (broken && barrel >= 2) {
        CHECK_FALSE(result.accepted);
        CHECK(result.refusal == Emission::Refusal::Kind);
        CHECK_FALSE(result.cue);
        return;
    }
    native->fixtures.strikeBarrel(barrel, power, 1, native->party, native->events());
    REQUIRE(result.accepted);
    REQUIRE(result.cue);
    CHECK_FALSE(result.pickup);
    CHECK(result.cue->name == (broken ? "S_BARREL_WOODG" : "S_WEAPONHITWOOD"));
    CHECK(native->lessons.empty());
    CHECK(native->enemyReleases == 0);
    CHECK(*replay->state() == *native->state());
}

TEST_CASE("shared barrel release preserves enemy-first fallback and exact help routing",
          "[ordinary-barrel-emission]") {
    enum class Outcome { Enemy, EmptyPickup, Pickup, RefusedPickup };
    const auto outcome =
        GENERATE(Outcome::Enemy, Outcome::EmptyPickup, Outcome::Pickup, Outcome::RefusedPickup);
    const s32 byPlayer = GENERATE(1, 3, 2);
    CAPTURE(outcome, byPlayer);
    auto fixture = std::make_unique<Fixture>("barrel-release-order", 5, 7);
    const Vec3 releasePosition = fixture->fixtures.barrels().barrel(0).figure.position();
    REQUIRE(releasePosition == Vec3{0, ItemFigure::kFloorLift, 0});
    std::vector<std::string> order;
    std::vector<usize> helped;
    const LevelFixtures::Events events{.help =
                                           [&](s32 lesson, usize index) {
                                               CHECK(lesson == HelpMessages::kBarrelsHold);
                                               order.emplace_back("help");
                                               helped.push_back(index);
                                               return false;
                                           },
                                       .releaseEnemy =
                                           [&](s32 record, const Vec3& position, s32 count) {
                                               CHECK(record == 5);
                                               CHECK(position == releasePosition);
                                               CHECK(count == 7);
                                               CHECK(fixture->fixtures.barrels().barrel(0).health ==
                                                     0);
                                               order.emplace_back("enemy");
                                               return outcome == Outcome::Enemy;
                                           }};
    const bool accepted = fixture->fixtures.strikeBarrel(
        0, 100, byPlayer, fixture->party, events,
        [&](const BreakableStrike& strike, std::string_view) {
            return fixture->fixtures.releaseBarrelContents(
                strike, byPlayer, fixture->party, events,
                [&](s32 record, const Vec3& position, s32 count) -> std::optional<bool> {
                    CHECK(record == strike.contents);
                    CHECK(position == strike.position);
                    CHECK(count == strike.count);
                    order.emplace_back("pickup");
                    if (outcome == Outcome::RefusedPickup) {
                        return std::nullopt;
                    }
                    return outcome == Outcome::Pickup;
                });
        });
    CHECK(accepted == (outcome != Outcome::RefusedPickup));
    std::vector<std::string> expected{"enemy"};
    if (outcome != Outcome::Enemy) {
        expected.emplace_back("pickup");
    }
    if ((outcome == Outcome::Enemy || outcome == Outcome::Pickup) && byPlayer != 2) {
        expected.emplace_back("help");
        CHECK(helped == std::vector<usize>{byPlayer == 3 ? usize{0} : usize{1}});
    } else {
        CHECK(helped.empty());
    }
    CHECK(order == expected);
    CHECK(fixture->world.placedItems().size() == 1);
}

} // namespace
