#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/combat/DamageTypes.h"
#include "game/screens/CombatResources.h"
#include "game/screens/EnemyFeedbackEmission.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path archiveFixture(std::string_view name,
                                     std::span<const std::string_view> trees) {
    const auto directory = test::scratchDirectory(name);
    std::filesystem::create_directories(directory);
    writeTextFile(directory / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(directory / "objects.json", R"({"objects":[
      {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(directory / "skin.png", test::kTinyPng);
    writeTextFile(directory / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    std::string text = R"({"trees":[)";
    for (const auto tree : trees) {
        if (text.back() != '[') {
            text += ',';
        }
        text += std::string{R"({"name":")"} + std::string{tree} + R"(",
          "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"ACTIVE","frames":9,"rate":30}]})";
    }
    text += "]}";
    writeTextFile(directory / "animations.json", text);
    test::convertModelFixture(directory);
    return directory;
}

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive enemy;
    ItemArchive weapons;
    LevelInfo level;

    Fixture() {
        constexpr std::array<std::string_view, 3> local{"BLOODFX1", "TREEHIT", "TREEDIE"};
        constexpr std::array<std::string_view, 6> common{"BLOODFX1", "BLOODFX2", "HITCOL",
                                                         "HITDIE",   "TREEHIT",  "TREEDIE"};
        REQUIRE(enemy.load(archiveFixture("enemy-feedback-local", local)));
        REQUIRE(weapons.load(archiveFixture("enemy-feedback-common", common)));
        level.enemies = {LevelEnemy{13, 2, "EGRUNT"}, LevelEnemy{11, 1, "TRE"}};
        level.bossType = -1;
    }
};

TEST_CASE("swarm feedback resolves native archive priority opacity and sound independently",
          "[enemy-feedback-emission]") {
    Fixture fixture;
    EnemyFeedback hit{
        .kind = 13, .tier = 2, .close = true, .position = {2, 3, 4}, .yaw = 0.75f, .halfHeight = 4};
    auto emission =
        EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, &fixture.level);
    CHECK(emission.sound == "S_EGRUNT2HIT1CL");
    CHECK(emission.archive == &fixture.enemy);
    CHECK(emission.tree == "BLOODFX1");
    CHECK(emission.position == hit.position);
    CHECK(emission.setting.scale == 2);
    CHECK(emission.setting.yaw == hit.yaw);
    CHECK(emission.setting.tint.a == 96);

    hit.killed = true;
    emission = EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, &fixture.level);
    CHECK(emission.sound == "S_EGRUNT2DIECLO");
    CHECK(emission.archive == &fixture.weapons);
    CHECK(emission.tree == "BLOODFX2");
    hit.flags = damage::kNoHitEffect;
    emission = EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, &fixture.level);
    CHECK(emission.sound == "S_EGRUNT2DIECLO");
    CHECK(emission.archive == nullptr);
    CHECK(emission.tree.empty());
    CHECK_FALSE(emission.prepare({}, {}));

    hit.flags = 0;
    hit.kind = 11;
    emission = EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, &fixture.level);
    CHECK(emission.archive == &fixture.enemy);
    CHECK(emission.tree == "TREEDIE");
    CHECK(emission.setting.tint.a == 255);
    hit.killed = false;
    emission = EnemyFeedbackEmission::resolve(hit, nullptr, fixture.weapons, nullptr);
    CHECK(emission.archive == &fixture.weapons);
    CHECK(emission.tree == "TREEHIT");
    CHECK(emission.setting.tint.a == 255);
    CHECK(emission.sound.empty());
    hit.kind = 5;
    emission = EnemyFeedbackEmission::resolve(hit, nullptr, fixture.weapons, nullptr);
    CHECK(emission.tree == "HITCOL");
    CHECK(emission.setting.scale == 1);
    CHECK(emission.setting.tint.a == 96);

    hit.kind = 13;
    hit.flags = damage::kFire;
    emission = EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, &fixture.level);
    CHECK(emission.archive == nullptr); // neither native bank supplies FIREHIT
    CHECK(emission.tree.empty());
    CHECK(emission.sound == "S_EGRUNT2HIT1CL");
}

TEST_CASE("swarm feedback preparation requires the exact archive and ordered lender binding",
          "[enemy-feedback-emission]") {
    Fixture fixture;
    CombatResources resources;
    const std::vector<TextureSet*> lenders{&fixture.enemy.textures, &fixture.weapons.textures};
    const std::array banks{CombatResources::Bank{1, &fixture.enemy, lenders},
                           CombatResources::Bank{2, &fixture.weapons, lenders}};
    REQUIRE(resources.bind(fixture.device, banks));
    const EnemyFeedback hit{.kind = 13, .halfHeight = 4};
    const auto emission =
        EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, nullptr);
    const auto prepared = emission.prepare(resources.resources().sharedEffects, lenders);
    REQUIRE(prepared);
    CHECK(prepared->archive == &fixture.enemy);
    CHECK(prepared->tree->name == "BLOODFX1");
    const std::array reversed{&fixture.weapons.textures, &fixture.enemy.textures};
    CHECK_FALSE(emission.prepare(resources.resources().sharedEffects, reversed));
    CHECK_FALSE(emission.prepare(resources.resources().sharedEffects, {}));
    CHECK_FALSE(emission.prepare({}, lenders));

    // Same authored spelling in another archive is not a permitted replacement.
    CombatResources other;
    REQUIRE(other.bind(fixture.device, std::span{banks}.subspan(1)));
    CHECK_FALSE(emission.prepare(other.resources().sharedEffects, lenders));

    auto missing = resources.resources().sharedEffects;
    std::vector<EffectTrees::PoolResource> nonEmitting;
    for (const auto& resource : missing) {
        if (resource.prototype == nullptr || !resource.prototype->emitParticles) {
            nonEmitting.push_back(resource);
        }
    }
    CHECK_FALSE(emission.prepare(nonEmitting, lenders));
}

TEST_CASE("prepared swarm feedback keeps native effect lifetime identities and shared clocks",
          "[enemy-feedback-emission]") {
    const s32 ticks = GENERATE(1, 2);
    Fixture fixture;
    CombatResources resources;
    const std::vector<TextureSet*> lenders{&fixture.enemy.textures, &fixture.weapons.textures};
    const std::array banks{CombatResources::Bank{1, &fixture.enemy, lenders},
                           CombatResources::Bank{2, &fixture.weapons, lenders}};
    REQUIRE(resources.bind(fixture.device, banks));
    EffectTrees native;
    EffectTrees replay;
    native.setTextureLenders(lenders);
    replay.setTextureLenders(lenders);
    const auto binding = resources.resources().sharedEffects;
    const f32 seconds = static_cast<f32>(ticks) / 60;
    for (s32 step = 0; step < 70; ++step) {
        if (step == 0 || step == 4 || step == 19) {
            const EnemyFeedback hit{.kind = step == 19 ? 11 : 13,
                                    .killed = step == 4,
                                    .position = {static_cast<f32>(step), 2, -4},
                                    .yaw = 0.5f,
                                    .halfHeight = 4};
            // Independent native awardEnemyLosses preparation, before sharing
            // its pure resolver with the replaying gameplay adapter.
            const auto tree = hit.effect();
            auto* archive = &fixture.enemy;
            if (!archive->trees.find(tree)) {
                archive = &fixture.weapons;
            }
            EffectTrees::Setting setting;
            setting.scale = hit.effectScale();
            setting.yaw = hit.yaw;
            if (tree != "TREEHIT" && tree != "TREEDIE") {
                setting.tint.a = 96;
            }
            const auto id = native.startSet(fixture.device, *archive, tree, hit.position, setting);
            REQUIRE(id != 0);
            const auto emission =
                EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, nullptr);
            const auto prepared = emission.prepare(binding, lenders);
            REQUIRE(prepared);
            const auto uploaded = fixture.device.texturesCreated;
            REQUIRE(replay.startPrepared(*prepared, emission.position, emission.setting) == id);
            CHECK(fixture.device.texturesCreated == uploaded);
        }
        const auto nativeState = native.checkpointPool(binding);
        REQUIRE(nativeState);
        CHECK(replay.checkpointPool(binding) == nativeState);
        native.capturePresentation();
        replay.capturePresentation();
        native.update(seconds);
        replay.update(seconds);
    }
    REQUIRE(native.count() == 0);
    CHECK(replay.count() == 0);
    CHECK(replay.checkpointPool(binding) == native.checkpointPool(binding));
}

} // namespace
