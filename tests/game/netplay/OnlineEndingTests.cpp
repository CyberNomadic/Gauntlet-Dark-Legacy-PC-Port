#include <algorithm>
#include <array>
#include <filesystem>
#include <span>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/OnlinePair.h"
#include "game/screens/OnlinePlay.h"
#include "game/world/TowerAccess.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct EndingPair {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice hostDevice;
    test::FakeRenderDevice guestDevice;
    GameConfig config;
    StringTable strings;
    LevelCatalog catalog;
    GameContext hostContext;
    GameContext guestContext;
    test::OnlinePair wire{{}, false};
    OnlineParty hostParty;
    OnlineParty guestParty;
    OnlinePlay host;
    OnlinePlay guest;
    usize hostSaved = 0;
    usize guestSaved = 0;

    EndingPair() {
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        config.game.autoSave = true;
        hostContext.config = &config;
        hostContext.strings = &strings;
        hostContext.levels = &catalog;
        hostContext.unpackedRoot = root;
        guestContext = hostContext;
        hostContext.saveLocalParty = [&](std::span<PartyMember> members) {
            ++hostSaved;
            REQUIRE(members.size() == 1);
            CHECK(members.front().player == 1); // local physical device, not room seat zero
            CHECK(members.front().slot == 14);
        };
        guestContext.saveLocalParty = [&](std::span<PartyMember> members) {
            ++guestSaved;
            REQUIRE(members.size() == 1);
            CHECK(members.front().player == 2);
            CHECK(members.front().slot == 15);
        };
        const auto choice = [](s32 device, s32 character, usize slot) {
            CharacterSave save;
            save.name = character == 0 ? "HOST" : "GUEST";
            save.character = character;
            save.color = character;
            save.gold = 4321 + character;
            save.progress().experience = levelExperience(60);
            save.progress().health = 10000;
            save.progress().lifetime.enemiesKilled = 73 + character;
            save.progress().lifetime.goldFound = 10432 + character;
            save.progress().relics.shards = TowerAccess::kBattlefieldShards;
            save.progress().relics.runes = TowerAccess::kGarmRunes;
            // These endings are reached after their approach levels. H1 also lowers
            // the native Battlefield return bridge; unlocking shards alone does not.
            save.progress().levels.beaten[6] = 0x01;
            save.progress().levels.beaten[8] = 0x07;
            return PartyMember{device, save, slot};
        };
        REQUIRE(hostParty.select(wire.hostSession, std::array{choice(1, 0, 14)}));
        REQUIRE(guestParty.select(wire.guestSession, std::array{choice(2, 1, 15)}));
        wire.start();
    }

    void step(const SessionInputs::Frame& hostInputs = {},
              const SessionInputs::Frame& guestInputs = {}, bool renderGuest = true) {
        REQUIRE(host.update(hostInputs) != OnlinePlay::Phase::Failed);
        REQUIRE(guest.update(guestInputs) != OnlinePlay::Phase::Failed);
        if (host.entryVisible()) {
            hostDevice.draws.clear();
            host.render(hostDevice, makeScreenProjection(1280, 720), 1280, 720, 1.0 / 60, 1);
        }
        // Hold only the ending movie. Boss/death/farewell snapshots must still pass
        // the actual independently loaded replica's show/draw path every frame.
        if (renderGuest || guest.completion().phase() != OnlineCompletion::Phase::Movie) {
            guestDevice.draws.clear();
            guest.render(guestDevice, makeScreenProjection(1280, 720), 1280, 720, 1.0 / 60, 1);
            REQUIRE(guest.phase() != OnlinePlay::Phase::Failed);
        }
    }

    void open(std::string_view name) {
        const auto level = catalog.byName(name);
        REQUIRE(level);
        PlayOptions options;
        options.welcome = false;
        REQUIRE(guest.open(guestDevice, guestContext, wire.guestSession, guestParty));
        REQUIRE(host.open(hostDevice, hostContext, wire.hostSession, hostParty, *level, options));
        for (s32 tick = 0; tick < 7200 && (host.phase() != OnlinePlay::Phase::Playing ||
                                           guest.phase() != OnlinePlay::Phase::Playing);
             ++tick) {
            step();
        }
        REQUIRE(host.phase() == OnlinePlay::Phase::Playing);
        REQUIRE(guest.phase() == OnlinePlay::Phase::Playing);
    }
};

SessionInputs::Frame finishLane(const OnlinePlay& play, s32 device, s32 seat, s32 tick) {
    SessionInputs::Frame inputs;
    const auto* shop = play.shopScene();
    if (shop == nullptr || tick % 30 != 0) {
        return inputs;
    }
    const auto lane = std::ranges::find(shop->session().lanes(), seat,
                                        [](const ShopLane& value) { return value.member.player; });
    if (lane == shop->session().lanes().end() || lane->phase == ShopPhase::Done) {
        return inputs;
    }
    auto& input = inputs[static_cast<usize>(device)].menu;
    if (lane->phase == ShopPhase::Shopping && tick % 60 == 0) {
        input.start = true; // focus Exit before confirming, never buy the initial item
    } else {
        input.select = true;
    }
    return inputs;
}

TEST_CASE("native final bosses synchronize ending movies results and the correct Tower return",
          "[netplay][online-ending][assets]") {
    const s32 kind = GENERATE(43, 44);
    CAPTURE(kind);
    EndingPair pair;
    pair.open(kind == 44 ? "H4" : "F2");
    REQUIRE(pair.host.hostScene());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): drive the real host boss death fixture
    auto& scene = *const_cast<PlayScene*>(pair.host.hostScene());
    scene.bosses().wake();
    for (s32 tick = 0; tick < 300; ++tick) {
        pair.step();
    }
    REQUIRE(scene.bossView());
    REQUIRE(scene.bossView()->kind == kind);
    REQUIRE(pair.guest.shown());
    REQUIRE(std::ranges::any_of(pair.guest.shown()->fighters,
                                [](const FighterMeshState& mesh) { return mesh.actor == 0; }));
    REQUIRE_FALSE(pair.guestDevice.draws.empty());
    EnemyHit hit;
    hit.damage = 100000;
    hit.player = 0;
    REQUIRE(scene.bosses().hurt(hit) > 0);
    REQUIRE_FALSE(scene.bossView()->alive);
    CHECK_FALSE(scene.victory().finished());
    bool farewell = false;
    for (s32 tick = 0;
         tick < 9000 && pair.guest.completion().phase() != OnlineCompletion::Phase::Movie; ++tick) {
        if (const auto* current = pair.host.hostScene(); current && current->victory().caption()) {
            farewell |= current->victory().caption()->message ==
                        (kind == 44 ? "GARM2_SPEECH" : "SKORNE2_SPEECH");
        }
        pair.step({}, {}, false); // the guest is connected but has not presented its first frame
    }
    REQUIRE(farewell);
    REQUIRE(pair.host.completion().phase() == OnlineCompletion::Phase::Movie);
    REQUIRE(pair.guest.completion().phase() == OnlineCompletion::Phase::Movie);
    REQUIRE(pair.wire.host.shop().setup());
    CHECK(pair.wire.host.shop().setup()->ending);
    CHECK(pair.host.completion().selection().movie == (kind == 44 ? "victory" : "garm"));
    CHECK(pair.guest.completion().selection().movie == pair.host.completion().selection().movie);
    CHECK_FALSE(pair.host.completion().selection().skippable);
    CHECK_FALSE(pair.guest.completion().selection().skippable);
    REQUIRE(pair.wire.host.travelParty());
    const MatchParty awarded = *pair.wire.host.travelParty();
    for (usize seat = 0; seat < 2; ++seat) {
        REQUIRE(awarded[seat]);
        const usize realm = kind == 44 ? 8 : 6;
        const u8 completed = kind == 44 ? 8 : 2;
        CHECK((awarded[seat]->progress.levels.beaten[realm] & completed) != 0);
        CHECK(awarded[seat]->progress.relics.hasShard(LevelRef::orderOf(static_cast<s32>(realm))));
    }
    const auto epoch = pair.wire.host.context().epoch;
    for (s32 tick = 0;
         tick < 1800 && pair.host.completion().phase() != OnlineCompletion::Phase::Ready; ++tick) {
        pair.step({}, {}, false);
    }
    REQUIRE(pair.host.completion().phase() == OnlineCompletion::Phase::Ready);
    for (s32 tick = 0; tick < 120; ++tick) {
        pair.step({}, {}, false);
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Loading);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Loading);
    CHECK(pair.wire.host.phase() == MatchSession::Phase::Loading);
    CHECK(pair.wire.host.context().epoch == epoch);
    CHECK(pair.guest.completion().phase() == OnlineCompletion::Phase::Movie);
    CHECK(pair.hostSaved == 0);
    CHECK(pair.guestSaved == 0);
    // Even injecting an all-player skip request cannot cross a Shop-ending barrier.
    CHECK_FALSE(pair.wire.host.requestMovieSkip(0));
    CHECK_FALSE(pair.wire.guest.requestMovieSkip(1));
    for (s32 tick = 0; tick < 2400 && (pair.host.phase() != OnlinePlay::Phase::Shopping ||
                                       pair.guest.phase() != OnlinePlay::Phase::Shopping);
         ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Shopping);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Shopping);
    REQUIRE(pair.host.shopScene());
    REQUIRE(pair.guest.shopScene());
    const auto visit = kind == 44 ? ShopVisit::Completion : ShopVisit::Level;
    CHECK(pair.host.shopScene()->session().visit() == visit);
    CHECK(pair.guest.shopScene()->session().visit() == visit);
    CHECK(pair.wire.host.context() == pair.wire.guest.context());
    bool shopping = false;
    bool finalStats = false;
    const auto checkResults = [&] {
        for (const auto* play : {&pair.host, &pair.guest}) {
            const auto* results = play->shopScene();
            if (results == nullptr) {
                continue;
            }
            for (const auto& lane : results->session().lanes()) {
                const auto seat = static_cast<usize>(lane.member.player);
                REQUIRE(awarded[seat]);
                CHECK(lane.member.save.gold == awarded[seat]->gold);
                CHECK(lane.member.save.experience() == awarded[seat]->progress.experience);
                CHECK(lane.member.save.progress().lifetime.enemiesKilled ==
                      awarded[seat]->progress.lifetime.enemiesKilled);
                CHECK(lane.member.save.progress().relics.shards ==
                      awarded[seat]->progress.relics.shards);
                shopping |= lane.phase == ShopPhase::Shopping;
                finalStats |= lane.phase == ShopPhase::FinalStats;
                if (kind == 44) {
                    CHECK(lane.phase != ShopPhase::Shopping);
                    CHECK(lane.phase != ShopPhase::Inventory);
                }
            }
        }
    };
    // Only the host accepts its pages; the remote character still holds the exit barrier.
    for (s32 tick = 0; tick < 1800; ++tick) {
        pair.step(finishLane(pair.host, 1, 0, tick));
        if (tick % 60 == 0) {
            checkResults();
        }
    }
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Shopping);
    CHECK(pair.hostSaved == 0);
    CHECK(pair.guestSaved == 0);
    for (s32 tick = 0; tick < 4800; ++tick) {
        pair.step(finishLane(pair.host, 1, 0, tick), finishLane(pair.guest, 2, 1, tick));
        if (tick % 60 == 0) {
            checkResults();
        }
        if (pair.host.phase() == OnlinePlay::Phase::Playing &&
            pair.guest.phase() == OnlinePlay::Phase::Playing) {
            break;
        }
    }
    CHECK(shopping == (kind == 43));
    CHECK(finalStats == (kind == 44));
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.host.hostScene());
    const auto& returned = *pair.host.hostScene();
    REQUIRE(returned.world()->isTower());
    CHECK(pair.wire.host.context() == pair.wire.guest.context());
    CHECK(pair.wire.host.context().epoch == epoch + 1);
    const auto* marker = returned.world()->startPoint(kind == 44 ? 11 : 10);
    REQUIRE(marker);
    REQUIRE(returned.actor(0));
    REQUIRE(returned.actor(1));
    const auto center = (returned.actor(0)->position() + returned.actor(1)->position()) * 0.5f;
    CHECK(glm::distance(Vec2(center.x, center.z), Vec2(marker->position.x, marker->position.z)) <
          0.05f);
    for (const auto& member : returned.party()) {
        const auto seat = static_cast<usize>(member.player);
        REQUIRE(awarded[seat]);
        CHECK(member.save.gold == awarded[seat]->gold);
        CHECK(member.save.experience() == awarded[seat]->progress.experience);
        CHECK(member.save.progress().lifetime.enemiesKilled ==
              awarded[seat]->progress.lifetime.enemiesKilled);
    }
    const usize expectedSaves = kind == 43 ? 1 : 0;
    CHECK(pair.hostSaved == expectedSaves);
    CHECK(pair.guestSaved == expectedSaves);
    for (s32 tick = 0; tick < 180; ++tick) {
        pair.step();
    }
    CHECK(pair.hostSaved == expectedSaves);
    CHECK(pair.guestSaved == expectedSaves);
}
} // namespace
