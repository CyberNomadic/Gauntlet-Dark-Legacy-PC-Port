#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/LegendsN64Texture.h"
#include "game/scenes/LegendsLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct ProgressFixture {
    test::FakeRenderDevice device;
    std::unique_ptr<LegendsLevel> level;

    explicit ProgressFixture(std::string_view name) {
        const auto directory =
            test::dataDirectory().parent_path() / "assets/n64/extracted/levels" / name;
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Extracted N64 Legends assets unavailable");
        }
        const auto models = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                                 readFile(directory / "ipr.rom"));
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(models, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
        level = std::make_unique<LegendsLevel>(device, map, std::move(adapter), directory,
                                               test::assetOrSkip("PLAYERS").parent_path());
    }

    void admit(std::span<PartyMember> party, std::string_view destination) {
        const auto portals = level->adapter().portals();
        const auto found = std::ranges::find(portals, destination, &formats::LegendsN64Portal::map);
        REQUIRE(found != portals.end());
        level->initializeParty(party);
        std::array<PlayerActor, 4> actors;
        std::vector<PlayerActor*> visitors;
        for (const auto& member : party) {
            if (!member.fallen) {
                REQUIRE(member.player >= 0);
                REQUIRE(member.player < 4);
                auto& actor = actors[static_cast<usize>(member.player)];
                actor.spawn(member.player, member.save, nullptr, found->position, 0);
                visitors.push_back(&actor);
            }
        }
        for (usize tick = 0; tick < 600 && !level->destination(); ++tick) {
            level->capturePresentation();
            level->update(1.0f / 60, false);
            level->updateParty(visitors, 1.0f / 60, false);
        }
        REQUIRE(level->destination());
        CHECK(*level->destination() == destination);
        REQUIRE(level->departure());
        REQUIRE_FALSE(level->departure()->finished);
        // Mirror PlayScene::party(): completion applies to the up-to-date saves,
        // not the frozen entry copies or an unrelated class's inventory.
        for (auto& member : party) {
            if (!member.fallen) {
                member.save = actors[static_cast<usize>(member.player)].save();
            }
        }
    }
};

std::array<PartyMember, 3> progressionParty() {
    std::array<PartyMember, 3> party;
    for (usize index = 0; index < party.size(); ++index) {
        auto& member = party[index];
        member.player = static_cast<s32>(index);
        member.slot = index + 4;
        member.save.character = static_cast<s32>(index + 1);
        member.save.gold = 123 + static_cast<s32>(index);
        member.save.progress().health = 500;
        member.save.progress().relics.runes = 0x5;
        member.save.progress().levels.recordBeaten(1, 3);
        member.save.progress().legendsProgress(LegendsPlatform::Dreamcast).obelisks = 7;
        member.save.progress()
            .legendsProgress(LegendsPlatform::Dreamcast)
            .recordCompleted("levela1");
        member.save.classes[0].legendsProgress(LegendsPlatform::N64).recordCompleted("mount5");
    }
    party[2].fallen = true;
    return party;
}
} // namespace

TEST_CASE("Completed native departures record only attending characters' departing platform stage",
          "[legends][level-progress][assets]") {
    ProgressFixture fixture{"mount1"};
    auto party = progressionParty();
    const auto entry = party;
    REQUIRE_FALSE(fixture.level->adapter().isTower());

    // A console close/jump has no admitted, completed departure to grant.
    fixture.level->completeTravel(party);
    for (usize index = 0; index < party.size(); ++index) {
        CHECK(party[index].save == entry[index].save);
    }

    fixture.admit(party, "tower2");
    const auto admitted = party;
    fixture.level->completeTravel(party);
    for (usize index = 0; index < party.size(); ++index) {
        CHECK(party[index].save == admitted[index].save);
    }

    fixture.level->update(kLegendsPortalDepartureSeconds, false);
    REQUIRE(fixture.level->departure()->finished);
    fixture.level->completeTravel(party);
    for (usize index = 0; index < party.size(); ++index) {
        auto expected = admitted[index].save;
        if (!party[index].fallen) {
            REQUIRE(expected.progress()
                        .legendsProgress(LegendsPlatform::N64)
                        .recordCompleted("mount1"));
        }
        CHECK(party[index].save == expected);
        CHECK(party[index].slot == admitted[index].slot);
        CHECK_FALSE(party[index]
                        .save.progress()
                        .legendsProgress(LegendsPlatform::N64)
                        .hasCompleted("tower2"));
        const auto saved = party[index].save.toJson();
        CHECK(CharacterSave::fromJson(saved).toJson() == saved);
    }

    const auto completed = party;
    fixture.level->completeTravel(party);
    for (usize index = 0; index < party.size(); ++index) {
        CHECK(party[index].save == completed[index].save);
    }
}

TEST_CASE("Leaving the native Tower never records Tower or destination completion",
          "[legends][level-progress][tower][assets]") {
    ProgressFixture fixture{"tower2"};
    auto party = progressionParty();
    REQUIRE(fixture.level->adapter().isTower());
    fixture.admit(party, "mount1");
    const auto before = party;
    fixture.level->update(kLegendsPortalDepartureSeconds, false);
    REQUIRE(fixture.level->departure()->finished);
    fixture.level->completeTravel(party);
    for (usize index = 0; index < party.size(); ++index) {
        CHECK(party[index].save == before[index].save);
        CHECK_FALSE(party[index]
                        .save.progress()
                        .legendsProgress(LegendsPlatform::N64)
                        .hasCompleted("tower2"));
        CHECK_FALSE(party[index]
                        .save.progress()
                        .legendsProgress(LegendsPlatform::N64)
                        .hasCompleted("mount1"));
    }
}
