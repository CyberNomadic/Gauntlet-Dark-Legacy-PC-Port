#pragma once

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/screens/OpponentPrediction.h"
#include "game/screens/OpponentResources.h"
#include "game/world/LevelWorld.h"

namespace gdl::test {
using namespace game;

struct ContinuationFixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    WorldCollision collision;
    Enemies enemies;
    Generators generators;
    EnemyMissiles missiles{45};
    OpponentSimulation native{generators, enemies, missiles};
    OpponentPrediction prediction;
    std::array<EnemyView, 1> players;
    std::vector<EnemyMissiles::Resource> resources;

    explicit ContinuationFixture(f32 gain = 1, s32 initialPlayers = 1, std::string_view name = {}) {
        auto level = LevelRef::tower();
        if (!name.empty()) {
            LevelCatalog catalog;
            REQUIRE(catalog.load(root));
            const auto found = catalog.byName(name);
            REQUIRE(found);
            level = *found;
        }
        REQUIRE(world.load(device, root, level));
        collision = world.collision();
        if (name.empty()) {
            // Isolate AI/contact behavior from tower walls while still loading
            // real level tuning, native enemy trees and generator resources.
            collision.build(
                {{{0, 1, 0}, {Vec3{-200, 0, -200}, Vec3{200, 0, 200}, Vec3{200, 0, -200}}, 0, 4},
                 {{0, 1, 0}, {Vec3{-200, 0, -200}, Vec3{-200, 0, 200}, Vec3{200, 0, 200}}, 0, 4}});
        }
        EnemyScales scales;
        scales.players = initialPlayers;
        GeneratorScales breeding;
        s32 most = Enemies::kMost;
        const auto* info = world.level();
        REQUIRE(info != nullptr);
        scales.health = info->tuning.enemyHealth;
        scales.speed = info->tuning.enemySpeedScale(gain);
        scales.sight = info->tuning.enemySightScale(gain);
        scales.damage = info->tuning.enemyDamage;
        scales.missileRate = info->tuning.enemyMissileRate;
        scales.missileAim = info->tuning.enemyMissileAim;
        scales.playerLevel = info->tuning.playerLevel;
        scales.bossEncounter = info->bossType >= 0;
        breeding.health = info->tuning.generatorHealth;
        breeding.rate = info->tuning.generatorRateScale(gain);
        breeding.most = info->tuning.generatorMostScale(gain);
        most = info->maxEnemies;
        enemies.open(device, root, &collision, most, scales, 1125);
        enemies.setContactGridBounds(world.layout().minBounds(), world.layout().maxBounds());
        enemies.setLookouts(LookoutRoute::of(world.layout().locators()));
        enemies.setHazards(&world.hazards());
        REQUIRE(enemies.loadKind(kGruntKind));
        const std::array lenders{&world.textures(), &world.items().textures,
                                 &world.realmItems().textures};
        REQUIRE(generators.bind(device, world.layout(), enemies, &collision, breeding,
                                initialPlayers, info->enemies, level.realmId, &world.items(),
                                lenders));
        resources = OpponentResources::missiles(enemies);
        REQUIRE(prediction.open(device, root, world, enemies, initialPlayers, gain));
        players[0].player = 0;
        players[0].position = {0, 0, 0};
    }

    OpponentState capture() const {
        const auto result = native.checkpoint(resources);
        REQUIRE(result);
        return *result;
    }
    OpponentSimulation::Frame frame(s32 ticks = 1) const {
        OpponentSimulation::Frame result;
        result.ticks = ticks;
        result.seconds = static_cast<f32>(ticks) / 60;
        result.players = players;
        result.collision = &collision;
        return result;
    }
    s32 spawn(Vec3 position = {0, 0, -18}) {
        const auto slot = enemies.spawn(
            {.kind = kGruntKind, .algorithm = kChaseWay, .position = position, .placed = true},
            players);
        REQUIRE(slot);
        return *slot;
    }
};
} // namespace gdl::test
