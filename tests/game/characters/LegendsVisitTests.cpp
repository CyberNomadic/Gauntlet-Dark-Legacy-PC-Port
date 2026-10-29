#include <catch2/catch_test_macros.hpp>

#include "game/characters/LegendsVisit.h"
#include "game/level/LegendsItems.h"

namespace {
using namespace gdl;
using namespace gdl::game;
} // namespace

TEST_CASE("A Legends visit returns earned goods without moving GDL progression",
          "[legends][save]") {
    CharacterSave before;
    before.name = "TEST";
    before.gold = 20;
    auto working = before;
    LegendsPickup coin;
    coin.offer = {static_cast<s32>(ItemKind::Gold), 100};
    REQUIRE(takeLegendsPickup(coin, LegendsPlatform::N64, working).took());
    LegendsPickup rune;
    rune.award = LegendsAward{LegendsRelic::Runestone, 0};
    REQUIRE(takeLegendsPickup(rune, LegendsPlatform::N64, working).took());
    auto live = before;
    REQUIRE(commitLegendsVisit(live, before, working));
    CHECK(live.gold == 120);
    CHECK(live.progress().legendsProgress(LegendsPlatform::N64).runestones == 1);
    CHECK(live.progress().legendsProgress(LegendsPlatform::Dreamcast).runestones == 0);
    CHECK(live.progress().relics == before.progress().relics);
    CHECK(live.progress().levels == before.progress().levels);
    const auto loaded = CharacterSave::fromJson(live.toJson());
    CHECK(loaded.gold == live.gold);
    CHECK(loaded.progress().legends == live.progress().legends);
    CHECK(loaded.progress().relics == live.progress().relics);
}

TEST_CASE("A Legends visit cannot overwrite concurrent console edits or a different character",
          "[legends][save]") {
    CharacterSave before;
    before.name = "FIRST";
    auto working = before;
    working.gold = 100;
    auto live = before;
    SECTION("the parked character received a console grant") {
        live.gold = 1;
    }
    SECTION("another class was changed") {
        live.classes[1].experience = 200;
    }
    SECTION("the working character was replaced") {
        working.name = "OTHER";
    }
    SECTION("the working class was replaced") {
        working.character = 1;
    }
    const auto preserved = live;
    CHECK_FALSE(commitLegendsVisit(live, before, working));
    CHECK(live == preserved);
}
