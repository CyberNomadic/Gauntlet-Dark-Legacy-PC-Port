#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/characters/CharacterSave.h"
#include "game/characters/Progression.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("armor absorption follows class level and permanent bonuses", "[players][damage]") {
    ClassStats stats;
    stats.armorMin = 300;
    stats.armorMax = 600;
    ClassProgress progress;
    CHECK(armorDefense(stats, progress) == Catch::Approx(1.5f));
    progress.experience = levelExperience(21);
    CHECK(armorDefense(stats, progress) == Catch::Approx(2));
    progress.armorAdd = 10.5f;
    CHECK(armorDefense(stats, progress) == Catch::Approx(2.0525f));
    progress.experience = levelExperience(99);
    CHECK(armorDefense(stats, progress) == Catch::Approx(3.0525f));
    progress.armorAdd = 1000;
    CHECK(armorDefense(stats, progress) == Catch::Approx(4.995f));
    progress.armorAdd = -1000;
    CHECK(armorDefense(stats, progress) == 0);
}

TEST_CASE("Sumner absorbs damage with the master's armor instead of wizard growth",
          "[players][damage][sumner-armor]") {
    ClassStats wizard;
    wizard.armorMin = 100;
    wizard.armorMax = 500;
    CharacterSave save;
    save.character = kSumnerClass;
    for (const s32 level : {1, 21, 99}) {
        save.progress().experience = levelExperience(level);
        for (const f32 bonus : {-1000.0f, 0.0f, 10.5f, 1000.0f}) {
            save.progress().armorAdd = bonus;
            CHECK(armorDefense(wizard, save) == Catch::Approx(4.995f));
        }
    }

    save.character = kWizardClass;
    save.progress().experience = levelExperience(21);
    save.progress().armorAdd = 10.5f;
    CHECK(armorDefense(wizard, save) == Catch::Approx(1.0525f));
    CHECK(armorDefense(wizard, save) == armorDefense(wizard, save.progress()));
}

TEST_CASE("the experience curve matches the original's levels", "[game][players]") {
    REQUIRE(levelExperience(1) == 0);
    REQUIRE(levelExperience(2) == 1060);
    REQUIRE(levelExperience(60) == 165200);
    REQUIRE(levelExperience(61) == 165200 + 4600);
    REQUIRE(experienceLevel(0) == 1);
    REQUIRE(experienceLevel(1059) == 1);
    REQUIRE(experienceLevel(1060) == 2);
    REQUIRE(experienceLevel(165200) == 60);
    REQUIRE(experienceLevel(10'000'000) == kMaxLevel);
}

TEST_CASE("every decade and legend rank require a separate tower award",
          "[game][players][promotion]") {
    ClassProgress progress;
    progress.promotedLevel = 1;
    for (s32 level = 1; level <= kMaxLevel; ++level) {
        progress.experience = levelExperience(level);
        const bool milestone = level % 10 == 0 || level == kMaxLevel;
        REQUIRE(progress.promotionPending() == milestone);
        if (milestone) {
            REQUIRE(progress.appearanceLevel() < level);
            progress.promotedLevel = level;
            REQUIRE_FALSE(progress.promotionPending());
        }
    }
}

TEST_CASE("displayed stats grow with the level and the saved bonuses", "[game][players]") {
    ClassStats warrior;
    warrior.fightMin = 600.0f;
    warrior.speedMin = 350.0f;
    warrior.armorMin = 300.0f;
    warrior.magicMin = 100.0f;
    const StatBlock fresh = displayStats(warrior, 1, ClassProgress{});
    REQUIRE(fresh.strength() == 600);
    REQUIRE(fresh.speed() == 350);
    REQUIRE(fresh.armor() == 300);
    REQUIRE(fresh.magic() == 100);
    REQUIRE(fresh.best() == 0);

    ClassProgress progress;
    progress.magicAdd = 20.0f;
    const StatBlock later = displayStats(warrior, 3, progress);
    REQUIRE(later.strength() == 610);
    REQUIRE(later.magic() == 130);

    ClassProgress huge;
    huge.fightAdd = 5000.0f;
    REQUIRE(displayStats(warrior, 99, huge).strength() == kMaxStat);

    // Growth stops at the class's most; what was bought still goes on top (PlayerUpdateAtts).
    warrior.fightMax = 650.0f;
    warrior.magicMax = 500.0f;
    REQUIRE(displayStats(warrior, 99, ClassProgress{}).strength() == 650);
    REQUIRE(displayStats(warrior, 99, ClassProgress{}).magic() == 500);
    REQUIRE(displayStats(warrior, 99, progress).magic() == 520);
    REQUIRE(displayStats(warrior, 3, ClassProgress{}).strength() == 610); // not yet reached

    const StatBlock master = masteryStats();
    REQUIRE(master.magic() == kMaxStat);
    REQUIRE(master.best() == 0);
}

} // namespace
