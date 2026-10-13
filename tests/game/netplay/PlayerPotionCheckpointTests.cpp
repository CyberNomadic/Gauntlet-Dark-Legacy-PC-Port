#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/DamageTypes.h"
#include "game/netplay/PlayerPotionCheckpoint.h"
#include "game/players/Progression.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/PlayScene.h"
#include "game/world/LevelCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;

PlayerPotionState potionState(usize count = 9) {
    PlayerPotionState result;
    for (usize i = 0; i < count; ++i) {
        result.potions.push_back(static_cast<s32>(i % 4 + 1));
    }
    result.powers.fill(20.5f);
    result.bonuses[3] = damage::kOwnColourBonus;
    result.powers[3] *= result.bonuses[3];
    result.damageFlags = damage::kHeal;
    result.ticks = PlayerPotionState::kMaxTicks;
    return result;
}

void writeWord(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize byte = 0; byte < 4; ++byte) {
        bytes[offset + byte] = static_cast<u8>(value >> (byte * 8));
    }
}

TEST_CASE("potion checkpoints preserve ordered native stack and exact colour tuning",
          "[netplay][potion-checkpoint]") {
    for (usize count = 0; count <= static_cast<usize>(Inventory::kMostPotions); ++count) {
        auto state = potionState(count);
        for (const u32 ticks : {0U, 12U, PlayerPotionState::kMaxTicks}) {
            state.textureFrame = ticks == 0 ? std::nullopt : std::optional{27.5f};
            state.ticks = ticks;
            const auto bytes = PlayerPotionCheckpoint::encode(state);
            REQUIRE(bytes);
            CHECK(bytes->size() == PlayerPotionCheckpoint::kHeaderBytes + count * 4);
            const auto restored = PlayerPotionCheckpoint::decode(*bytes);
            REQUIRE(restored);
            CHECK(*restored == state);
            CHECK(PlayerPotionCheckpoint::encode(*restored) == bytes);
        }
    }
    const auto maximum = PlayerPotionCheckpoint::encode(potionState());
    REQUIRE(maximum);
    CHECK(maximum->size() == PlayerPotionCheckpoint::kMaxBytes);
}

TEST_CASE("potion checkpoint decoding rejects malformed counts flags tuning and every truncation",
          "[netplay][potion-checkpoint]") {
    const auto state = potionState();
    const auto encoded = PlayerPotionCheckpoint::encode(state);
    REQUIRE(encoded);
    const auto& bytes = *encoded;
    for (usize size = 0; size < bytes.size(); ++size) {
        CHECK_FALSE(PlayerPotionCheckpoint::decode(std::span{bytes}.first(size)));
    }
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK_FALSE(PlayerPotionCheckpoint::decode(trailing));
    for (const auto [offset, value] :
         std::array<std::pair<usize, u32>, 16>{{{0, PlayerPotionState::kMaxTicks + 1},
                                                {4, 10},
                                                {4, 0xFFFFFFFF},
                                                {8, 1},
                                                {8, damage::kHeal | 1U},
                                                {12, std::bit_cast<u32>(7.9f)},
                                                {12, std::bit_cast<u32>(32.1f)},
                                                {16, std::bit_cast<u32>(21.0f)},
                                                {32, std::bit_cast<u32>(damage::kOwnColourBonus)},
                                                {52, 2},
                                                {56, 1},
                                                {56, 0x80000000},
                                                {56, 0x7FC00000},
                                                {60, 0},
                                                {60, 5},
                                                {60, 0xFFFFFFFF}}}) {
        auto malformed = bytes;
        writeWord(malformed, offset, value);
        CHECK_FALSE(PlayerPotionCheckpoint::decode(malformed));
    }
    for (usize offset = 12; offset < 52; offset += 4) {
        for (const f32 value :
             {std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity(),
              -std::numeric_limits<f32>::infinity()}) {
            auto malformed = bytes;
            writeWord(malformed, offset, std::bit_cast<u32>(value));
            CHECK_FALSE(PlayerPotionCheckpoint::decode(malformed));
        }
    }
    auto malformed = state;
    malformed.potions.push_back(1);
    CHECK_FALSE(PlayerPotionCheckpoint::encode(malformed));
    malformed = state;
    malformed.bonuses[1] = damage::kOwnColourBonus;
    malformed.powers[1] *= malformed.bonuses[1];
    CHECK_FALSE(PlayerPotionCheckpoint::encode(malformed));
    malformed = state;
    malformed.textureFrame = 0;
    const auto zeroClock = PlayerPotionCheckpoint::encode(malformed);
    REQUIRE(zeroClock);
    const auto restoredClock = PlayerPotionCheckpoint::decode(*zeroClock);
    REQUIRE(restoredClock);
    CHECK(restoredClock->textureFrame == std::optional{0.0f});
    for (const f32 badClock : {-1.0f, 1'000'000'128.0f, std::numeric_limits<f32>::quiet_NaN(),
                               std::numeric_limits<f32>::infinity()}) {
        malformed.textureFrame = badClock;
        CHECK_FALSE(PlayerPotionCheckpoint::encode(malformed));
        auto badBytes = *zeroClock;
        writeWord(badBytes, 56, std::bit_cast<u32>(badClock));
        CHECK_FALSE(PlayerPotionCheckpoint::decode(badBytes));
    }
}

struct PotionHostFixture {
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene play;
    explicit PotionHostFixture(std::string_view name) {
        const auto root = test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2")
                              .parent_path()
                              .parent_path()
                              .parent_path();
        const auto directory = test::sampleLevel("potion-certificate");
        writeTextFile(directory / "world.json", R"({"objects":[
            {"name":"FLOOR","position":[0,0,0],"flags":4}],"animations":[]})");
        REQUIRE(catalog.load(root));
        auto level = catalog.byName(name);
        REQUIRE(level);
        level->directory = directory.string();
        REQUIRE(world.load(device, root, *level));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): owned native scene fixture
        auto& collision = const_cast<WorldCollision&>(world.collision());
        collision.build(
            {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
             {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
        GameContext context;
        context.config = &config;
        context.unpackedRoot = root;
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{0};
        CharacterSave save;
        save.progress().health = 1000;
        save.autoAttack = false;
        const std::array party{PartyMember{0, save}};
        REQUIRE(play.open(device, context, world, party, options));
        for (s32 tick = 0; tick < 400; ++tick) {
            REQUIRE(play.update(1.0 / 60, {}) == PlayOutcome::Running);
        }
        REQUIRE(play.movementPredictable());
        REQUIRE_FALSE(player().figure->animator().entering());
    }
    PlayerRuntime& player() {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): owned native scene fixture
        return const_cast<PlayerRuntime&>(play.participants()[0]);
    }
    std::optional<PlayerPotionState> capture() {
        const auto contacts = play.locomotionPreview(0);
        return contacts ? play.potionPreview(0, *contacts) : std::nullopt;
    }
};

TEST_CASE("native potion certificates stop at magic and pace expiry without spending inventory",
          "[netplay][potion-checkpoint][potion-capture][assets]") {
    const auto level =
        GENERATE(std::string_view{"L1"}, std::string_view{"G1"}, std::string_view{"G5"});
    CAPTURE(level);
    PotionHostFixture scene(level);
    auto& inventory = scene.player().actor.save().progress().inventory;
    inventory = {};
    inventory.potions = {1, 4, 2, 3, 1};
    const f32 rate = level == "L1" ? 0 : (level == "G5" ? 3.0f : 1.0f);
    for (const auto kind : {powerup::kMagic, powerup::kSpeed}) {
        for (const f32 duration : {0.001f, 0.04f, 0.1f, -1.0f}) {
            inventory.powerups[0] = {duration, kind, 2.5f, 0, true};
            inventory.powerups[1] = {-1, kind, 1.25f, 0, true};
            const auto before = inventory;
            const auto captured = scene.capture();
            REQUIRE(captured);
            CHECK(captured->potions == inventory.potions);
            auto future = before;
            const auto initial = PowerupEffects::of(future);
            u32 ticks = 0;
            for (; ticks < PlayerLocomotionState::kMaxTicks; ++ticks) {
                future.advance(rate / 60);
                const auto next = PowerupEffects::of(future);
                if (next.magicAdd != initial.magicAdd || next.paceAdd != initial.paceAdd ||
                    next.special != initial.special) {
                    break;
                }
            }
            CHECK(captured->ticks == ticks);
            CHECK(captured->bonuses[0] == 1);
            CHECK(captured->damageFlags ==
                  damage::magicHeal(experienceLevel(scene.player().actor.save().experience())));
            for (usize colour = 1; colour < captured->powers.size(); ++colour) {
                CHECK(captured->powers[colour] == captured->powers[0] * captured->bonuses[colour]);
            }
            CHECK(inventory == before);
        }
    }
    inventory.powerups = {};
    REQUIRE(scene.capture());
    inventory.potions.insert(inventory.potions.begin(), 0);
    CHECK_FALSE(scene.capture()); // even a buried shared-kind bottle cannot become locally owned
    inventory.potions = {2};
    inventory.powerups[0] = {-1, powerup::kArmor, 0, powerup::kFireShield, true};
    CHECK_FALSE(scene.capture());
    inventory.powerups[0] = {-1, powerup::kSpecial, 0, powerup::kPojo, true};
    CHECK_FALSE(scene.capture());
    inventory.powerups[0] = {1, powerup::kMagic, std::numeric_limits<f32>::quiet_NaN(), 0, true};
    CHECK_FALSE(scene.capture());
    inventory.powerups = {};
    scene.player().reaction = PlayerDeed::Flinch;
    CHECK_FALSE(scene.capture());
    scene.player().reaction = PlayerDeed::None;
    scene.player().combo.partner = 1;
    CHECK_FALSE(scene.capture());
}
TEST_CASE("native potion capture retains active use and held throw through empty recovery",
          "[netplay][potion-checkpoint][potion-capture][assets]") {
    const bool thrown = GENERATE(false, true);
    CAPTURE(thrown);
    PotionHostFixture scene("G1");
    auto& inventory = scene.player().actor.save().progress().inventory;
    inventory = {};
    CHECK_FALSE(scene.capture());
    inventory.potions = {3};
    REQUIRE(scene.capture());
    bool active = false;
    bool emptyRecovery = false;
    bool nativeBottle = false;
    u64 releaseGeneration = 0;
    for (u32 tick = 0; tick < 180; ++tick) {
        PlayScene::Inputs input;
        input[0].usePotion = !thrown && tick == 0;
        input[0].throwPotion = thrown && tick < 20;
        REQUIRE(scene.play.update(1.0 / 60, input) == PlayOutcome::Running);
        const auto& animator = scene.player().figure->animator();
        if (animator.potionThrown()) {
            releaseGeneration = animator.player().generation();
        }
        for (usize index = 0; index < scene.play.missiles().count(); ++index) {
            const auto& missile = scene.play.missiles().missile(index);
            if (missile.potion == 3) {
                nativeBottle = true;
                REQUIRE(missile.potionRelease);
                CHECK(missile.potionRelease->generation == releaseGeneration);
                CHECK(missile.effect != 0);
            }
        }
        if (animator.conjuring()) {
            active = true;
            const auto captured = scene.capture();
            REQUIRE(captured);
            CHECK(captured->potions == inventory.potions);
            if (nativeBottle) {
                CHECK(captured->textureFrame.has_value());
            }
            emptyRecovery = emptyRecovery || inventory.potions.empty();
        }
    }
    CHECK(active);
    CHECK(emptyRecovery);
    CHECK(nativeBottle == thrown);
    CHECK(inventory.potions.empty());
    CHECK_FALSE(scene.capture());
}
} // namespace
