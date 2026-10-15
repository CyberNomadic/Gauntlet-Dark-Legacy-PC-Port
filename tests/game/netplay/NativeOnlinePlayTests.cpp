#include <algorithm>
#include <array>
#include <map>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/OnlinePair.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/NativeOnlinePlay.h"
#include "game/screens/OnlineParty.h"
#include "game/screens/SimulationObserver.h"
#include "game/world/TowerAccess.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr std::array<s32, 2> kSeats{1, 3};
constexpr s32 kHostDevice = 2;
constexpr s32 kGuestDevice = 0;

struct NativePair {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    RoomSnapshot room{"ABCDEFGH",
                      std::string(32, 'a'),
                      1,
                      false,
                      {{std::string(32, 'a'), {1}, false}, {std::string(32, 'b'), {3}, false}}};
    test::OnlinePair::Service hostService{room, std::string(32, 'a')};
    test::OnlinePair::Service guestService{room, std::string(32, 'b')};
    test::OnlinePair::Wire hostWire, guestWire;
    OnlineSession hostSession{hostService, hostWire, true, 1};
    OnlineSession guestSession{guestService, guestWire, false, 1};
    test::FakeRenderDevice hostDevice, guestDevice;
    GameConfig hostConfig, guestConfig;
    StringTable strings;
    LevelCatalog catalog;
    GameContext hostContext, guestContext;
    OnlineParty hostParty, guestParty;
    // Only the diagnostic test opts into these catalogs. NativeOnlinePlay does
    // not require or publish old combat snapshots to run either native scene.
    NativeOnlinePlay host, guest;
    std::unique_ptr<NativePlaySetup> hostSetup, guestSetup;
    u32 hostSaves = 0, guestSaves = 0;
    u32 hostManualSaves = 0, guestManualSaves = 0;
    bool manualSaveSucceeds = true;
    u32 hostAwards = 0, guestAwards = 0;
    u16 hostUnlocks = 0, guestUnlocks = 0;

    explicit NativePair(PlayOptions options = {}, std::string_view beatenLevel = {},
                        std::optional<bool> autoSave = {}, std::string_view initialLevel = "L1") {
        hostWire.remote = &guestWire;
        guestWire.remote = &hostWire;
        hostDevice.recordDraws = guestDevice.recordDraws = false;
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        hostConfig.camera.horizontalFovDegrees = 42;
        guestConfig.camera.horizontalFovDegrees = 97;
        hostConfig.timing.gameplayFrameRate = 30;
        guestConfig.timing.gameplayFrameRate = 120;
        hostConfig.game.autoSave = guestConfig.game.autoSave =
            autoSave.value_or(!beatenLevel.empty());
        hostContext.config = &hostConfig;
        guestContext.config = &guestConfig;
        for (auto* context : {&hostContext, &guestContext}) {
            context->strings = &strings;
            context->levels = &catalog;
            context->unpackedRoot = root;
        }
        hostContext.saveLocalParty = [&](std::span<PartyMember> members) {
            REQUIRE(members.size() == 1);
            CHECK(members[0].player == kHostDevice);
            CHECK(members[0].slot == 22);
            ++hostSaves;
        };
        guestContext.saveLocalParty = [&](std::span<PartyMember> members) {
            REQUIRE(members.size() == 1);
            CHECK(members[0].player == kGuestDevice);
            CHECK(members[0].slot == 20);
            ++guestSaves;
        };
        hostContext.manualSaveLocalParty = [&](std::span<PartyMember> members) {
            REQUIRE(members.size() == 1);
            CHECK(members[0].player == kHostDevice);
            CHECK(members[0].slot == 22);
            ++hostManualSaves;
            return manualSaveSucceeds;
        };
        guestContext.manualSaveLocalParty = [&](std::span<PartyMember> members) {
            REQUIRE(members.size() == 1);
            CHECK(members[0].player == kGuestDevice);
            CHECK(members[0].slot == 20);
            ++guestManualSaves;
            return manualSaveSucceeds;
        };
        hostContext.unlockClasses = [&](u16 mask) {
            hostUnlocks |= mask;
            ++hostAwards;
        };
        guestContext.unlockClasses = [&](u16 mask) {
            guestUnlocks |= mask;
            ++guestAwards;
        };
        const auto selected = [&](s32 device, s32 character, const std::string& name) {
            PartyMember result;
            result.player = device;
            result.slot = static_cast<usize>(20 + device);
            result.save.name = name;
            result.save.character = character;
            result.save.color = character;
            result.save.classUnlock = beatenLevel.empty() ? 0xFFFF : 0;
            result.save.moviesSeen = {"LOCAL"};
            result.save.classes[static_cast<usize>(character + 2)].experience = 123456;
            // Selected class is genuinely fresh: zero persisted health denotes
            // its default spawn health, not a fallen campaign continuation.
            REQUIRE(result.save.progress().health == 0);
            if (!beatenLevel.empty()) {
                const auto prior = catalog.byName(beatenLevel);
                REQUIRE(prior);
                result.save.progress().levels.recordBeaten(prior->realmId, prior->index);
                const auto gate = TowerAccess::kWorldGates[static_cast<usize>(prior->realmId)];
                REQUIRE(gate > 0);
                result.save.progress().crystals[static_cast<usize>(gate)] = -1;
            }
            return result;
        };
        REQUIRE(hostParty.select(hostSession, std::array{selected(kHostDevice, 0, "HOST")}));
        REQUIRE(guestParty.select(guestSession, std::array{selected(kGuestDevice, 1, "GUEST")}));
        // The guest's first poll creates its connection event for the host's
        // next poll. Admission of the room is not yet authenticated readiness.
        for (u32 attempt = 0;
             attempt < 10 && (!hostSession.connected() || !guestSession.connected()); ++attempt) {
            pump();
        }
        REQUIRE(hostSession.connected());
        REQUIRE(guestSession.connected());
        const OnlineSession::NativeIdentity identity{"test-executable", "same-test-runtime",
                                                     std::string(64, 'a'), std::string(64, 'b'),
                                                     "en"};
        auto rules = OnlineSession::NativeRules{};
        const auto initial = catalog.byName(initialLevel);
        REQUIRE(initial);
        const auto scene = NativeOnlinePlay::sceneId(catalog, *initial);
        REQUIRE(scene);
        rules.scene = *scene;
        REQUIRE(hostSession.configureNative(identity, rules));
        REQUIRE(guestSession.configureNative(identity));
        REQUIRE(hostSession.ready());
        REQUIRE(guestSession.ready());
        pump();
        REQUIRE(hostSession.start());
        for (u32 attempt = 0; attempt < 40; ++attempt) {
            pump();
            if (hostSession.nativeSetupPending()) {
                hostSetup = makeSetup(hostSession, hostConfig, options);
                REQUIRE(hostSession.confirmNativeSetup(hostSetup->digest()));
            }
            if (guestSession.nativeSetupPending()) {
                guestSetup = makeSetup(guestSession, guestConfig, options);
                REQUIRE(guestSession.confirmNativeSetup(guestSetup->digest()));
            }
            if (hostSession.phase() == OnlineSession::Phase::Active &&
                guestSession.phase() == OnlineSession::Phase::Active) {
                break;
            }
        }
        REQUIRE(hostSetup);
        REQUIRE(guestSetup);
        REQUIRE(hostSetup->digest() == guestSetup->digest());
        REQUIRE(hostSession.phase() == OnlineSession::Phase::Active);
        REQUIRE(guestSession.phase() == OnlineSession::Phase::Active);
        REQUIRE(hostSession.match().nativeInputs());
        REQUIRE(guestSession.match().nativeInputs());
        REQUIRE(
            guest.open(guestDevice, guestContext, guestSession, guestParty, std::move(guestSetup)));
        REQUIRE(host.open(hostDevice, hostContext, hostSession, hostParty, std::move(hostSetup)));
        for (u32 frame = 0; frame < 3600 && (host.phase() != NativeOnlinePlay::Phase::Playing ||
                                             guest.phase() != NativeOnlinePlay::Phase::Playing);
             ++frame) {
            step();
        }
        REQUIRE(host.phase() == NativeOnlinePlay::Phase::Playing);
        REQUIRE(guest.phase() == NativeOnlinePlay::Phase::Playing);
        REQUIRE(host.scene());
        REQUIRE(guest.scene());
    }

    static std::unique_ptr<NativePlaySetup>
    makeSetup(OnlineSession& session, const GameConfig& config, const PlayOptions& options) {
        const auto* agreed = session.nativeContract();
        REQUIRE(agreed);
        REQUIRE(session.party());
        REQUIRE(session.room());
        NativePlaySetup::Contract contract{
            agreed->rules.scene,        agreed->rules.autoMelee, agreed->rules.autoActivateItems,
            agreed->rules.expandedShop, agreed->identity.build,  agreed->identity.content,
            agreed->identity.data};
        auto result = NativePlaySetup::create(config, session.room()->settings, *session.party(),
                                              options, contract);
        REQUIRE(result);
        return result;
    }
    void pump() {
        hostSession.update(0);
        guestSession.update(0);
        REQUIRE(hostSession.phase() != OnlineSession::Phase::Failed);
        REQUIRE(guestSession.phase() != OnlineSession::Phase::Failed);
    }
    void draw(NativeOnlinePlay& play, test::FakeRenderDevice& device, f32 blend = 1) {
        play.render(device, Mat4{1}, 1280, 720, 1.0 / 60, blend);
        REQUIRE(play.phase() != NativeOnlinePlay::Phase::Failed);
    }
    void step(const SessionInputs::Frame& first = {}, const SessionInputs::Frame& second = {}) {
        INFO("host failure=" << static_cast<u32>(host.failure())
                             << " guest failure=" << static_cast<u32>(guest.failure()));
        auto hostInput = first;
        auto guestInput = second;
        // Exercise the ordinary unanimous movie-skip vote, not a test-only
        // entry bypass. Each peer votes solely through its owned device.
        hostInput[kHostDevice].movieSkipPressed =
            host.entryPhase() == OnlineLevelEntry::Phase::Movie;
        guestInput[kGuestDevice].movieSkipPressed =
            guest.entryPhase() == OnlineLevelEntry::Phase::Movie;
        REQUIRE(host.update(hostInput) != NativeOnlinePlay::Phase::Failed);
        REQUIRE(guest.update(guestInput) != NativeOnlinePlay::Phase::Failed);
        if (host.entryVisible()) {
            draw(host, hostDevice);
        }
        if (guest.entryVisible()) {
            draw(guest, guestDevice);
        }
    }
    void settle() {
        for (u32 frame = 0; frame < 1200; ++frame) {
            REQUIRE(host.scene());
            REQUIRE(guest.scene());
            if (!host.scene()->spawning() && !guest.scene()->spawning() &&
                !host.scene()->startCamera().active() && !guest.scene()->startCamera().active() &&
                !host.scene()->scroll().active() && !guest.scene()->scroll().active()) {
                return;
            }
            SessionInputs::Frame first, second;
            first[kHostDevice].menu.select = true;
            second[kGuestDevice].menu.select = true;
            step(first, second);
        }
        FAIL("native entry did not release gameplay within its bounded test window");
    }
    void synchronize() {
        // Identical fixture mutations must occur at the same native boundary,
        // not merely in the same application loop on differently paced peers.
        for (u32 attempt = 0; attempt < 30 && host.completedTick() != guest.completedTick();
             ++attempt) {
            auto& behind = host.completedTick() < guest.completedTick() ? host : guest;
            REQUIRE(behind.update({}) != NativeOnlinePlay::Phase::Failed);
        }
        REQUIRE(host.completedTick() == guest.completedTick());
    }
    void placeOnPortal(NativeOnlinePlay& play, const std::string& destination = {},
                       bool secret = false) {
        REQUIRE(play.scene());
        const auto& exits = play.scene()->portals();
        usize found = 0;
        while (found < exits.size() &&
               (!exits.portal(found).destination ||
                (!destination.empty() && exits.portal(found).destination->name != destination) ||
                exits.portal(found).secret != secret)) {
            ++found;
        }
        REQUIRE(found < exits.size());
        REQUIRE_FALSE(exits.portal(found).shut);
        const auto position = exits.portal(found).position;
        for (const auto seat : kSeats) {
            REQUIRE(play.scene()->actor(seat));
            // Identical native contact fixture on each independent scene. No
            // transition/private flag is manufactured or network input changed.
            auto& actor = *const_cast<PlayerActor*>(play.scene()->actor(seat));
            actor.place(position);
        }
    }
};

PlayOptions immediateTower() {
    // The direct bootstrap fixture agrees these exact options on both peers;
    // menu sessions retain the default Tower setup.
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{0, 0, 0};
    return options;
}

TEST_CASE("native online bootstrap opens canonical sparse fresh parties on both peers",
          "[native-online-play][native-bootstrap][assets]") {
    auto pair = std::make_unique<NativePair>();
    CHECK(pair->host.scene() != pair->guest.scene());
    CHECK(pair->host.scene()->world() != pair->guest.scene()->world());
    CHECK(pair->host.scene()->world()->ref().isTower());
    CHECK(pair->guest.scene()->world()->ref().isTower());
    for (const auto* play : {&pair->host, &pair->guest}) {
        REQUIRE(play->scene()->actorCount() == 2);
        CHECK_FALSE(play->scene()->actor(0));
        CHECK_FALSE(play->scene()->actor(2));
        for (const auto& member : play->scene()->participants()) {
            CHECK(std::ranges::find(kSeats, member.actor.player()) != kSeats.end());
            CHECK_FALSE(member.slot);
            CHECK(member.life == PlayerLife::Standing);
            CHECK(member.actor.save().health() > 0);
            CHECK(member.actor.save().classUnlock == 0);
            CHECK(member.actor.save().moviesSeen.empty());
            CHECK(member.actor.save()
                      .classes[static_cast<usize>(member.actor.save().character + 2)]
                      .experience == 0);
        }
    }
    CHECK(pair->hostParty.localMembers()[0].slot == 22);
    CHECK(pair->guestParty.localMembers()[0].slot == 20);
    CHECK(pair->hostParty.localMembers()[0].save.classUnlock == 0xFFFF);
    CHECK(pair->guestParty.localMembers()[0].save.classUnlock == 0xFFFF);

    auto* host = const_cast<PlayScene*>(pair->host.scene());
    auto* guest = const_cast<PlayScene*>(pair->guest.scene());
    SimulationObserver hostObserver{*host}, guestObserver{*guest};
    REQUIRE(hostObserver.bind());
    REQUIRE(guestObserver.bind());
    std::map<u64, SimulationObservation> first, second;
    u32 compared = 0;
    const auto observe = [&](NativeOnlinePlay& play, SimulationObserver& observer,
                             std::map<u64, SimulationObservation>& samples) {
        if (play.completedTick() > 0 && play.completedTick() % 30 == 0 &&
            !samples.contains(play.completedTick())) {
            auto state = observer.observe();
            REQUIRE_FALSE(state.complete); // Partial diagnostics never certify the whole world.
            for (const auto& subsystem : state.subsystems) {
                INFO(subsystem.name << ": " << subsystem.unavailable);
                REQUIRE(subsystem.digest);
            }
            samples.emplace(play.completedTick(), std::move(state));
        }
    };
    for (u32 frame = 0; frame < 240; ++frame) {
        REQUIRE(pair->host.update({}) != NativeOnlinePlay::Phase::Failed);
        observe(pair->host, hostObserver, first); // No host renders after entry.
        REQUIRE(pair->guest.update({}) != NativeOnlinePlay::Phase::Failed);
        pair->draw(pair->guest, pair->guestDevice, 0.25f);
        pair->draw(pair->guest, pair->guestDevice, 0.75f);
        observe(pair->guest, guestObserver, second);
    }
    for (const auto& [tick, state] : first) {
        if (const auto found = second.find(tick); found != second.end()) {
            CAPTURE(tick);
            CHECK(state == found->second);
            ++compared;
        }
    }
    CHECK(compared >= 6);
}

TEST_CASE("native online local pause fences only the owned device while agreed ticks continue",
          "[native-online-play][native-pause][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower());
    pair->settle();
    REQUIRE_FALSE(pair->host.pause(kGuestDevice));
    REQUIRE(pair->host.pause(kHostDevice));
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Paused);
    const auto before = pair->host.completedTick();
    u32 checked = 0;
    for (u32 frame = 0; frame < 50; ++frame) {
        SessionInputs::Frame first, second;
        first[kHostDevice].move = {{1, 0}, 1};
        first[kHostDevice].attack = true;
        second[kGuestDevice].move = {{-1, 0}, 0.5f};
        pair->step(first, second);
        if (frame > 10) {
            for (const auto* session : {&pair->hostSession, &pair->guestSession}) {
                const auto& stopped = session->match().resolvedInput(1);
                const auto& moving = session->match().resolvedInput(3);
                REQUIRE(stopped);
                REQUIRE(moving);
                CHECK_FALSE(stopped->held(CommandHeld::Attack));
                CHECK(stopped->magnitude == 0);
                CHECK(moving->magnitude > 0);
                ++checked;
            }
        }
    }
    CHECK(checked > 0);
    CHECK(pair->host.completedTick() >= before + 40);
    REQUIRE(pair->host.resume(kHostDevice));
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK_FALSE(pair->host.resume(kHostDevice));
}

TEST_CASE("native online travel drains the terminal tick and retains the native shop party",
          "[native-online-play][native-travel][native-shop][assets]") {
    const bool autoSave = GENERATE(false, true);
    auto pair = std::make_unique<NativePair>(immediateTower(), "", autoSave);
    CHECK_FALSE(pair->host.hasCompletedProgress());
    CHECK_FALSE(pair->guest.hasCompletedProgress());
    CHECK_FALSE(pair->host.saveCompletedProgress());
    CHECK_FALSE(pair->guest.saveCompletedProgress());
    CHECK(pair->hostManualSaves == 0);
    CHECK(pair->guestManualSaves == 0);
    pair->settle();
    pair->synchronize();
    const auto epoch = pair->hostSession.match().context().epoch;
    bool observedDrain = false;
    u64 sourceTick = 0;
    for (u32 frame = 0; frame < 1800 && pair->hostSession.match().context().epoch == epoch;
         ++frame) {
        pair->placeOnPortal(pair->host, "G1");
        pair->placeOnPortal(pair->guest, "G1");
        REQUIRE(pair->host.update({}) != NativeOnlinePlay::Phase::Failed);
        if (pair->hostSession.match().context().epoch != epoch) {
            const auto& next = pair->hostSession.match().context();
            REQUIRE(next.transition == MatchTransition::Travel);
            REQUIRE(next.sourceEpoch == epoch);
            sourceTick = next.sourceTick;
            CHECK(sourceTick == pair->host.completedTick());
            const auto before = pair->guest.completedTick();
            // Poll control and source input without stepping the scene. The
            // authenticated Prepare must not replace an unexecuted old world.
            pair->guestSession.update(0);
            REQUIRE(pair->guestSession.match().nativeDraining());
            CHECK(pair->guestSession.match().context().epoch == epoch);
            CHECK(pair->guest.completedTick() == before);
            CHECK(pair->guest.scene()->world()->ref().isTower());
            CHECK(before < sourceTick);
            observedDrain = true;
        }
        REQUIRE(pair->guest.update({}) != NativeOnlinePlay::Phase::Failed);
    }
    REQUIRE(observedDrain);
    REQUIRE(sourceTick > 0);
    REQUIRE(pair->guest.completedTick() == sourceTick);
    REQUIRE(pair->guestSession.match().context().sourceTick == sourceTick);
    REQUIRE(pair->host.pause(kHostDevice));
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Loading);
    for (u32 frame = 0; frame < 3600 && (pair->host.phase() != NativeOnlinePlay::Phase::Playing ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Playing);
         ++frame) {
        pair->step();
        if (pair->host.phase() == NativeOnlinePlay::Phase::Paused) {
            // The local overlay survives resource loading/Commit and continues
            // to suppress its device until the explicit Resume action.
            REQUIRE(pair->host.resume(kHostDevice));
        }
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->host.scene());
    REQUIRE(pair->guest.scene());
    CHECK(pair->host.scene()->world()->ref().name == "G1");
    CHECK(pair->guest.scene()->world()->ref().name == "G1");
    CHECK(pair->hostSession.match().context() == pair->guestSession.match().context());
    for (const auto seat : kSeats) {
        CHECK(pair->host.scene()->actor(seat)->save().name ==
              pair->guest.scene()->actor(seat)->save().name);
        CHECK_FALSE(pair->host.scene()->party()[seat == 1 ? 0 : 1].slot);
        CHECK_FALSE(pair->guest.scene()->party()[seat == 1 ? 0 : 1].slot);
    }

    pair->settle();
    pair->synchronize();
    // Mutate the same legitimate native fixture on each peer. CharacterProfile
    // deliberately omits these transient fields: rebuilding from it at travel
    // would lose help history and the accumulated results checkpoint. Native
    // PartyRecords resets turbo at a level boundary; retain that rule too.
    for (const auto* play : {&pair->host, &pair->guest}) {
        for (const auto& member : play->scene()->participants()) {
            auto& runtime = const_cast<PlayerRuntime&>(member);
            runtime.actor.save().gold = 10000 + runtime.actor.player();
            runtime.turbo.add(37.0f);
            runtime.helpHeard = {HelpMessages::kFirstTurboName};
            runtime.levelKills = runtime.actor.player() + 2;
        }
    }
    const auto sourceEpoch = pair->hostSession.match().context().epoch;
    for (u32 frame = 0; frame < 1800 && (pair->host.phase() != NativeOnlinePlay::Phase::Shopping ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Shopping);
         ++frame) {
        // Normal G1 tags name its next stage. Native leaveBy redirects that
        // journey to the Tower through the tally/shop; do not invent a tag.
        if (pair->host.scene() && pair->host.scene()->world()->ref().name == "G1") {
            pair->placeOnPortal(pair->host);
        }
        if (pair->guest.scene() && pair->guest.scene()->world()->ref().name == "G1") {
            pair->placeOnPortal(pair->guest);
        }
        pair->step();
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Shopping);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Shopping);
    REQUIRE(pair->host.shopScene());
    REQUIRE(pair->guest.shopScene());
    CHECK(pair->hostSession.match().context() == pair->guestSession.match().context());
    CHECK(pair->hostSession.match().context().transition == MatchTransition::Shop);
    CHECK(pair->hostSession.match().context().sourceEpoch == sourceEpoch);
    CHECK(pair->hostSession.match().context().sourceTick > 0);
    CHECK(pair->hostSaves == 0);
    CHECK(pair->guestSaves == 0);
    const auto hostMembers = pair->host.shopScene()->session().party();
    const auto guestMembers = pair->guest.shopScene()->session().party();
    REQUIRE(hostMembers.size() == kSeats.size());
    REQUIRE(guestMembers.size() == kSeats.size());
    for (usize i = 0; i < kSeats.size(); ++i) {
        const auto& host = hostMembers[i];
        const auto& guest = guestMembers[i];
        CAPTURE(i, host.player, guest.player);
        CHECK(host.player == kSeats[i]);
        CHECK(guest.player == kSeats[i]);
        CHECK_FALSE(host.slot);
        CHECK_FALSE(guest.slot);
        CHECK(host.fallen == guest.fallen);
        CHECK(host.turbo == 0);
        CHECK(host.turbo == guest.turbo);
        CHECK(std::ranges::find(host.helpHeard, HelpMessages::kFirstTurboName) !=
              host.helpHeard.end());
        CHECK(host.helpHeard == guest.helpHeard);
        REQUIRE(host.resultsCheckpoint);
        REQUIRE(guest.resultsCheckpoint);
        CHECK(host.resultsCheckpoint->kills >= host.player + 2);
        CHECK(host.resultsCheckpoint->gold == guest.resultsCheckpoint->gold);
        CHECK(host.resultsCheckpoint->experience == guest.resultsCheckpoint->experience);
        CHECK(host.resultsCheckpoint->kills == guest.resultsCheckpoint->kills);
        const auto first = CharacterProfile::capture(host.save);
        const auto second = CharacterProfile::capture(guest.save);
        REQUIRE(first);
        REQUIRE(second);
        CHECK(CharacterProfilePacket::encode(*first) == CharacterProfilePacket::encode(*second));
    }

    const auto shopEpoch = pair->hostSession.match().context().epoch;
    REQUIRE(pair->host.pause(kHostDevice));
    CHECK(pair->host.phase() == NativeOnlinePlay::Phase::Shopping);
    const auto localLane = [](const NativeOnlinePlay& play, s32 seat) -> const ShopLane& {
        const auto& lanes = play.shopScene()->session().lanes();
        const auto found =
            std::ranges::find(lanes, seat, [](const ShopLane& lane) { return lane.member.player; });
        REQUIRE(found != lanes.end());
        return *found;
    };
    REQUIRE(localLane(pair->host, kSeats[0]).phase == ShopPhase::Tally);
    for (u32 frame = 0; frame < 90; ++frame) {
        SessionInputs::Frame first, second;
        first[kHostDevice].menu.select = second[kGuestDevice].menu.select = frame % 10 == 0;
        pair->step(first, second);
    }
    CHECK(localLane(pair->host, kSeats[0]).phase == ShopPhase::Tally);
    CHECK(localLane(pair->guest, kSeats[0]).phase == ShopPhase::Tally);
    CHECK(localLane(pair->host, kSeats[1]).phase != ShopPhase::Tally);
    REQUIRE(pair->host.resume(kHostDevice));
    for (u32 frame = 0; frame < 3600; ++frame) {
        SessionInputs::Frame first, second;
        if (frame % 60 == 0) {
            first[kHostDevice].menu.start = second[kGuestDevice].menu.start = true;
        } else if (frame % 60 == 30) {
            first[kHostDevice].menu.select = second[kGuestDevice].menu.select = true;
        }
        pair->step(first, second);
        if (pair->host.phase() == NativeOnlinePlay::Phase::Playing &&
            pair->guest.phase() == NativeOnlinePlay::Phase::Playing) {
            break;
        }
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->host.scene());
    REQUIRE(pair->guest.scene());
    CHECK(pair->host.scene()->world()->isTower());
    CHECK(pair->guest.scene()->world()->isTower());
    CHECK(pair->hostSession.match().context() == pair->guestSession.match().context());
    CHECK(pair->hostSession.match().context().sourceEpoch == shopEpoch);
    CHECK(pair->hostSession.match().context().sourceTick == 0);
    CHECK(pair->hostSaves == (autoSave ? 1U : 0U));
    CHECK(pair->guestSaves == (autoSave ? 1U : 0U));
    REQUIRE(pair->host.hasCompletedProgress());
    REQUIRE(pair->guest.hasCompletedProgress());
    const auto canonical = pair->host.scene()->party();
    const auto agreed = *pair->hostSession.match().travelParty();
    const auto completedGold = pair->hostParty.localMembers()[0].save.gold;
    const_cast<PlayerActor*>(pair->host.scene()->actor(kSeats[0]))->save().gold =
        completedGold + 777;
    REQUIRE(pair->host.saveCompletedProgress());
    CHECK(pair->hostParty.localMembers()[0].save.gold == completedGold);
    CHECK(pair->host.scene()->actor(kSeats[0])->save().gold == completedGold + 777);
    for (usize seat = 0; seat < agreed.size(); ++seat) {
        if (agreed[seat]) {
            CHECK(
                CharacterProfilePacket::encode(*agreed[seat]) ==
                CharacterProfilePacket::encode(*(*pair->hostSession.match().travelParty())[seat]));
        }
    }
    // Restore the test's deliberate live-only mutation before more shared ticks.
    const_cast<PlayerActor*>(pair->host.scene()->actor(kSeats[0]))->save().gold =
        canonical[0].save.gold;
    pair->manualSaveSucceeds = false;
    CHECK_FALSE(pair->guest.saveCompletedProgress());
    CHECK(pair->guest.hasCompletedProgress());
    pair->manualSaveSucceeds = true;
    REQUIRE(pair->guest.saveCompletedProgress());
    CHECK(pair->hostManualSaves == 1);
    CHECK(pair->guestManualSaves == 2);
    CHECK(pair->hostParty.localMembers()[0].save.classUnlock == 0xFFFF);
    CHECK(pair->guestParty.localMembers()[0].save.classUnlock == 0xFFFF);
    for (const auto* play : {&pair->host, &pair->guest}) {
        for (const auto& member : play->scene()->participants()) {
            CHECK_FALSE(member.slot);
            CHECK_FALSE(member.resultsCheckpoint);
            CHECK(std::ranges::find(member.helpHeard, HelpMessages::kFirstTurboName) !=
                  member.helpHeard.end());
        }
    }
}

TEST_CASE("native secret travel resumes each retained parent and commits only local rewards",
          "[native-online-play][native-secret][assets]") {
    // B1 completion is part of the agreed initial profiles, not a fabricated
    // Prepare. It opens the actual Tower portal to B2, whose authored secret
    // exit reaches S4 through the same native contact phase on both peers.
    auto pair = std::make_unique<NativePair>(immediateTower(), "B1");
    pair->settle();
    pair->synchronize();
    const auto towerEpoch = pair->hostSession.match().context().epoch;
    for (u32 frame = 0; frame < 1800 && pair->hostSession.match().context().epoch == towerEpoch;
         ++frame) {
        pair->placeOnPortal(pair->host, "B2");
        pair->placeOnPortal(pair->guest, "B2");
        pair->step();
    }
    REQUIRE(pair->hostSession.match().context().transition == MatchTransition::Travel);
    for (u32 frame = 0; frame < 3600 && (pair->host.phase() != NativeOnlinePlay::Phase::Playing ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Playing);
         ++frame) {
        pair->step();
    }
    REQUIRE(pair->host.scene());
    REQUIRE(pair->guest.scene());
    REQUIRE(pair->host.scene()->world()->ref().name == "B2");
    REQUIRE(pair->guest.scene()->world()->ref().name == "B2");
    pair->settle();
    pair->synchronize();
    auto* hostParent = const_cast<PlayScene*>(pair->host.scene());
    auto* guestParent = const_cast<PlayScene*>(pair->guest.scene());
    const auto* hostWorld = hostParent->world();
    const auto* guestWorld = guestParent->world();
    const auto parentEpoch = pair->hostSession.match().context().epoch;
    for (auto* scene : {hostParent, guestParent}) {
        for (const auto seat : kSeats) {
            const_cast<PlayerActor*>(scene->actor(seat))->save().gold += 250 + seat;
            scene->awardExperience(seat, 100, true);
        }
    }
    const auto parentResults = hostParent->levelResults();
    for (u32 frame = 0; frame < 1800 && pair->hostSession.match().context().epoch == parentEpoch;
         ++frame) {
        pair->placeOnPortal(pair->host, "S4", true);
        pair->placeOnPortal(pair->guest, "S4", true);
        pair->step();
    }
    REQUIRE(pair->hostSession.match().context().transition == MatchTransition::SecretEnter);
    REQUIRE(pair->guestSession.match().context() == pair->hostSession.match().context());
    const auto hostFrozen = hostWorld->scene().geometry();
    const auto guestFrozen = guestWorld->scene().geometry();
    const auto consumed = [](const PlayScene& scene) {
        usize count = 0;
        for (usize index = 0; index < scene.portals().size(); ++index) {
            const auto& portal = scene.portals().portal(index);
            count += portal.secret && portal.consumed ? 1 : 0;
        }
        return count;
    };
    CHECK(consumed(*hostParent) == 1);
    CHECK(consumed(*guestParent) == 1);
    for (u32 frame = 0; frame < 3600 && (pair->host.phase() != NativeOnlinePlay::Phase::Playing ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Playing);
         ++frame) {
        pair->step();
    }
    REQUIRE(pair->host.scene());
    REQUIRE(pair->guest.scene());
    REQUIRE(pair->host.scene()->world()->ref().name == "S4");
    REQUIRE(pair->guest.scene()->world()->ref().name == "S4");
    CHECK(pair->host.scene() != hostParent);
    CHECK(pair->guest.scene() != guestParent);
    pair->settle();
    pair->synchronize();
    auto* hostChallenge = const_cast<PlayScene*>(pair->host.scene());
    auto* guestChallenge = const_cast<PlayScene*>(pair->guest.scene());
    REQUIRE(hostChallenge->challenge().totalCoins() > 0);
    REQUIRE(hostChallenge->challenge().totalCoins() == guestChallenge->challenge().totalCoins());
    for (auto* scene : {hostChallenge, guestChallenge}) {
        for (const auto seat : kSeats) {
            const_cast<PlayerActor*>(scene->actor(seat))->save().gold += 125;
            scene->awardExperience(seat, 80, true);
        }
    }
    const auto& items = hostChallenge->world()->placedItems();
    REQUIRE(items.size() == guestChallenge->world()->placedItems().size());
    for (usize index = 0; index < items.size(); ++index) {
        const auto& coin = items.item(index);
        if (coin.subtype != 1 || !coin.visible || coin.taken) {
            continue;
        }
        pair->synchronize();
        CHECK(coin.position == guestChallenge->world()->placedItems().item(index).position);
        for (auto* scene : {hostChallenge, guestChallenge}) {
            for (const auto seat : kSeats) {
                const_cast<PlayerActor*>(scene->actor(seat))->place(coin.position);
            }
        }
        pair->step();
        pair->step();
    }
    REQUIRE(hostChallenge->challenge().coinsLeft() == 0);
    REQUIRE(guestChallenge->challenge().coinsLeft() == 0);
    REQUIRE(hostChallenge->challenge().won());
    REQUIRE(guestChallenge->challenge().won());
    const auto reward = hostChallenge->challenge().rewardMask();
    REQUIRE(reward != 0);
    CHECK(guestChallenge->challenge().rewardMask() == reward);
    CHECK(pair->hostAwards == 0);
    CHECK(pair->guestAwards == 0);
    CHECK(pair->hostSaves == 0);
    CHECK(pair->guestSaves == 0);
    const auto challengeResults = hostChallenge->levelResults();
    const auto challengeEpoch = pair->hostSession.match().context().epoch;
    REQUIRE(hostChallenge->scroll().active());
    REQUIRE(guestChallenge->scroll().active());
    for (u32 frame = 0; frame < 1200 && pair->hostSession.match().context().epoch == challengeEpoch;
         ++frame) {
        // ALLCOINS is an actual native scroll: it holds the one-second won
        // countdown until both owned inputs accept and burn the message.
        SessionInputs::Frame first, second;
        first[kHostDevice].menu.select = second[kGuestDevice].menu.select = frame % 30 == 0;
        pair->step(first, second);
    }
    CHECK_FALSE(hostChallenge->scroll().active());
    CHECK_FALSE(guestChallenge->scroll().active());
    REQUIRE(pair->hostSession.match().context().transition == MatchTransition::SecretReturn);
    REQUIRE(pair->hostSession.match().context().challengeWon);
    const auto checkFrozen = [](const SceneGeometry& actual, const SceneGeometry& expected) {
        CHECK(actual.layout == expected.layout);
        CHECK(actual.objectCount == expected.objectCount);
        CHECK(actual.darken == expected.darken);
        REQUIRE(actual.objects.size() == expected.objects.size());
        for (usize index = 0; index < actual.objects.size(); ++index) {
            CAPTURE(index);
            CHECK(actual.objects[index].index == expected.objects[index].index);
            CHECK(actual.objects[index].continuity == expected.objects[index].continuity);
            CHECK(actual.objects[index].local == expected.objects[index].local);
            CHECK(actual.objects[index].alpha == expected.objects[index].alpha);
            CHECK(actual.objects[index].visible == expected.objects[index].visible);
        }
    };
    checkFrozen(hostWorld->scene().geometry(), hostFrozen);
    checkFrozen(guestWorld->scene().geometry(), guestFrozen);
    for (u32 frame = 0; frame < 3600 && (pair->host.phase() != NativeOnlinePlay::Phase::Playing ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Playing);
         ++frame) {
        pair->step();
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->host.scene() == hostParent);
    CHECK(pair->guest.scene() == guestParent);
    CHECK(hostParent->world() == hostWorld);
    CHECK(guestParent->world() == guestWorld);
    CHECK_FALSE(hostParent->secretTravel());
    CHECK_FALSE(guestParent->secretTravel());
    CHECK(consumed(*hostParent) == 1);
    CHECK(consumed(*guestParent) == 1);
    CHECK(pair->hostUnlocks == reward);
    CHECK(pair->guestUnlocks == reward);
    CHECK(pair->hostAwards == 1);
    CHECK(pair->guestAwards == 1);
    CHECK(pair->hostSaves == 1);
    CHECK(pair->guestSaves == 1);
    CHECK(pair->hostParty.localMembers()[0].save.classUnlock == reward);
    CHECK(pair->guestParty.localMembers()[0].save.classUnlock == reward);
    REQUIRE(hostParent->levelResults().size() == kSeats.size());
    REQUIRE(guestParent->levelResults().size() == kSeats.size());
    for (usize index = 0; index < kSeats.size(); ++index) {
        const auto actual = hostParent->levelResults()[index];
        CHECK(actual.totals == guestParent->levelResults()[index].totals);
        CHECK(actual.totals == challengeResults[index].totals);
        CHECK(actual.totals[0] >= parentResults[index].totals[0] + 125);
        CHECK(actual.totals[2] > parentResults[index].totals[2]);
        CHECK_FALSE(hostParent->party()[index].slot);
        CHECK_FALSE(guestParent->party()[index].slot);
    }
    for (u32 frame = 0; frame < 120; ++frame) {
        pair->step();
    }
    CHECK(pair->hostAwards == 1);
    CHECK(pair->guestAwards == 1);
    CHECK(pair->hostSaves == 1);
    CHECK(pair->guestSaves == 1);
}
TEST_CASE("native completion checkpoints save only local progress and honor Auto Save",
          "[native-online-play][native-save][native-completion][assets]") {
    (void)test::assetOrSkip("VQMOVIES/victory.avi");
    const bool autoSave = GENERATE(false, true);
    PlayOptions options;
    options.welcome = false;
    auto pair = std::make_unique<NativePair>(options, "", autoSave, "H4");
    pair->settle();
    pair->synchronize();
    for (const auto* play : {&pair->host, &pair->guest}) {
        REQUIRE(play->scene()->world()->level());
        REQUIRE(play->scene()->world()->level()->bossType == 44);
        // Prepare the same native wizard timeline immediately before its final
        // departure. The actual scene update must produce the ending boundary;
        // no Shop/ending packet or completion state is fabricated.
        auto& victory = const_cast<BossVictory&>(play->scene()->victory());
        victory.begin(44, 'H', 0, 0, false);
        for (u32 step = 0; step < 20 && victory.stage() != BossVictory::Stage::Gone; ++step) {
            victory.update(600, {});
        }
        REQUIRE(victory.stage() == BossVictory::Stage::Gone);
    }
    for (u32 frame = 0; frame < 18000 && (pair->host.phase() != NativeOnlinePlay::Phase::Shopping ||
                                          pair->guest.phase() != NativeOnlinePlay::Phase::Shopping);
         ++frame) {
        pair->step();
    }
    REQUIRE(pair->host.shopScene());
    REQUIRE(pair->guest.shopScene());
    CHECK(pair->host.shopScene()->session().visit() == ShopVisit::Completion);
    CHECK(pair->guest.shopScene()->session().visit() == ShopVisit::Completion);
    REQUIRE(pair->hostSession.match().shop().setup());
    CHECK(pair->hostSession.match().shop().setup()->ending);
    CHECK(pair->hostSaves == 0);
    CHECK(pair->guestSaves == 0);
    for (u32 frame = 0; frame < 3600 && (pair->host.phase() != NativeOnlinePlay::Phase::Playing ||
                                         pair->guest.phase() != NativeOnlinePlay::Phase::Playing);
         ++frame) {
        SessionInputs::Frame first, second;
        first[kHostDevice].menu.select = second[kGuestDevice].menu.select = frame % 30 == 0;
        pair->step(first, second);
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Playing);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Playing);
    CHECK(pair->host.scene()->world()->isTower());
    CHECK(pair->guest.scene()->world()->isTower());
    CHECK(pair->hostSaves == (autoSave ? 1U : 0U));
    CHECK(pair->guestSaves == (autoSave ? 1U : 0U));
    CHECK(pair->host.hasCompletedProgress());
    CHECK(pair->guest.hasCompletedProgress());
    REQUIRE(pair->host.saveCompletedProgress());
    REQUIRE(pair->guest.saveCompletedProgress());
    CHECK(pair->hostManualSaves == 1);
    CHECK(pair->guestManualSaves == 1);
    for (const auto& member : pair->host.scene()->party()) {
        CHECK_FALSE(member.slot);
    }
}

TEST_CASE("native GameOver finishes locally while terminal input relay remains live",
          "[native-online-play][native-finished][assets]") {
    auto pair = std::make_unique<NativePair>(immediateTower());
    pair->settle();
    pair->synchronize();
    const_cast<PlayScene*>(pair->host.scene())->startGameOver();
    const_cast<PlayScene*>(pair->guest.scene())->startGameOver();
    for (u32 frame = 0; frame < static_cast<u32>(GameOver::kDurationTicks) + 30 &&
                        (pair->host.phase() != NativeOnlinePlay::Phase::Finished ||
                         pair->guest.phase() != NativeOnlinePlay::Phase::Finished);
         ++frame) {
        pair->step();
    }
    REQUIRE(pair->host.phase() == NativeOnlinePlay::Phase::Finished);
    REQUIRE(pair->guest.phase() == NativeOnlinePlay::Phase::Finished);
    CHECK(pair->host.outcome() == NativePlayDriver::Result::GameOver);
    CHECK(pair->guest.outcome() == NativePlayDriver::Result::GameOver);
    CHECK_FALSE(pair->host.hasCompletedProgress());
    CHECK_FALSE(pair->host.pause(kHostDevice));
    const auto hostTick = pair->host.completedTick();
    const auto guestTick = pair->guest.completedTick();
    for (u32 frame = 0; frame < 10; ++frame) {
        pair->step();
    }
    CHECK(pair->host.completedTick() == hostTick);
    CHECK(pair->guest.completedTick() == guestTick);
    CHECK(pair->hostSession.phase() == OnlineSession::Phase::Active);
    CHECK(pair->guestSession.phase() == OnlineSession::Phase::Active);
}
} // namespace
