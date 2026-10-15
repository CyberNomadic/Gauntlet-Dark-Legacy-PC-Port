#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/CharacterProfile.h"
#include "game/netplay/PlacedEnemyCheckpoint.h"
#include "game/screens/OpponentResources.h"
#include "game/screens/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene scene;

    explicit Fixture(std::string_view name = "G1") {
        REQUIRE(catalog.load(root));
        const auto level = catalog.byName(name);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        GameContext context;
        context.config = &config;
        context.levels = &catalog;
        context.unpackedRoot = root;
        CharacterSave save;
        save.name = "WATCH";
        save.progress().health = 2000;
        REQUIRE(CharacterProfile::capture(save));
        const std::vector<PartyMember> party{{0, save}};
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
    }

    SimulationObservation::Digest digest() {
        const auto textures = device.texturesCreated;
        const auto uploads = device.textureUpdates;
        const auto draws = device.draws.size();
        const auto result = scene.nativeAgreementDigest();
        REQUIRE(result);
        CHECK(scene.nativeAgreementDigest() == result);
        CHECK(device.texturesCreated == textures);
        CHECK(device.textureUpdates == uploads);
        CHECK(device.draws.size() == draws);
        return *result;
    }
};
} // namespace

TEST_CASE("native watchdog observes boss generators without a restore-domain certificate",
          "[native-agreement-scene][assets]") {
    auto fixture = std::make_unique<Fixture>();
    const auto before = fixture->digest();
    auto& generators = fixture->scene.generators();
    const auto index = static_cast<s32>(generators.count());
    ItemArchive noArt;
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 100;
    info.height = 5;
    info.radius = 2;
    // This is the native boss-effect birth API. Missing artwork legitimately
    // leaves the gameplay generator present; no checkpoint state is injected.
    REQUIRE(generators.placeBoss(fixture->device, info, noArt, fixture->scene.enemies(), kGruntKind,
                                 Mat4{1}, nullptr));
    REQUIRE_FALSE(generators.checkpoint());
    const auto born = fixture->digest();
    CHECK(born != before);
    REQUIRE(generators.strike(index, 1, 0));
    CHECK(fixture->digest() != born);
    REQUIRE_FALSE(generators.checkpoint()); // Restore admission is unchanged.
}

TEST_CASE("native watchdog observes missile physics without a model manifest or packet cap",
          "[native-agreement-scene][assets]") {
    auto fixture = std::make_unique<Fixture>();
    auto& missiles = const_cast<EnemyMissiles&>(fixture->scene.enemyMissiles());
    TreeModel firstArt;
    TreeModel otherArt;
    const auto before = fixture->digest();
    missiles.launch(EnemyMissileKind::arrow(), {0, 10, 0}, {0, 10, 30}, 1, &firstArt, 0);
    REQUIRE(missiles.count() == 1);
    REQUIRE_FALSE(missiles.checkpoint(OpponentResources::missiles(fixture->scene.enemies())));
    const auto launched = fixture->digest();
    CHECK(launched != before);
    // Process-local artwork addresses must not influence agreement. Both are
    // legal borrowed models, but neither belongs to the replay stock manifest.
    const_cast<EnemyMissile&>(missiles.missile(0)).model = &otherArt;
    CHECK(fixture->digest() == launched);
    missiles.update(1.0f / 60, nullptr, {});
    REQUIRE(missiles.count() == 1);
    CHECK(fixture->digest() != launched);
    missiles.burst({0, 0, 0}, 5, 10, 0, 1, -1);
    const auto withBlast = fixture->digest();
    REQUIRE(missiles.burstCount() == 1);
    missiles.clear(); // Clear borrowers before the local artwork owners die.
    CHECK(fixture->digest() != withBlast);
}

TEST_CASE("native watchdog walks the entire pending placement roster beyond wire limits",
          "[native-agreement-scene][assets]") {
    auto fixture = std::make_unique<Fixture>();
    auto& placements = const_cast<PlacedEnemies&>(fixture->scene.placedEnemies());
    PlacedEnemies::Placement placement;
    placement.kind = kGruntKind;
    placement.spawn.kind = kGruntKind;
    placement.spawn.placed = true;
    std::vector<PlacedEnemies::Placement> roster(PlacedEnemyCheckpoint::kMaxPlacements + 1,
                                                 placement);
    placements.bind(roster);
    const auto local = placements.checkpoint();
    REQUIRE(local);
    REQUIRE_FALSE(PlacedEnemyCheckpoint::canonicalDigest(*local));
    const auto complete = fixture->digest();
    roster.back().spawn.position.x = 0.25f;
    placements.bind(roster);
    const auto changedLast = fixture->digest();
    CHECK(changedLast != complete); // No truncation to the wire's 512 records.
    usize dispatched = 0;
    placements.activate([&](const PlacedEnemies::Placement&) { ++dispatched; });
    REQUIRE(dispatched == roster.size());
    REQUIRE(placements.pending().empty());
    CHECK(fixture->digest() != changedLast);
}

TEST_CASE("native watchdog includes a real boss encounter without claiming full boss state",
          "[native-agreement-scene][assets]") {
    auto fixture = std::make_unique<Fixture>("G5");
    REQUIRE(fixture->scene.bosses().present());
    REQUIRE_FALSE(fixture->scene.bosses().view().awake);
    const auto sleeping = fixture->digest();
    fixture->scene.bosses().wake();
    REQUIRE(fixture->scene.bosses().view().awake);
    CHECK(fixture->digest() != sleeping);
    const auto& fighter = fixture->scene.bosses().fighter();
    const auto position = fighter.position();
    CHECK(fixture->scene.bosses().position() != nullptr);
    CHECK(*fixture->scene.bosses().position() == position);
}
