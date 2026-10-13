#include <algorithm>
#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/CombatSnapshot.h"
#include "game/screens/PlayScene.h"
#include "game/world/LevelCatalog.h"
#include "game/world/PowerupCompanion.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Scene {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene play;
    explicit Scene(std::string_view levelName = {}) {
        if (!levelName.empty()) {
            REQUIRE(catalog.load(root));
            const auto level = catalog.byName(levelName);
            REQUIRE(level);
            REQUIRE(world.load(device, root, *level));
        }
        GameContext context;
        context.config = &config;
        context.levels = levelName.empty() ? nullptr : &catalog;
        context.unpackedRoot = root;
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{3, 2, -20};
        if (!levelName.empty()) {
            REQUIRE(world.startPoint(0));
            options.position = world.startPoint(0)->position;
        }
        CharacterSave save;
        save.progress().health = 1000;
        const std::array party{PartyMember{0, save}};
        REQUIRE(play.open(device, context, world, party, options));
    }
    PlayerRuntime& player() {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
        return const_cast<PlayerRuntime&>(play.participants()[0]);
    }
};

TEST_CASE("shield certificates preserve exact masks pace and native lifetime boundaries",
          "[netplay][shield-prediction][assets]") {
    const auto level = GENERATE(std::string_view{}, std::string_view{"G1"}, std::string_view{"G5"});
    Scene scene(level);
    auto& actor = scene.player().actor;
    auto& inventory = actor.save().progress().inventory;
    actor.setPaceBonus(0);
    const f32 base = actor.speed();
    const f32 rate = level.empty() ? 0.0f : (level == "G5" ? 3.0f : 1.0f);
    const auto verify = [&] {
        const auto before = inventory;
        const auto worn = PowerupEffects::of(before);
        const auto certificate = scene.play.weaponModifiers(0);
        REQUIRE(certificate);
        REQUIRE(certificate->valid());
        CHECK(certificate->shields == (worn.armor & powerup::kShields));
        CHECK(certificate->flags == worn.weapon);
        CHECK(certificate->speed == ((worn.special & powerup::kSpeedBoost) != 0));
        auto future = before;
        u32 ticks = 0;
        for (; ticks < PlayerWeaponModifiers::kMaxTicks; ++ticks) {
            future.advance((1.0f / 60) * rate);
            const auto next = PowerupEffects::of(future);
            if ((next.armor & powerup::kShields) != certificate->shields ||
                next.weapon != worn.weapon || next.paceAdd != worn.paceAdd ||
                ((next.special & powerup::kSpeedBoost) != 0) != certificate->speed) {
                break;
            }
        }
        CHECK(certificate->ticks == ticks);
        CHECK(scene.play.predictionSpeed(0) ==
              std::clamp(base + worn.paceAdd, PlayerActor::kMinSpeed, PlayerActor::kMaxSpeed));
        CHECK_FALSE(scene.play.itemsPredictable(0));
        CHECK_FALSE(scene.play.rangedItemsPredictable(0));
        CHECK_FALSE(scene.play.weaponPreview(0));
        CHECK_FALSE(scene.play.meleePreview(0));
        CHECK_FALSE(scene.play.turboGeometry(0));
        CHECK(actor.speed() == base);
        CHECK(inventory == before);
        return certificate.value();
    };
    for (const u32 shield : {powerup::kReflectShield, powerup::kFireShield,
                             powerup::kLightningShield, powerup::kShields}) {
        for (const f32 duration : {0.001f, 0.04f, 0.8f, -1.0f}) {
            CAPTURE(level, shield, duration);
            inventory = {};
            inventory.powerups[0] = {duration, powerup::kArmor, 0, shield, true};
            const auto certificate = verify();
            CHECK(certificate.fireSeconds == ((shield & powerup::kFireShield) != 0 ? duration : 0));
            const f32 expectedStep =
                (shield & powerup::kFireShield) != 0 && level == "G1" ? 1.0f / 60 : 0;
            // G5 starts with its boss asleep: its fade clock is held, although
            // the expiry certificate conservatively anticipates awakening.
            CHECK(certificate.inventoryStep == expectedStep);
            const f32 fade = certificate.fireSeconds < 0
                                 ? 1.0f
                                 : std::clamp(certificate.fireSeconds, 0.0f, 1.0f);
            if ((shield & powerup::kFireShield) != 0) {
                CHECK(fade == PowerupCompanion::fadeOf(inventory));
            }
            inventory.powerups[1] = {0.2f, powerup::kSpeed, 2, 0, true};
            inventory.powerups[2] = {-1, powerup::kWeapon, 0, PlayerMissiles::kPlainModifiers | 4U,
                                     true};
            inventory.powerups[3] = {-1, powerup::kArmor, 0,
                                     powerup::kInvulnerable | powerup::kGoldInvulnerable, true};
            verify();
        }
    }
    // A duplicate may expire without changing the effective shield or its fade.
    inventory = {};
    inventory.powerups[0] = {0.001f, powerup::kArmor, 0, powerup::kFireShield, true};
    inventory.powerups[1] = {-1, powerup::kArmor, 0, powerup::kFireShield, true};
    CHECK(verify().ticks == PlayerWeaponModifiers::kMaxTicks);
    CHECK(verify().fireSeconds == -1);
    inventory.powerups[1].strength = 0.8f;
    CHECK(verify().fireSeconds == 0.8f);
    // Changing one overlapping shield changes the native arm choice even when
    // shielded animation remains active; the prefix stops at that boundary.
    inventory.powerups[1].flags = powerup::kLightningShield;
    const auto overlap = verify();
    if (!level.empty()) {
        CHECK(overlap.ticks == 0);
    }
}

TEST_CASE("shield walking refuses automatic Reflect melee and unsupported equipment",
          "[netplay][shield-prediction][assets]") {
    Scene scene;
    auto& actor = scene.player().actor;
    auto& inventory = actor.save().progress().inventory;
    inventory = {};
    inventory.powerups[0] = {-1, powerup::kArmor, 0, powerup::kReflectShield, true};
    actor.save().autoAttack = true;
    scene.config.combat.autoMelee = false;
    CHECK(scene.play.predictionSpeed(0));
    scene.config.combat.autoMelee = true;
    CHECK_FALSE(scene.play.predictionSpeed(0));
    actor.save().autoAttack = false;
    CHECK(scene.play.predictionSpeed(0));
    actor.save().autoAttack = true;
    for (const u32 contact : {powerup::kFireShield, powerup::kLightningShield}) {
        inventory.powerups[0].flags = powerup::kReflectShield | contact;
        CHECK(scene.play.predictionSpeed(0)); // native contact shields suppress auto-melee
    }
    scene.config.combat.autoMelee = false;
    inventory.powerups[0].flags = powerup::kFireShield;
    for (const auto [kind, flags] :
         std::array<std::pair<s32, u32>, 5>{{{powerup::kArmor, 0x80000U},
                                             {powerup::kArmor, 0x80000000U},
                                             {powerup::kSpecial, powerup::kLeftGauntlet},
                                             {powerup::kSpecial, powerup::kGrowth},
                                             {powerup::kWeapon, powerup::kThunderHammer}}}) {
        inventory.powerups[1] = {-1, kind, 0, flags, true};
        CHECK_FALSE(scene.play.predictionSpeed(0));
        inventory.powerups[1].on = false;
        CHECK(scene.play.predictionSpeed(0));
    }
    inventory.powerups[1] = {};
    for (const f32 invalid :
         {std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity(),
          -std::numeric_limits<f32>::infinity()}) {
        inventory.powerups[0].strength = invalid;
        CHECK_FALSE(scene.play.weaponModifiers(0));
        CHECK_FALSE(scene.play.predictionSpeed(0));
        inventory.powerups[0].on = false;
        CHECK(scene.play.predictionSpeed(0));
        inventory.powerups[0].on = true;
    }
    CHECK_FALSE(scene.play.weaponModifiers(-1));
    CHECK_FALSE(scene.play.predictionSpeed(-1));
}
} // namespace
