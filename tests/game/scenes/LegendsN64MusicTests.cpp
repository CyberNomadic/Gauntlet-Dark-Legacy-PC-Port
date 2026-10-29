#include <catch2/catch_test_macros.hpp>

#include "game/scenes/LegendsN64Music.h"

TEST_CASE("N64 level music uses native routing instead of filename inference",
          "[legends][audio][music]") {
    using gdl::legendsN64SongForLevel;
    CHECK(legendsN64SongForLevel("mount1") == 0);
    CHECK(legendsN64SongForLevel("castle6") == 7);
    CHECK(legendsN64SongForLevel("ice2") == 7);
    CHECK(legendsN64SongForLevel("ice5") == 4);
    CHECK(legendsN64SongForLevel("ice6") == 4);
    CHECK(legendsN64SongForLevel("town4") == 18);
    CHECK(legendsN64SongForLevel("boss1") == 28);
    CHECK(legendsN64SongForLevel("tower2") == 17);
    CHECK(legendsN64SongForLevel("TOWER2") == 17);
    CHECK(legendsN64SongForLevel("MoUnT1") == 0);
    CHECK(legendsN64SongForLevel("ICE2") == 7);
    CHECK(legendsN64SongForLevel("secret2") == 37);
    CHECK(legendsN64SongForLevel("secret6") == 38);
    CHECK_FALSE(legendsN64SongForLevel("secret1"));
    CHECK_FALSE(legendsN64SongForLevel("_test1"));
    CHECK_FALSE(legendsN64SongForLevel("unknown"));
    CHECK_FALSE(legendsN64SongForLevel("../mount1"));
}
