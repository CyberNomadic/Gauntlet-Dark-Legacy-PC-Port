#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/LegendsN64Texture.h"
#include "game/critters/CombatantAssets.h"
#include "game/enemies/LevelOpponents.h"
#include "game/scenes/LegendsLevel.h"
#include "game/scenes/LegendsTraversal.h"

namespace {
using namespace gdl;
using namespace gdl::game;

formats::LegendsMap nativeMap(const std::filesystem::path& directory) {
    const auto models = readFile(directory / "objects.wad");
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                             readFile(directory / "ipr.rom"));
    const auto dynamicPath = directory / "texdyn.rom";
    const auto dynamic =
        std::filesystem::is_regular_file(dynamicPath) ? readFile(dynamicPath) : std::vector<u8>{};
    for (auto& texture : formats::decodeLegendsN64Textures(models, dynamic)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    return map;
}

LevelOpponents::Events noPlayerDamage() {
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.destroyedGenerator = [](s32) {};
    events.shake = [] {};
    events.help = [](s32, usize) { return false; };
    return events;
}
} // namespace

TEST_CASE("All native Legends boss arenas open through the common opponent owner",
          "[legends][bosses][legends-boss-integration][assets]") {
    const auto nativeRoot = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(nativeRoot / "_program/Boss.bin")) {
        SKIP("native N64 boss extraction is not installed");
    }
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    const auto program = readFile(nativeRoot / "_program/game.bin");
    const auto overlay = readFile(nativeRoot / "_program/Boss.bin");
    for (const char* const name : {"mount6", "ice5", "town5", "castle5", "boss2", "hell2"}) {
        DYNAMIC_SECTION(name) {
            const auto encounter = legendsN64Boss(name, program, overlay);
            REQUIRE(encounter);
            const auto directory = nativeRoot / "levels" / name;
            auto map = nativeMap(directory);
            test::FakeRenderDevice device;
            auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
            auto imported =
                std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, root);
            REQUIRE(imported->importedBoss());
            CHECK(imported->importedBoss()->kind == encounter->combat.kind);
            CHECK(imported->arenaTargets().size() == encounter->combat.arenaRocks.size());
            CHECK(imported->arenaAnchors().size() == encounter->combat.arenaRocks.size());

            CharacterSave save;
            save.progress().legendsProgress(LegendsPlatform::N64).runestones = 0x1fff;
            std::array<PartyMember, 1> party;
            party[0].player = 0;
            party[0].save = save;
            imported->initializeParty(party);
            LegendsTraversal traversal;
            traversal.open(map, 0, save);
            LevelWorld world;
            LevelRef ref;
            ref.realmId = -1;
            ref.name = "n64:" + std::string(name);
            REQUIRE(world.loadImported(
                device, root, ref, WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                traversal.collision(), imported));
            CHECK(world.ref().name == ref.name);
            CHECK(world.level() == nullptr); // donor's campaign/tuning must never become this map
            ItemArchive weapons;
            REQUIRE(weapons.load(root / "WEAPONS"));
            EffectTrees effects;
            LevelSoundscape audio;
            audio.open(root, nullptr, nullptr); // shared cues; boss bank must be borrowed by open
            std::array<PlayerRuntime, 1> players;
            players[0].actor.spawn(0, save, nullptr, encounter->combat.position + Vec3{0, 0, 10},
                                   0);
            LevelOpponents opponents;
            opponents.open({device, world, weapons, effects, audio, root}, players);
            REQUIRE(opponents.bosses().present());
            CHECK(opponents.bosses().view().kind == encounter->combat.kind);
            CHECK(opponents.bosses().view().alive);
            CHECK_FALSE(opponents.bosses().view().awake);
            REQUIRE(opponents.bosses().position());
            CHECK(opponents.bosses().position()->x == encounter->combat.position.x);
            CHECK(opponents.bosses().position()->z == encounter->combat.position.z);
            usize cues = 0;
            for (const auto* assets : opponents.bosses().resources()) {
                REQUIRE(assets);
                for (const auto& effect : assets->data.sounds()) {
                    const auto cue = effect.soundFor(encounter->combat.counterpartStage.front());
                    if (cue.empty()) {
                        continue;
                    }
                    INFO(cue);
                    CHECK(audio.hasNamed(cue));
                    ++cues;
                }
            }
            CHECK(cues > 0);
            EnemyHit hit;
            hit.player = 0;
            hit.damage = 100;
            const f32 initial = opponents.bosses().view().health;
            opponents.bosses().hurt(hit);
            CHECK(opponents.bosses().view().health == initial);
            auto events = noPlayerDamage();
            events.arenaAnchors = [&] { return imported->arenaAnchors(); };
            events.arenaTargets = [&] { return imported->arenaTargets(); };
            events.activateArena = [&](const CombatArenaActivation& action) {
                imported->activateArena(action);
            };
            constexpr f32 kStep = 1.0f / 60;
            for (s32 frame = 0; frame < 360; ++frame) {
                REQUIRE(opponents.bosses().position());
                players[0].actor.place(*opponents.bosses().position() + Vec3{0, 0, 10});
                opponents.update(1, kStep, players, {}, events);
            }
            REQUIRE(opponents.bosses().view().awake);
            REQUIRE_FALSE(opponents.bosses().targets().empty());
            bool damaged = false;
            for (s32 frame = 0; frame < 180 && !damaged; ++frame) {
                opponents.bosses().hurt(hit);
                damaged = opponents.bosses().view().health < initial;
                if (!damaged) {
                    opponents.update(1, kStep, players, {}, events);
                }
            }
            CHECK(damaged);
            device.draws.clear();
            opponents.bosses().draw(device, Mat4{1}, world.lighting());
            CHECK_FALSE(device.draws.empty());

            // Exercise the same death callback as PlayScene: the native map must
            // receive victory only after the common combatant finishes dying.
            const auto scriptRewards = [&] {
                std::vector<LegendsPickup> result;
                for (const auto& pickup : imported->adapter().pickups()) {
                    if (pickup.nativeIndex >= 0x80000000U) {
                        result.push_back(pickup);
                    }
                }
                return result;
            };
            const auto before = scriptRewards();
            REQUIRE(before.size() == encounter->rewards.size());
            for (const auto& reward : before) {
                CHECK_FALSE(reward.active);
            }
            CHECK_FALSE(imported->travelRequested());
            std::array visitors{&players[0].actor};
            usize victories = 0;
            events.fallen = [&](const Vec3& position) {
                ++victories;
                imported->bossDefeated(position + opponents.bosses().rewardOffset(), visitors);
            };
            hit.damage = 100000;
            for (s32 frame = 0; frame < 1800 && victories == 0; ++frame) {
                if (opponents.bosses().view().alive) {
                    opponents.bosses().hurt(hit);
                }
                imported->update(kStep, false);
                opponents.update(1, kStep, players, {}, events);
            }
            REQUIRE(victories == 1);
            CHECK_FALSE(opponents.bosses().present());
            CHECK_FALSE(imported->travelRequested());
            for (s32 frame = 0; frame < 3; ++frame) {
                opponents.update(1, kStep, players, {}, events);
            }
            CHECK(victories == 1);
            const auto released = scriptRewards();
            for (const auto& reward : released) {
                CHECK(reward.active);
                CHECK_FALSE(reward.collected);
            }
            imported->bossDefeated(encounter->combat.position + Vec3{1}, visitors);
            const auto repeated = scriptRewards();
            REQUIRE(repeated.size() == released.size());
            for (usize i = 0; i < repeated.size(); ++i) {
                CHECK(repeated[i].position == released[i].position);
            }
            CHECK(players[0]
                      .actor.save()
                      .progress()
                      .legendsProgress(LegendsPlatform::N64)
                      .runestones ==
                  0x1fff); // final unsupported permanent reward cannot eat earned runestones
            imported->completeTravel(party);
            CHECK_FALSE(
                party[0].save.progress().legendsProgress(LegendsPlatform::N64).hasCompleted(name));
            const s32 waitFrames = static_cast<s32>(std::ceil(encounter->departureSeconds * 60));
            for (s32 frame = 0; frame < waitFrames - 1; ++frame) {
                imported->update(kStep, false);
            }
            CHECK_FALSE(imported->travelRequested());
            for (s32 frame = 0; frame < 2; ++frame) {
                imported->update(kStep, false);
            }
            REQUIRE(imported->destination());
            CHECK(*imported->destination() == "tower2");
            REQUIRE(imported->departure());
            CHECK_FALSE(imported->departure()->finished);
            for (s32 frame = 0; frame < 121; ++frame) {
                imported->update(kStep, false);
            }
            REQUIRE(imported->departure()->finished);
            party[0].save = players[0].actor.save();
            imported->completeTravel(party);
            CHECK(
                party[0].save.progress().legendsProgress(LegendsPlatform::N64).hasCompleted(name));
            CHECK_FALSE(party[0]
                            .save.progress()
                            .legendsProgress(LegendsPlatform::Dreamcast)
                            .hasCompleted(name));
            opponents.close();
            audio.close();
        }
    }
}
