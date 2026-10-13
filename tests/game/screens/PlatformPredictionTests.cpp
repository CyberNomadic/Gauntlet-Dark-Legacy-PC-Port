#include <array>
#include <format>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/PlatformCheckpoint.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/PlayScene.h"
#include "game/world/LevelCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;

enum class Pad { None, Pressure, Forced };

/** Native PlayScene/WorldAnimator/PartyMotion, with a long-running authored
 * lift and a distant sibling deck below the same moving parent. */
struct PlatformScene {
    std::filesystem::path assets =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    std::filesystem::path directory = test::sampleLevel("platform-admission");
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene play;

    explicit PlatformScene(Pad pad = Pad::None, bool second = false) {
        const std::string triggers = pad == Pad::None ? "" : R"(
            "itemInstances":[{"info":1,"position":[0,5,0],
                "params":[0,0,8,0,4,255,5,0,0,0,0,0]}],)";
        writeTextFile(directory / "world.json", std::format(R"({{
            "objects":[
                {{"name":"GROUP","position":[0,5,0],"flags":4096,"child":1,"next":3}},
                {{"name":"FLOOR","position":[0,0,0],"flags":4100,"next":2}},
                {{"name":"FLOOR","position":[40,0,0],"flags":4100}},
                {{"name":"FLOOR","position":[0,-10,0],"flags":4}}],
            "itemInfos":[{{"type":1,"subtype":3,"name":"APPLE","radius":1,"height":1}},
                          {{"type":5,"subtype":24,"name":"INVISIBLEPAD","radius":2,"height":1}}],
            {} "animations":[{{"object":0,"frames":601,"state":257,"start":0,
                "track":{{"flags":32,"frames":[0,600],"values":[0,20]}}}}]}})",
                                                            triggers));
        REQUIRE(catalog.load(assets));
        auto level = catalog.byName("G1");
        REQUIRE(level);
        level->directory = directory.string();
        REQUIRE(world.load(device, assets, *level));
        std::vector<CollisionTriangle> triangles;
        for (const s32 object : {1, 2, 3}) {
            const f32 width = object == 3 ? 100.0f : 10.0f;
            const f32 y = object == 3 ? -10.0f : 0.0f;
            const u32 flags = object == 3 ? 4U : 4100U;
            triangles.push_back(
                {{0, 1, 0},
                 {Vec3{-width, y, -width}, Vec3{width, y, width}, Vec3{width, y, -width}},
                 object,
                 flags});
            triangles.push_back(
                {{0, 1, 0},
                 {Vec3{-width, y, -width}, Vec3{-width, y, width}, Vec3{width, y, width}},
                 object,
                 flags});
        }
        // The fixture owns the world; replace only its test floor triangles.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        auto& collision = const_cast<WorldCollision&>(world.collision());
        collision.build(std::move(triangles));
        collision.setMovingObjects(std::array<s32, 2>{1, 2});
        for (const s32 object : {1, 2}) {
            collision.setObjectTransform(object,
                                         world.scene().worldTransform(static_cast<usize>(object)));
        }
        GameContext context;
        context.config = &config;
        context.unpackedRoot = assets;
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{0, 5, 0};
        CharacterSave save;
        save.progress().health = 10000;
        const std::array party{PartyMember{0, save}, PartyMember{1, save}};
        REQUIRE(play.open(device, context, world, std::span(party).first(second ? 2 : 1), options));
        if (pad == Pad::Forced) {
            world.activateTrigger(5, false);
        }
        for (s32 tick = 0;
             tick < 400 && (!play.movementPredictable() || play.animator(0)->entering()); ++tick) {
            REQUIRE(play.update(1.0 / 60, {}) == PlayOutcome::Running);
        }
        REQUIRE(play.movementPredictable());
        REQUIRE_FALSE(play.animator(0)->entering());
        REQUIRE(PlayerAnimator::locomotion(play.animator(0)->action()));
        place(0, 1);
        if (second) {
            place(1, 3, Vec3{80, 0, 0});
        }
    }

    PlayerRuntime& player(usize index = 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): fixture owns this live native
        // scene
        return const_cast<PlayerRuntime&>(play.participants()[index]);
    }
    void place(usize index, s32 object, Vec3 local = Vec3{0}) {
        auto& runtime = player(index);
        const auto transform = world.collision().objectTransform(object);
        runtime.actor.place(transform ? Vec3{*transform * Vec4{local, 1}}
                                      : Vec3{local.x, -10, local.z});
        runtime.floor.object = object;
        runtime.floor.flags = transform ? 4100U : 4U;
        runtime.floor.placement = transform;
    }
    bool permitted(u32 ticks = PlatformState::kMaxTicks) const {
        return play.platformPredictable(0, world.motion(), ticks);
    }
};

TEST_CASE("native moving rider admission preserves live clocks and excludes unsafe player states",
          "[netplay][platform-admission][assets]") {
    PlatformScene scene;
    REQUIRE(scene.permitted());
    const auto motion = scene.world.motion();
    const auto floor = scene.player().floor;
    const auto feet = scene.player().actor.position();
    const auto animation = scene.player().figure->animator().checkpoint();
    CHECK(scene.permitted(1));
    CHECK_FALSE(scene.permitted(0));
    CHECK_FALSE(scene.permitted(PlatformState::kMaxTicks + 1));
    CHECK(scene.world.motion() == motion);
    CHECK(scene.player().actor.position() == feet);
    CHECK(scene.player().floor.placement == floor.placement);
    CHECK(scene.player().figure->animator().checkpoint() == animation);
    auto malformed = motion;
    malformed.collision.clear();
    CHECK_FALSE(scene.play.platformPredictable(0, malformed, 12));
    scene.player().reaction = PlayerDeed::Flinch;
    CHECK_FALSE(scene.permitted());
    scene.player().reaction = PlayerDeed::None;
    scene.player().actor.place(feet + Vec3{0, 1, 0});
    CHECK_FALSE(scene.permitted());
    scene.player().actor.place(feet);
    auto& inventory = scene.player().actor.save().progress().inventory;
    inventory.powerups[0] = {-1, powerup::kSpecial, 0, powerup::kLevitation, true};
    CHECK_FALSE(scene.permitted());
    inventory = {};
    inventory.powerups[0] = {-1, powerup::kArmor, 0, powerup::kFireShield, true};
    CHECK(scene.permitted());
    inventory.powerups[0].strength = 0.001f;
    CHECK_FALSE(scene.permitted());
}

TEST_CASE("moving subtree admission rejects a distant sibling rider and pickup",
          "[netplay][platform-admission][assets]") {
    PlatformScene scene(Pad::None, true);
    REQUIRE(scene.permitted());
    scene.place(1, 2);
    REQUIRE(glm::distance(scene.player().actor.position(), scene.player(1).actor.position()) > 30);
    CHECK_FALSE(scene.permitted());
    scene.place(1, 3, Vec3{80, 0, 0});
    REQUIRE(scene.permitted());
    const Vec3 sibling{scene.world.scene().worldTransform(2)[3]};
    REQUIRE(scene.world.placeItem(scene.device, "APPLE", sibling));
    REQUIRE(scene.world.placedItems().size() == 1);
    REQUIRE(scene.world.placedItems().item(0).floor);
    REQUIRE(scene.world.placedItems().item(0).floor->object == 2);
    CHECK_FALSE(scene.permitted());
}

TEST_CASE("moving support pressure cannot keep stale contacts but forced motion may continue",
          "[netplay][platform-admission][assets]") {
    const Pad mode = GENERATE(Pad::Pressure, Pad::Forced);
    PlatformScene scene(mode);
    const std::array visitors{
        TriggerVisitor{.position = scene.player().actor.position(), .floorObject = 1}};
    scene.world.updateTriggers(1.0f / 60, visitors);
    REQUIRE(scene.world.triggers().opened(0));
    REQUIRE(scene.world.motion().triggers.targets[0].pressed == (mode == Pad::Pressure));
    CHECK(scene.permitted() == (mode == Pad::Forced));
}
} // namespace
