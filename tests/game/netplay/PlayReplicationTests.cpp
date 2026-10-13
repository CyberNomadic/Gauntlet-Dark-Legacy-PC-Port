#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/ClientClock.h"
#include "game/netplay/CombatReplica.h"
#include "game/netplay/OnlinePair.h"
#include "game/players/ItemPickup.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/MatchInputs.h"
#include "game/screens/PlayReplication.h"
#include "game/screens/ReplicaView.h"
#include "game/world/LevelCatalog.h"
#include "game/world/PotionEmission.h"
#include "game/world/WeaponGlow.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Result = PlayReplication::Result;

struct Wire final : PacketTransport {
    std::vector<std::vector<u8>> packets;
    SendResult send(Connection /*connection*/, std::span<const u8> bytes,
                    Delivery /*delivery*/) override {
        packets.emplace_back(bytes.begin(), bytes.end());
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection /*connection*/) const override {
        return std::nullopt;
    }
    void close(Connection /*connection*/) override {}
};
struct Pair {
    MatchSession host;
    MatchSession guest;
    Wire wire;
    Pair() {
        constexpr MatchOwners kOwners{1, 2, 0, 0};
        const std::array clients{MatchLink{2, 1}};
        const std::array server{MatchLink{1, 99}};
        REQUIRE(host.open(1, kOwners, clients));
        REQUIRE(guest.open(2, kOwners, server));
    }
    void send(MatchSession& from, MatchSession& to, PacketTransport::Connection sender,
              bool duplicate = false) {
        from.flush(wire);
        for (const auto& bytes : wire.packets) {
            const auto result = to.receive(sender, bytes);
            REQUIRE((result == MatchSession::Admission::Accepted ||
                     result == MatchSession::Admission::Stale));
            if (duplicate) {
                const auto again = to.receive(sender, bytes);
                REQUIRE((again == MatchSession::Admission::Accepted ||
                         again == MatchSession::Admission::Stale));
            }
        }
        wire.packets.clear();
    }
    void pump() {
        send(host, guest, 99);
        send(guest, host, 1);
        send(host, guest, 99);
    }
    void prepare(MatchTransition transition = MatchTransition::Start) {
        REQUIRE(host.prepare(1, transition));
        pump();
    }
    void ready() {
        REQUIRE(host.loaded());
        REQUIRE(guest.loaded());
        pump();
        REQUIRE(host.phase() == MatchSession::Phase::Running);
        REQUIRE(guest.phase() == MatchSession::Phase::Running);
    }
};
struct Scene {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene play;
    PickupResources pickups;
    FixtureResources fixtures;
    explicit Scene(std::string_view levelName = {}, bool companions = false, bool timers = false,
                   bool welcome = false, std::span<const PartyMember> selected = {}) {
        if (!levelName.empty()) {
            REQUIRE(catalog.load(root));
            const auto level = catalog.byName(levelName);
            REQUIRE(level);
            REQUIRE(world.load(device, root, *level));
        }
        GameContext context;
        context.config = &config;
        context.levels = levelName.empty() ? nullptr : &catalog;
        context.unpackedRoot = root;
        PlayOptions options;
        options.welcome = welcome;
        options.position = Vec3{3, 2, -20};
        if (timers) {
            REQUIRE(world.startPoint(0));
            options.position = world.startPoint(0)->position;
        }
        CharacterSave save;
        save.progress().health = 1000;
        if (timers) {
            save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 60);
            save.progress().relics.addShard(9);
        }
        if (companions) {
            save.progress().experience = levelExperience(80);
            save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
        }
        const std::array party{PartyMember{0, save}, PartyMember{1, save}};
        REQUIRE(play.open(device, context, world, selected.empty() ? std::span(party) : selected,
                          options));
        REQUIRE(pickups.bind(device, world.placedItems().archives()));
        auto source = world.placedItems().archives();
        std::vector<ItemArchive*> archives(source.begin(), source.end());
        if (world.realmItems().loaded()) {
            archives.push_back(&world.realmItems());
        }
        REQUIRE(
            fixtures.bind(device, archives, play.generators(), play.safeRocks(), &play.statues()));
    }
    void settle() {
        for (s32 i = 0; i < 400; ++i) {
            REQUIRE(play.update(1.0 / 60, {}) == PlayOutcome::Running);
        }
        REQUIRE_FALSE(play.spawning());
        REQUIRE_FALSE(play.animator(0)->entering());
        REQUIRE_FALSE(play.animator(1)->entering());
    }
};

TEST_CASE("remote scroll dismissal advances the host once and respects the native page hold",
          "[netplay][play-replication][assets]") {
    Scene scene({}, false, false, true);
    for (s32 i = 0; i < 600 && !scene.play.scrollLook(); ++i) {
        REQUIRE(scene.play.update(1.0 / 60, {}) == PlayOutcome::Running);
    }
    REQUIRE(scene.play.scrollLook());
    CHECK(scene.play.scrollLook()->page == 0);
    const Vec3 start = scene.play.actor(1)->position();
    Pair pair;
    PlayReplication driver;
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    const auto step = [&](const PlayInput& input) {
        SessionInputs::Frame inputs;
        inputs[0] = input;
        REQUIRE(MatchInputs::sample(pair.guest, inputs));
        pair.send(pair.guest, pair.host, 1, true);
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        REQUIRE(driver.latest()->hud);
        const auto local = scene.play.scrollLook();
        const auto& captured = driver.latest()->hud->scroll;
        REQUIRE(captured.has_value() == local.has_value());
        if (local) {
            CHECK(captured->message == local->message);
            CHECK(captured->page == local->page);
            CHECK(captured->burnFrame == local->burnFrame);
            CHECK(captured->promptAlpha == local->promptAlpha);
            CHECK(scene.play.actor(1)->position() == start);
        }
        pair.pump();
    };
    PlayInput early;
    early.menu.buttonPressed = true;
    step(early);
    PlayInput held;
    held.attack = true;
    held.move = {{1, 0}, 1};
    for (s32 tick = 0; tick < 40; ++tick) {
        step(held);
    }
    REQUIRE(scene.play.scrollLook());
    CHECK(scene.play.scrollLook()->page == 0); // The early edge is not saved for later.
    const auto pages = scene.play.scroll().pageCount();
    for (usize page = 1; page < pages; ++page) {
        PlayInput press;
        press.menu.pointerPressed = true;
        step(press);
        for (s32 tick = 0; tick < 40; ++tick) {
            step(held);
        }
        REQUIRE(scene.play.scrollLook());
        CHECK(scene.play.scrollLook()->page == page);
        CHECK(scene.play.scrollLook()->burnFrame == -1);
        REQUIRE(pair.guest.playback().latest());
        CHECK(pair.guest.playback().latest()->hud->scroll->page == page);
    }
    PlayInput dismiss;
    dismiss.menu.buttonPressed = true;
    step(dismiss);
    bool sawBurn = false;
    for (s32 tick = 0; tick < 120 && scene.play.scrollLook(); ++tick) {
        step({});
        sawBurn = sawBurn || (scene.play.scrollLook() && scene.play.scrollLook()->burnFrame >= 0);
    }
    CHECK(sawBurn);
    CHECK_FALSE(scene.play.scrollLook());
    for (s32 tick = 0; tick < 4; ++tick) {
        step({});
    }
    REQUIRE(pair.guest.playback().latest());
    CHECK_FALSE(pair.guest.playback().latest()->hud->scroll);
}

TEST_CASE("real scene ticks only on the host after the load barrier and holds across pause",
          "[netplay][play-replication][assets]") {
    std::array<CharacterProfile, 2> profiles;
    for (usize seat = 0; seat < profiles.size(); ++seat) {
        CharacterSave save;
        save.name = seat == 0 ? "HOST" : "GUEST";
        save.progress().health = 1000;
        save.progress().experience = levelExperience(80);
        save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
        const auto profile = CharacterProfile::capture(save);
        REQUIRE(profile);
        profiles[seat] = *profile;
    }
    test::OnlinePair pair(profiles);
    REQUIRE(pair.hostSession.party());
    REQUIRE(pair.guestSession.party());
    std::vector<PartyMember> members;
    for (usize seat = 0; seat < pair.hostSession.party()->size(); ++seat) {
        if (const auto& profile = (*pair.hostSession.party())[seat]) {
            members.push_back({static_cast<s32>(seat), profile->gameplayCopy()});
        }
    }
    Scene scene({}, false, false, false, members);
    scene.settle();
    PlayReplication driver;
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    CHECK_FALSE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    const Vec3 start = scene.play.actor(1)->position();
    const auto initialFrame = scene.play.animator(1)->player().frame();
    REQUIRE(pair.host.loaded());
    pair.pump();
    for (s32 i = 0; i < 60; ++i) {
        CHECK(driver.advance(pair.host, {}, resources) == Result::Held);
    }
    CHECK(scene.play.actor(1)->position() == start);
    CHECK(scene.play.animator(1)->player().frame() == initialFrame);
    CHECK(pair.host.tick() == 0);
    REQUIRE(pair.guest.loaded());
    pair.pump();

    // Independent client resources: no PlayScene exists on this side.
    LevelWorld clientWorld;
    REQUIRE(clientWorld.load(scene.device, scene.root));
    Enemies clientEnemies;
    clientEnemies.open(scene.device, scene.root, nullptr, 4, {}, 1);
    ProjectileResources clientProjectiles;
    PickupResources clientPickups;
    REQUIRE(clientPickups.bind(scene.device, clientWorld.placedItems().archives()));
    FixtureResources clientFixtures;
    const Generators clientGenerators;
    const SafeRocks clientRocks;
    const auto sources = clientWorld.placedItems().archives();
    std::vector<ItemArchive*> fixtureArchives(sources.begin(), sources.end());
    if (clientWorld.realmItems().loaded()) {
        fixtureArchives.push_back(&clientWorld.realmItems());
    }
    REQUIRE(clientFixtures.bind(scene.device, fixtureArchives, clientGenerators, clientRocks));
    ReplicaView client;
    REQUIRE(client.begin(pair.guest.context(), clientWorld, clientEnemies, clientProjectiles,
                         clientPickups, clientFixtures));
    std::array<PlayerFigure*, InputCommand::kSeats> clientFigures{};
    for (u8 seat = 0; seat < 2; ++seat) {
        const auto save = (*pair.guestSession.party())[seat]->gameplayCopy();
        auto figure = PlayerFigure::load(scene.device, scene.root, save, false);
        clientFigures[seat] = figure.get();
        REQUIRE(client.setPlayer(seat, std::move(figure), PlayerFigure::bodyScale(save, {})));
    }
    ItemArchive companionWeapons;
    REQUIRE(companionWeapons.load(scene.root / "WEAPONS"));
    TextureSet clientStatic;
    REQUIRE(clientStatic.load(scene.root / "STATIC"));
    const std::array<TextureSet*, 5> clientLenders{
        &companionWeapons.textures, &clientWorld.items().textures,
        &clientWorld.realmItems().textures, &clientWorld.powerups().textures, &clientStatic};
    REQUIRE(
        clientProjectiles.addPlayers(scene.device, companionWeapons, clientFigures, clientLenders));
    REQUIRE(clientProjectiles.addStage(
        scene.device, {&clientWorld.items(), &clientWorld.realmItems(), &clientWorld.powerups()},
        {}, clientLenders));
    REQUIRE(client.bindCompanions(scene.device, clientWorld.powerups(), &companionWeapons));
    HudResources hudResources;
    hudResources.powerups = &clientWorld.powerups();
    REQUIRE(client.loadHud(scene.device, scene.root, nullptr, hudResources));
    REQUIRE(scene.world.placeItem(scene.device, "TREAS_GOLD", scene.play.actor(0)->position()));
    const auto goldBefore = scene.play.actor(0)->save().gold;
    SessionInputs::Frame guestInput;
    guestInput[0].move = {{0, 1}, 1}; // guest device zero owns global seat one
    for (s32 i = 0; i < 40; ++i) {
        REQUIRE(MatchInputs::sample(pair.guest, guestInput));
        pair.pump();
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        CHECK(driver.latest()->motion.tick == pair.host.tick() - 1);
        CHECK(driver.latest()->motion.players[1]->position == scene.play.actor(1)->position());
        CHECK(driver.latest()->motion.camera.position == scene.play.viewCamera().position);
        for (usize seat = 0; seat < 2; ++seat) {
            REQUIRE(driver.latest()->players[seat]->companions[0]);
            REQUIRE(driver.latest()->players[seat]->companions[1]);
            REQUIRE(driver.latest()->hud);
            REQUIRE(driver.latest()->hud->players[seat]);
            CHECK(driver.latest()->hud->players[seat]->health ==
                  scene.play.actor(static_cast<s32>(seat))->save().health());
            CHECK(driver.latest()->hud->visible == scene.play.hudVisible());
        }
        pair.pump();
    }
    CHECK(glm::distance(start, scene.play.actor(1)->position()) > 1);
    CHECK(scene.play.actor(0)->save().gold > goldBefore);
    REQUIRE(driver.latest()->hud);
    REQUIRE_FALSE(driver.latest()->hud->cards.empty());
    CHECK(driver.latest()->hud->cards.back().kind == HudCardKind::Gold);
    REQUIRE(pair.guest.playback().latest());
    CHECK(pair.guest.playback().latest()->motion.tick == 39);
    ClientClock clock;
    REQUIRE(clock.begin(pair.guest));
    const auto firstShown = clock.sample(pair.guest, 0);
    REQUIRE(firstShown);
    REQUIRE(client.show(*firstShown));
    CHECK(client.actors().visiblePlayers() == 2);
    REQUIRE_FALSE(driver.latest()->pickups.empty());
    CHECK(client.pickupCount() == driver.latest()->pickups.size());
    REQUIRE_FALSE(driver.latest()->fixtures.empty());
    CHECK(client.fixtureCount() == driver.latest()->fixtures.size());
    const auto version = client.actors().playerFigure(1)->animationRevision();
    const auto captured = CombatPacket::encode(*driver.latest());
    const Mat4 projection = glm::orthoLH_ZO(0.0f, 1920.0f, 1080.0f, 0.0f, 0.0f, 1.0f);
    for (s32 draw = 0; draw < 3; ++draw) {
        scene.device.draws.clear();
        client.draw(scene.device, projection, 1920, 1080, 0);
        CHECK_FALSE(scene.device.draws.empty());
    }
    CHECK(client.actors().playerFigure(1)->animationRevision() == version);
    CHECK(clientEnemies.count() == 0);
    CHECK(clientEnemies.takeFeedback().empty());
    CHECK(CombatPacket::encode(*driver.latest()) == captured);
    guestInput = {};
    guestInput[0].attack = true;
    usize receivedShots = 0;
    for (s32 i = 0; i < 180; ++i) {
        REQUIRE(MatchInputs::sample(pair.guest, guestInput));
        pair.pump();
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        pair.pump();
        REQUIRE(pair.guest.playback().latest());
        for (s32 render = 0; render < 2; ++render) {
            const auto shown = clock.sample(pair.guest, 1.0 / 120);
            REQUIRE(shown);
            CAPTURE(i, render, clock.tick(), clock.fraction(), shown->motion.tick);
            REQUIRE(shown->valid());
            REQUIRE(shown->geometry);
            REQUIRE(client.geometry().acceptsGeometry(*shown->geometry));
            REQUIRE(client.actors().companionsReady(*shown));
            for (const auto& shot : shown->projectiles) {
                CAPTURE(shot.resource, shot.animation.frame, shot.animation.transition);
                REQUIRE(clientProjectiles.accepts(shot));
            }
            for (const auto& item : shown->pickups) {
                REQUIRE(clientPickups.accepts(item));
            }
            for (const auto& fixture : shown->fixtures) {
                REQUIRE(clientFixtures.accepts(fixture));
            }
            REQUIRE(client.show(*shown));
            receivedShots += shown->projectiles.size();
        }
    }
    CHECK(receivedShots > 0);
    const auto beforePause = CombatPacket::encode(*driver.latest());
    REQUIRE(pair.guest.requestPause());
    pair.pump();
    const auto tick = pair.host.tick();
    const auto paused = scene.play.actor(1)->position();
    for (s32 i = 0; i < 120; ++i) {
        CHECK(driver.advance(pair.host, {}, resources) == Result::Held);
    }
    CHECK(pair.host.tick() == tick);
    CHECK(scene.play.actor(1)->position() == paused);
    CHECK(CombatPacket::encode(*driver.latest()) == beforePause);
    const auto oldSnapshot = *driver.latest();
    pair.prepare(MatchTransition::Resume);
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    CHECK(driver.latest() == nullptr);
    const auto* figureBeforeResume = client.actors().playerFigure(1);
    const auto texturesBeforeResume = scene.device.texturesCreated;
    const auto heldImage = CombatPacket::encode(*client.shown());
    REQUIRE(client.resume(pair.guest.context()));
    REQUIRE(clock.begin(pair.guest));
    CHECK_FALSE(client.resume(pair.guest.context()));
    CHECK(client.actors().playerFigure(1) == figureBeforeResume);
    CHECK(CombatPacket::encode(*client.shown()) == heldImage);
    CHECK_FALSE(client.show(oldSnapshot));
    scene.device.draws.clear();
    client.draw(scene.device, projection, 1920, 1080, 0);
    CHECK_FALSE(scene.device.draws.empty());
    pair.ready();
    REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
    CHECK(driver.latest()->motion.tick == 0);
    CHECK(driver.latest()->motion.epoch > oldSnapshot.motion.epoch);
    CHECK(scene.play.actor(1)->position() == paused); // no stale held movement across resume
    pair.pump();
    REQUIRE(pair.guest.playback().latest());
    REQUIRE(client.show(*pair.guest.playback().latest()));
    CHECK(client.actors().visiblePlayers() == 2);
    receivedShots = 0;
    for (s32 i = 0; i < 60; ++i) {
        REQUIRE(MatchInputs::sample(pair.guest, guestInput));
        pair.pump();
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        pair.pump();
        const auto shown = clock.sample(pair.guest, 1.0 / 60);
        REQUIRE(shown);
        REQUIRE(client.show(*shown));
        receivedShots += client.projectileCount();
        scene.device.draws.clear();
        client.draw(scene.device, projection, 1920, 1080, 0);
    }
    CHECK(receivedShots > 0);
    CHECK(scene.device.texturesCreated == texturesBeforeResume);
    pair.host.disconnected(1);
    CHECK(driver.advance(pair.host, {}, resources) == Result::Held);
}

TEST_CASE("scene capture preserves timer priority pause and runestone visibility",
          "[netplay][play-replication][assets]") {
    for (const std::string_view level : {"G1", "S4"}) {
        CAPTURE(level);
        Scene scene(level, false, true);
        ProjectileResources resources;
        REQUIRE(scene.play.bindSceneProjectiles(resources));
        Pair pair;
        PlayReplication driver;
        pair.prepare();
        REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
        pair.ready();
        std::vector<std::vector<u8>> history;
        for (s32 tick = 0; tick < 8; ++tick) {
            REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
            const auto encoded = HudPacket::encode(*driver.latest()->hud);
            REQUIRE(encoded);
            history.push_back(*encoded);
            pair.pump();
            // Secret stages suppress Stop Time and show their own challenge
            // timer. Capture must follow the scene's selected HUD, not the item.
            CHECK(scene.play.hud().hourglassLook(scene.play.participants()).has_value() ==
                  !scene.world.ref().isSecret());
            const auto look = scene.play.hourglassLook();
            REQUIRE(look);
            REQUIRE(driver.latest()->hud->hourglass);
            CHECK(driver.latest()->hud->hourglass->elapsed == look->elapsed);
            CHECK(driver.latest()->hud->hourglass->fallingFrame == look->fallingFrame);
            const auto* received = pair.guest.playback().latest();
            REQUIRE(received);
            REQUIRE(received->motion.tick < history.size());
            CHECK(HudPacket::encode(*received->hud) ==
                  history[static_cast<usize>(received->motion.tick)]);
            if (scene.play.runeMeter().visible()) {
                CHECK(driver.latest()->hud->runeFill == scene.play.runeMeter().fill());
            } else {
                CHECK_FALSE(driver.latest()->hud->runeFill);
            }
        }
        const auto held = CombatPacket::encode(*driver.latest());
        REQUIRE(pair.guest.requestPause());
        pair.pump();
        for (s32 tick = 0; tick < 120; ++tick) {
            CHECK(driver.advance(pair.host, {}, {}) == Result::Held);
        }
        CHECK(CombatPacket::encode(*driver.latest()) == held);
        pair.prepare(MatchTransition::Resume);
        REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
        pair.ready();
        // Expire the test inventory without waiting a minute or advancing a client clock.
        for (s32 seat = 0; seat < 2; ++seat) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
            auto* actor = const_cast<PlayerActor*>(scene.play.actor(seat));
            actor->save().progress().inventory.advance(100);
        }
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        if (scene.world.ref().isSecret()) {
            REQUIRE(scene.play.challenge().state() == SecretChallenge::State::Collecting);
            REQUIRE(scene.play.spawning());
            REQUIRE(driver.latest()->hud->hourglass);
            CHECK(driver.latest()->hud->hourglass->elapsed == 0);
            CHECK(driver.latest()->hud->hourglass->fallingFrame == -1);
        } else {
            CHECK_FALSE(driver.latest()->hud->hourglass);
            REQUIRE(scene.play.runeMeter().visible());
            REQUIRE(driver.latest()->hud->runeFill);
        }
    }
}

TEST_CASE("scene binding rejects mismatched ownership clocks and non-host simulation",
          "[netplay][play-replication][assets]") {
    Scene scene;
    Pair pair;
    pair.prepare();
    PlayReplication driver;
    PlayScene closed;
    CHECK_FALSE(driver.bind(closed, pair.host, scene.pickups, scene.fixtures));
    CHECK_FALSE(driver.bind(scene.play, pair.guest, scene.pickups, scene.fixtures));
    scene.config.timing.tickRate = 30;
    CHECK_FALSE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    scene.config.timing.tickRate = 60;
    MatchSession solo;
    REQUIRE(solo.open(1, {1, 0, 0, 0}, {}));
    REQUIRE(solo.prepare(1));
    CHECK_FALSE(driver.bind(scene.play, solo, scene.pickups, scene.fixtures));
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    REQUIRE(pair.host.advance()); // another owner must not also advance the session
    CHECK(driver.advance(pair.host, {}, {}) == Result::Failed);
    CHECK(driver.failed());
    CHECK(driver.latest() == nullptr);
    driver.clear();
    CHECK_FALSE(driver.failed());
}

TEST_CASE("an unregistered real weapon fails capture instead of disappearing remotely",
          "[netplay][play-replication][assets]") {
    Scene scene;
    scene.settle();
    ProjectileResources resources;
    // Sumner is visible immediately. Register the stage so the intentional
    // missing-player-resource failure occurs only when a real weapon is born.
    REQUIRE(resources.addStage(
        scene.device, {&scene.world.items(), &scene.world.realmItems(), &scene.world.powerups()},
        {}, scene.play.effects().textureLenders()));
    REQUIRE(resources.treeId(&scene.world.items(), scene.play.sumner().tree()) != 0);
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    SessionInputs::Frame input;
    input[0].attack = true;
    input[0].attackPressed = true;
    Result result = Result::Advanced;
    for (s32 i = 0; i < 180 && result == Result::Advanced; ++i) {
        result = driver.advance(pair.host, input, resources);
        input[0].attackPressed = false;
    }
    REQUIRE(scene.play.missiles().count() > 0);
    REQUIRE(result == Result::Failed);
    const auto tick = pair.host.tick();
    CHECK(driver.advance(pair.host, input, resources) == Result::Failed);
    CHECK(pair.host.tick() == tick);
}

TEST_CASE("remote continuity uses the same teleport cutoff as local figures",
          "[netplay][play-replication]") {
    PlayerRuntime player;
    player.actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    player.previous.continuous = true;
    player.previous.position = {0.1f, 0, 0};
    CHECK(PartyFigures::presentationContinuous(player));
    player.previous.position = {100, 0, 0};
    CHECK_FALSE(PartyFigures::presentationContinuous(player));
    player.previous.position = player.actor.position();
    player.previous.continuous = false;
    CHECK_FALSE(PartyFigures::presentationContinuous(player));
}

TEST_CASE("entrance camera cuts remain marked when snapshots are sent less often than ticks",
          "[netplay][play-replication][assets]") {
    Scene scene;
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    u32 previous = 0;
    for (s32 tick = 0; tick < 7; ++tick) {
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        if (tick > 0) {
            CHECK(driver.latest()->motion.cameraContinuity ==
                  previous + (scene.play.cameraContinuous() ? 0 : 1));
        }
        REQUIRE(driver.latest()->hud);
        CHECK(driver.latest()->hud->screen.titleScale == scene.play.arrival().titleScale());
        CHECK(driver.latest()->hud->screen.transitionOpacity == scene.play.transition().opacity());
        CHECK(driver.latest()->hud->screen.cinematicBars == scene.play.cinematicBars());
        previous = driver.latest()->motion.cameraContinuity;
        pair.pump();
        if (tick % 3 == 0) {
            REQUIRE(pair.guest.playback().latest());
            CHECK(pair.guest.playback().latest()->motion.cameraContinuity == previous);
            CHECK(pair.guest.playback().latest()->motion.tick == static_cast<u64>(tick));
        }
    }
    REQUIRE(pair.guest.requestPause());
    pair.pump();
    const auto held = CombatPacket::encode(*driver.latest());
    for (s32 tick = 0; tick < 120; ++tick) {
        CHECK(driver.advance(pair.host, {}, {}) == Result::Held);
        CHECK(CombatPacket::encode(*driver.latest()) == held);
    }
}
TEST_CASE("a loaded cemetery scene publishes its dormant statues through the real host bridge",
          "[netplay][play-replication][replica-statues][assets]") {
    Scene scene("G4");
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    REQUIRE(scene.play.statues().count() > 0);
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    for (s32 tick = 0; tick < 4; ++tick) {
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        const auto& state = *driver.latest();
        CHECK(std::ranges::count_if(state.fixtures, [](const auto& fixture) {
                  return fixture.source == FixtureSource::Statue;
              }) == static_cast<std::ptrdiff_t>(scene.play.statues().count()));
        pair.pump();
        if (tick % 3 == 0) {
            REQUIRE(pair.guest.playback().latest());
            CHECK(CombatPacket::encode(*pair.guest.playback().latest()) ==
                  CombatPacket::encode(state));
        }
    }
}

TEST_CASE(
    "native G1 capture grants stationary melee context only outside world and equipment locks",
    "[netplay][play-replication][melee-prediction][assets]") {
    CharacterSave save;
    save.progress().health = 1000;
    const std::array party{PartyMember{0, save}, PartyMember{1, save}};
    // The fixture's timers option selects the authored start. Explicit profiles
    // retain ordinary equipment instead of its optional Stop Time inventory.
    Scene scene("G1", false, true, false, party);
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    const auto step = [&] {
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        pair.pump();
    };
    step();
    REQUIRE(scene.play.spawning());
    REQUIRE(driver.latest()->players[1]);
    CHECK_FALSE(driver.latest()->players[1]->movement.current);
    CHECK_FALSE(driver.latest()->players[1]->melee);

    for (s32 tick = 0; tick < 1200 && !driver.latest()->players[1]->melee; ++tick) {
        step();
    }
    REQUIRE(driver.latest()->players[1]->melee);
    const auto& player = *driver.latest()->players[1];
    REQUIRE(player.rollback);
    CHECK(player.movement.current);
    CHECK(player.movement.enabled);
    CHECK(player.melee->selection.feet == scene.play.actor(1)->position());
    CHECK(player.melee->selection.reach == scene.play.actor(1)->reach());
    const auto bytes = CombatPacket::encode(*driver.latest());
    REQUIRE(bytes);
    const auto decoded = CombatPacket::decode(*bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->players[1]->melee);
    CHECK(PlayerMeleeCheckpoint::encode(*decoded->players[1]->melee) ==
          PlayerMeleeCheckpoint::encode(*player.melee));

    scene.config.combat.autoMelee = true;
    step();
    CHECK_FALSE(driver.latest()->players[1]->melee);
    scene.config.combat.autoMelee = false;
    step();
    REQUIRE(driver.latest()->players[1]->melee);
    // An item can replace both attack choice and animation speed. The local
    // player must wait for its authoritative action rather than reuse old melee.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto* actor = const_cast<PlayerActor*>(scene.play.actor(1));
    actor->save().progress().inventory.addPowerup(powerup::kWeapon, powerup::kSuperShot, 0, 60);
    step();
    CHECK_FALSE(driver.latest()->players[1]->melee);
}
TEST_CASE(
    "native host certifies reaction slides without advancing gameplay or admitting unsafe bodies",
    "[netplay][play-replication][knockback-prediction][assets]") {
    const bool speed = GENERATE(false, true);
    CharacterSave save;
    save.progress().health = 1000;
    const std::array party{PartyMember{0, save}, PartyMember{1, save}};
    Scene scene("G1", false, true, false, party);
    scene.settle();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[1]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& other = const_cast<PlayerRuntime&>(scene.play.participants()[0]);
    other.life = PlayerLife::InTower;
    if (speed) {
        runtime.actor.save().progress().inventory.addPowerup(powerup::kSpeed, 0, 2, -1);
    }
    runtime.reaction = PlayerDeed::Flinch;
    runtime.knockback.queue({0.25f, 0, 0}, Knockback::kKnockBack, 10);
    const auto impulse = runtime.knockback.checkpoint();
    const auto action = runtime.figure->animator().checkpoint();
    const Vec3 position = runtime.actor.position();
    const auto inventory = runtime.actor.save().progress().inventory;
    const auto health = runtime.actor.save().health();
    const usize missiles = scene.play.missiles().count();
    const usize effects = scene.play.effects().count();
    const u32 prefix = scene.play.knockbackPreview(1);
    REQUIRE(prefix > 0);
    CHECK(prefix <= PlayerCombatState::kKnockbackTicks);
    for (s32 repeat = 0; repeat < 4; ++repeat) {
        CHECK(scene.play.knockbackPreview(1) == prefix);
    }
    CHECK(runtime.knockback.checkpoint() == impulse);
    CHECK(runtime.reaction == PlayerDeed::Flinch);
    CHECK(runtime.figure->animator().checkpoint() == action);
    CHECK(runtime.actor.position() == position);
    CHECK(runtime.actor.save().health() == health);
    CHECK(runtime.actor.save().progress().inventory == inventory);
    CHECK(scene.play.missiles().count() == missiles);
    CHECK(scene.play.effects().count() == effects);

    scene.config.combat.autoMelee = true;
    CHECK(scene.play.knockbackPreview(1) == 0);
    scene.config.combat.autoMelee = false;
    runtime.actor.save().progress().inventory.addPowerup(powerup::kArmor, powerup::kReflectShield,
                                                         0, 60);
    CHECK(scene.play.knockbackPreview(1) == 0);
    runtime.actor.save().progress().inventory = inventory;
    runtime.gesture = PlayerDeed::Pick;
    CHECK(scene.play.knockbackPreview(1) == 0);
    runtime.gesture = PlayerDeed::None;
    runtime.reaction = PlayerDeed::FallBack;
    CHECK(scene.play.knockbackPreview(1) == 0);
    runtime.reaction = PlayerDeed::Flinch;
    runtime.knockback.queue({0, 0, 1}, Knockback::kKnockDown, 10);
    CHECK(scene.play.knockbackPreview(1) == 0);
    REQUIRE(runtime.knockback.restore(impulse));
    // A fixture can push the feet onto static ground before FloorRiding has
    // released the preceding moving support. Either retained marker is unsafe.
    const auto floor = runtime.floor;
    runtime.floor.placement = Mat4{1};
    CHECK(scene.play.knockbackPreview(1) == 0);
    runtime.floor = floor;
    REQUIRE(scene.play.knockbackPreview(1) == prefix);
    runtime.floor.flags |= FloorRiding::kMoving;
    CHECK(scene.play.knockbackPreview(1) == 0);
    runtime.floor = floor;
    REQUIRE(scene.play.knockbackPreview(1) == prefix);
    const Vec3 otherPosition = other.actor.position();
    other.life = PlayerLife::Standing;
    other.actor.place(runtime.actor.position());
    CHECK(scene.play.knockbackPreview(1) == 0);
    other.actor.place(otherPosition);
    other.life = PlayerLife::InTower;
    REQUIRE(scene.play.knockbackPreview(1) == prefix);

    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
    REQUIRE(driver.latest());
    const auto& player = *driver.latest()->players[1];
    REQUIRE(player.knockback);
    CHECK(*player.knockback == runtime.knockback.checkpoint());
    CHECK(player.reaction == runtime.reaction);
    CHECK(player.knockbackTicks > 0);
    CHECK(player.movement.enabled);
    CHECK(player.movement.current);
    CHECK(player.modifiers.has_value() == speed);
    if (player.modifiers) {
        CHECK(player.modifiers->speed);
    }
    CHECK_FALSE(player.movement.ranged);
    CHECK_FALSE(player.melee);
    pair.pump();
    REQUIRE(pair.guest.playback().latest());
    const auto& received = *pair.guest.playback().latest()->players[1];
    CHECK(received.knockback == player.knockback);
    CHECK(received.reaction == player.reaction);
    CHECK(received.knockbackTicks == player.knockbackTicks);
}
TEST_CASE("native G1 captures moving melee geometry without replaying combat side effects",
          "[netplay][play-replication][moving-melee][assets]") {
    CharacterSave save;
    save.progress().health = 1000;
    const std::array party{PartyMember{0, save}, PartyMember{1, save}};
    Scene scene("G1", false, true, false, party);
    scene.settle();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[1]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& other = const_cast<PlayerRuntime&>(scene.play.participants()[0]);
    other.life = PlayerLife::InTower;
    auto& enemies = scene.play.enemies();
    REQUIRE(enemies.loadKind(kGruntKind));
    const Vec3 position = runtime.actor.position();
    const auto enemy = enemies.spawn(
        {.kind = kGruntKind,
         .position = position + runtime.actor.facing() *
                                    (runtime.actor.reach() + 2.5f + enemyKind(kGruntKind).radius),
         .placed = true},
        {});
    REQUIRE(enemy);
    // Authored barrel 19 is inside the starting area's all-bearing acquisition
    // range. Mixed item/creature contact is deliberately outside this lane.
    const auto mixed = scene.play.meleePreview(1);
    REQUIRE(mixed);
    CHECK_FALSE(mixed->moving);
    // Keep real G1 geometry, native enemies and the host bridge, while isolating
    // the admitted ordinary-cylinder encounter from unrelated barrel targeting.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& barrels = const_cast<Breakables&>(scene.play.barrels());
    REQUIRE(barrels.size() > 0);
    barrels.clear();
    const auto preview = scene.play.meleePreview(1);
    REQUIRE(preview);
    REQUIRE(preview->moving);
    CHECK(preview->moving->origin == position);
    CHECK(preview->moving->ticks == PlayerMeleeState::kMaxTicks);
    REQUIRE_FALSE(preview->moving->targets.empty());
    const auto checkpoint = runtime.figure->animator().checkpoint();
    const auto impulse = runtime.knockback.checkpoint();
    const auto enemyHealth = enemies.healthOf(*enemy);
    const auto inventory = runtime.actor.save().progress().inventory;
    const usize missiles = scene.play.missiles().count();
    const usize effects = scene.play.effects().count();
    for (s32 repeat = 0; repeat < 4; ++repeat) {
        const auto again = scene.play.meleePreview(1);
        REQUIRE(again);
        REQUIRE(again->moving);
        CHECK(PlayerMeleeCheckpoint::encode(*again) == PlayerMeleeCheckpoint::encode(*preview));
    }
    CHECK(runtime.actor.position() == position);
    CHECK(runtime.figure->animator().checkpoint() == checkpoint);
    CHECK(runtime.knockback.checkpoint() == impulse);
    CHECK(enemies.healthOf(*enemy) == enemyHealth);
    CHECK(runtime.actor.save().progress().inventory == inventory);
    CHECK(scene.play.missiles().count() == missiles);
    CHECK(scene.play.effects().count() == effects);

    const auto movingDenied = [&] {
        const auto denied = scene.play.meleePreview(1);
        CHECK((!denied || !denied->moving));
    };
    other.life = PlayerLife::Standing;
    other.actor.place(position);
    movingDenied();
    other.life = PlayerLife::InTower;
    const auto floor = runtime.floor;
    runtime.floor.placement = Mat4{1};
    movingDenied();
    runtime.floor = floor;
    runtime.floor.flags |= FloorRiding::kMoving;
    movingDenied();
    runtime.floor = floor;
    runtime.gesture = PlayerDeed::Pick;
    movingDenied();
    runtime.gesture = PlayerDeed::None;
    runtime.reaction = PlayerDeed::Flinch;
    movingDenied();
    runtime.reaction = PlayerDeed::None;
    scene.config.combat.autoMelee = true;
    movingDenied();
    scene.config.combat.autoMelee = false;
    runtime.actor.save().progress().inventory.addPowerup(powerup::kArmor, powerup::kReflectShield,
                                                         0, 60);
    movingDenied();
    runtime.actor.save().progress().inventory = inventory;
    const auto restored = scene.play.meleePreview(1);
    REQUIRE(restored);
    REQUIRE(restored->moving);

    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    SessionInputs::Frame input;
    input[0].move = {{0, 1}, 1};
    input[0].aimDirection = runtime.actor.facing();
    input[0].attack = true;
    input[0].attackPressed = true;
    REQUIRE(MatchInputs::sample(pair.guest, input));
    REQUIRE(pair.guest.sampledInputs().size() == 1);
    const auto sampled = pair.guest.sampledInputs().front();
    REQUIRE(sampled.seat == 1);
    pair.send(pair.guest, pair.host, 1);
    // Guest input is scheduled ahead. Reach its actual resolution deadline;
    // receipt of a future command is not evidence that gameplay consumed it.
    do {
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
    } while (driver.latest()->motion.tick < sampled.tick);
    REQUIRE(driver.latest());
    REQUIRE(driver.latest()->players[1]);
    CHECK(runtime.actor.moving());
    CHECK(runtime.figure->animator().lunging());
    const auto& received = *driver.latest()->players[1];
    REQUIRE(received.rollbackInput);
    CHECK(received.rollbackInput->tick == sampled.tick);
    CHECK(received.rollbackInput->grant == sampled.grant);
    CHECK(received.rollbackInput->heldButtons == sampled.heldButtons);
    CHECK(received.rollbackInput->aimDirection == sampled.aimDirection);
    REQUIRE(received.melee);
    REQUIRE(received.melee->moving);
    CHECK(received.movement.current);
    CHECK(received.movement.enabled);
    CHECK_FALSE(received.movement.ranged);
    // A certified turbo context may carry its single-volley weapon descriptor
    // even while ordinary ranged input is not authorized near a melee target.
    CHECK(received.weapon.has_value() == received.turbo.has_value());
    CHECK(received.melee->moving->origin == runtime.actor.position());
    const auto bytes = CombatPacket::encode(*driver.latest());
    REQUIRE(bytes);
    const auto decoded = CombatPacket::decode(*bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->players[1]->melee);
    CHECK(PlayerMeleeCheckpoint::encode(*decoded->players[1]->melee) ==
          PlayerMeleeCheckpoint::encode(*received.melee));
}
TEST_CASE(
    "native host grants supported turbo and shove context without spending or emitting attacks",
    "[netplay][play-replication][turbo-prediction][shove-prediction][assets]") {
    const bool charge = GENERATE(false, true);
    const f32 speedBonus = GENERATE(0.0f, 2.0f, 20.0f);
    CharacterSave save;
    save.progress().health = 1000;
    const std::array party{PartyMember{0, save}, PartyMember{1, save}};
    Scene scene("G1", false, true, false, party);
    scene.settle();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[1]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& other = const_cast<PlayerRuntime&>(scene.play.participants()[0]);
    other.life = PlayerLife::InTower;
    // G1's nearby barrel is a mixed target; prediction must not certify it
    // before the fixture isolates the supported native target families.
    CHECK_FALSE(scene.play.turboGeometry(1));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& barrels = const_cast<Breakables&>(scene.play.barrels());
    REQUIRE(barrels.size() > 0);
    barrels.clear();
    const f32 cachedSpeed = runtime.actor.speed();
    if (speedBonus != 0) {
        runtime.actor.save().progress().inventory.addPowerup(powerup::kSpeed, 0, speedBonus, -1);
    }
    runtime.turbo.reset();
    const f32 initialMeter = charge ? TurboMeter::kShoveFrom : TurboMeter::kStrongCost;
    runtime.turbo.add(initialMeter);
    const auto geometry = scene.play.turboGeometry(1);
    REQUIRE(geometry);
    REQUIRE(geometry->moving);
    const f32 expectedSpeed =
        std::clamp(cachedSpeed + speedBonus, PlayerActor::kMinSpeed, PlayerActor::kMaxSpeed);
    CHECK(geometry->moving->radius ==
          std::min(PlayerMeleeState::kMaxRadius,
                   1.5f * expectedSpeed * static_cast<f32>(geometry->moving->ticks) / 60 + 0.01f));
    CHECK(runtime.actor.speed() == cachedSpeed);
    REQUIRE(scene.play.shovePredictable(1, *geometry));
    // The combined native acquisition query reserves [1000, 2000) for
    // enemies and [2000, 3000) for generators; fixture/critter IDs are excluded.
    constexpr s32 kFirstSupportedTarget = 1000;
    constexpr s32 kEndSupportedTargets = 3000;
    for (const auto& target : geometry->moving->targets) {
        CHECK(target.id >= kFirstSupportedTarget);
        CHECK(target.id < kEndSupportedTargets);
    }
    const auto checkpoint = runtime.figure->animator().checkpoint();
    const auto inventory = runtime.actor.save().progress().inventory;
    const auto health = runtime.actor.save().progress().health;
    const auto state = PlayerTurboState{runtime.turbo.checkpoint(), runtime.move.checkpoint()};
    const auto missiles = scene.play.missiles().count();
    const auto effects = scene.play.effects().count();
    for (s32 repeat = 0; repeat < 5; ++repeat) {
        const auto again = scene.play.turboGeometry(1);
        REQUIRE(again);
        CHECK(PlayerMeleeCheckpoint::encode(*again) == PlayerMeleeCheckpoint::encode(*geometry));
        REQUIRE(scene.play.turboPreview(1));
        CHECK(*scene.play.turboPreview(1) == state);
        CHECK(scene.play.shovePredictable(1, *again));
    }
    CHECK(runtime.figure->animator().checkpoint() == checkpoint);
    CHECK(runtime.actor.save().progress().inventory == inventory);
    CHECK(runtime.actor.save().progress().health == health);
    CHECK(runtime.turbo.checkpoint() == state.meter);
    CHECK(runtime.move.checkpoint() == state.move);
    CHECK(scene.play.missiles().count() == missiles);
    CHECK(scene.play.effects().count() == effects);

    // Native charge hits raw standing fixtures even if they are nonsolid or
    // absent from ordinary acquisition (SafeRocks). Do not infer ram safety
    // from the already accepted movement boxes alone.
    const auto ramDirectory = test::scratchDirectory("shove-capture-fixtures");
    writeTextFile(ramDirectory / "world.json", R"({
      "objects": [{"name":"GROUND","position":[0,0,0],"next":-1,"child":-1}],
      "itemInfos": [
        {"type":10,"subtype":43,"name":"BARREL","radius":1,"height":3,
         "collisionType":1,"hitPoints":10},
        {"type":10,"subtype":41,"name":"ROCK","radius":1,"height":3,
         "collisionType":1,"hitPoints":10}],
      "itemInstances": [
        {"info":0,"minPlayers":1,"position":[0,0,0],"rotation":[0,0,0],
         "params":[0,0,3,0,0,0,0,0,0,0,0,0]},
        {"info":1,"minPlayers":1,"position":[0,0,0],"rotation":[0,0,0],
         "params":[41,0,3,0,0,0,0,0,0,0,0,0]}]
    })");
    WorldLayout ramLayout;
    REQUIRE(ramLayout.load(ramDirectory));
    ItemArchive missing;
    REQUIRE(barrels.bind(scene.device, ramLayout, missing, nullptr));
    REQUIRE(barrels.standing(0));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned test fixture
    auto& ramBarrel = const_cast<Breakables::Barrel&>(barrels.barrel(0));
    ramBarrel.box.centre = runtime.actor.position();
    ramBarrel.box.solid = false;
    CHECK_FALSE(scene.play.shovePredictable(1, *geometry));
    const f32 outside =
        geometry->moving->radius + runtime.actor.radius() + PlayerAttacks::kRamReach + 20;
    ramBarrel.box.centre += Vec3{outside, 0, 0};
    CHECK(scene.play.shovePredictable(1, *geometry));
    barrels.clear();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned test fixture
    auto& rocks = const_cast<SafeRocks&>(scene.play.safeRocks());
    REQUIRE(rocks.bind(scene.device, ramLayout, missing));
    REQUIRE(rocks.standing(0));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned test fixture
    auto& ramRock = const_cast<SafeRocks::Rock&>(rocks.rock(0));
    ramRock.obstacle.centre = runtime.actor.position() + Vec3{0, 100, 0};
    ramRock.obstacle.solid = false;
    CHECK_FALSE(scene.play.shovePredictable(1, *geometry));
    ramRock.obstacle.centre += Vec3{outside, 0, 0};
    CHECK(scene.play.shovePredictable(1, *geometry));
    rocks.clear();
    CHECK_FALSE(scene.play.shovePredictable(-1, *geometry));
    auto invalidRegion = *geometry;
    invalidRegion.moving.reset();
    CHECK_FALSE(scene.play.shovePredictable(1, invalidRegion));
    invalidRegion = *geometry;
    invalidRegion.selection.feet += Vec3{1, 0, 0};
    invalidRegion.moving->origin = invalidRegion.selection.feet;
    CHECK_FALSE(scene.play.shovePredictable(1, invalidRegion));

    runtime.actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 60);
    CHECK_FALSE(scene.play.turboGeometry(1));
    runtime.actor.save().progress().inventory = inventory;
    scene.config.combat.autoMelee = true;
    CHECK_FALSE(scene.play.turboGeometry(1));
    scene.config.combat.autoMelee = false;
    const auto floor = runtime.floor;
    runtime.floor.placement = Mat4{1};
    CHECK_FALSE(scene.play.turboGeometry(1));
    runtime.floor = floor;
    runtime.gesture = PlayerDeed::Pick;
    CHECK_FALSE(scene.play.turboGeometry(1));
    runtime.gesture = PlayerDeed::None;
    REQUIRE(scene.play.turboGeometry(1));

    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    SessionInputs::Frame input;
    input[0].turboAttackPressed = !charge;
    input[0].chargePressed = charge;
    REQUIRE(MatchInputs::sample(pair.guest, input));
    REQUIRE(pair.guest.sampledInputs().size() == 1);
    const auto sampled = pair.guest.sampledInputs().front();
    pair.send(pair.guest, pair.host, 1);
    f32 beforeStepMeter = runtime.turbo.held();
    do {
        beforeStepMeter = runtime.turbo.held();
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
    } while (driver.latest()->motion.tick < sampled.tick);
    REQUIRE(runtime.figure->animator().action() ==
            (charge ? PlayerAnimator::Action::Shove : PlayerAnimator::Action::TurboStrong));
    const auto& captured = *driver.latest()->players[1];
    REQUIRE(captured.turbo);
    REQUIRE(captured.turboGeometry);
    REQUIRE(captured.turboGeometry->moving);
    CHECK(captured.turbo->meter == runtime.turbo.checkpoint());
    CHECK(captured.turbo->move == runtime.move.checkpoint());
    // Warrior B's first damaging native row starts at frame zero, so the
    // first post-step checkpoint already records its payment.
    CHECK(captured.turbo->move.owed == 0);
    CHECK(captured.turbo->meter.held < beforeStepMeter);
    CHECK(captured.shove);
    if (charge) {
        CHECK(captured.turbo->move == TurboMove::Checkpoint{});
        CHECK(runtime.rammed.empty());
    }
    CHECK(captured.movement.enabled);
    CHECK(captured.movement.current);
    CHECK_FALSE(captured.movement.ranged);
    REQUIRE(captured.weapon);
    CHECK(resources.acceptsWeapon(*captured.weapon));
    const auto bytes = CombatPacket::encode(*driver.latest());
    REQUIRE(bytes);
    const auto decoded = CombatPacket::decode(*bytes);
    REQUIRE(decoded);
    CHECK(decoded->players[1]->turbo == captured.turbo);
    CHECK(decoded->players[1]->shove == captured.shove);
    REQUIRE(decoded->players[1]->turboGeometry);
    CHECK(PlayerMeleeCheckpoint::encode(*decoded->players[1]->turboGeometry) ==
          PlayerMeleeCheckpoint::encode(*captured.turboGeometry));
}
TEST_CASE("weapon modifier certification stops before effective inventory changes without mutation",
          "[netplay][play-replication][weapon-modifiers][assets]") {
    const auto level = GENERATE(std::string_view{}, std::string_view{"G1"}, std::string_view{"G5"});
    Scene scene(level);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[1]);
    auto& inventory = runtime.actor.save().progress().inventory;
    inventory = {};
    CHECK_FALSE(scene.play.weaponModifiers(1));
    CHECK_FALSE(scene.play.weaponModifiers(-1));
    for (const u32 flags :
         {powerup::kRapidFire, powerup::kThreeWayShot, powerup::kFiveWayShot,
          PlayerMissiles::kPlainModifiers, 1U, 2U, 3U, 4U, PlayerMissiles::kPlainModifiers | 4U}) {
        for (const f32 duration : {0.001f, 0.04f, 0.2f, -1.0f}) {
            CAPTURE(level, flags, duration);
            inventory.powerups[0] = {duration, powerup::kWeapon, 0, flags, true};
            const auto before = inventory;
            const auto modifiers = scene.play.weaponModifiers(1);
            REQUIRE(modifiers);
            CHECK(modifiers->flags == flags);
            auto oracle = before;
            f32 rate = 1;
            if (level.empty()) {
                rate = 0;
            } else if (level == "G5") {
                rate = 3;
            }
            u32 ticks = 0;
            for (; ticks < PlayerWeaponModifiers::kMaxTicks; ++ticks) {
                oracle.advance((1.0f / 60) * rate);
                if (PowerupEffects::of(oracle).weapon != flags) {
                    break;
                }
            }
            CHECK(modifiers->ticks == ticks);
            CHECK(inventory == before);
            const auto preview = scene.play.weaponPreview(1);
            REQUIRE(preview);
            CHECK(preview->shots == PowerupEffects::of(inventory).shots());
        }
    }
    inventory = {};
    inventory.powerups[0] = {0.001f, powerup::kWeapon, 0, powerup::kRapidFire, true};
    inventory.powerups[1] = {-1, powerup::kWeapon, 0, powerup::kRapidFire, true};
    inventory.powerups[2] = {30, powerup::kWeapon, 0, powerup::kSuperShot, false};
    const auto retained = scene.play.weaponModifiers(1);
    REQUIRE(retained);
    CHECK(retained->ticks == PlayerWeaponModifiers::kMaxTicks);
    REQUIRE(scene.play.weaponPreview(1));
    inventory.powerups[2].on = true;
    CHECK_FALSE(scene.play.weaponModifiers(1));
    CHECK_FALSE(scene.play.weaponPreview(1));
    CHECK_FALSE(scene.play.rangedPredictable(1));
    CHECK_FALSE(scene.play.meleePreview(1));
    CHECK_FALSE(scene.play.turboGeometry(1));
    inventory.powerups[2].on = false;
    inventory.powerups[0].on = false;
    inventory.powerups[1].on = false;
    CHECK_FALSE(scene.play.weaponModifiers(1));
    REQUIRE(scene.play.weaponPreview(1));
    CHECK(scene.play.weaponPreview(1)->shots == 1);
}

TEST_CASE("host captures rapid spread weapons with stable native fan identities",
          "[netplay][play-replication][weapon-modifiers][assets]") {
    const u32 flags = GENERATE(powerup::kRapidFire, powerup::kThreeWayShot, powerup::kFiveWayShot,
                               PlayerMissiles::kPlainModifiers);
    Scene scene;
    scene.settle();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[1]);
    auto& inventory = runtime.actor.save().progress().inventory;
    inventory = {};
    inventory.addPowerup(powerup::kWeapon, flags, 0, 30);
    const auto before = inventory;
    const auto expectedShots = static_cast<usize>(PowerupEffects::of(inventory).shots());
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    SessionInputs::Frame input;
    input[0].attack = true;
    input[0].attackPressed = true;
    for (s32 tick = 0; tick < 180 && scene.play.missiles().count() == 0; ++tick) {
        REQUIRE(MatchInputs::sample(pair.guest, input));
        pair.pump();
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        pair.pump();
        input[0].attackPressed = false;
    }
    REQUIRE(scene.play.missiles().count() == expectedShots);
    REQUIRE(driver.latest());
    const auto& captured = *driver.latest();
    REQUIRE(captured.players[1]);
    const auto& player = *captured.players[1];
    REQUIRE(player.modifiers);
    CHECK(player.modifiers->flags == flags);
    CHECK(player.modifiers->ticks == PlayerWeaponModifiers::kMaxTicks);
    REQUIRE(player.weapon);
    CHECK(player.weapon->shots == expectedShots);
    REQUIRE(player.rollback);
    CHECK(player.rollback->rapid == ((flags & powerup::kRapidFire) != 0));
    CHECK(inventory == before);
    std::array<bool, 5> ordinals{};
    for (const auto& shot : captured.projectiles) {
        if (!shot.rollback) {
            continue;
        }
        const auto& origin = shot.rollback->origin;
        CHECK(origin.seat == 1);
        CHECK(origin.tick == captured.motion.tick);
        CHECK(origin.generation == player.rollback->animation.generation);
        REQUIRE(origin.ordinal < expectedShots);
        ordinals[origin.ordinal] = true;
    }
    CHECK(static_cast<usize>(std::ranges::count(ordinals, true)) == expectedShots);
    const auto bytes = CombatPacket::encode(captured);
    REQUIRE(bytes);
    const auto decoded = CombatPacket::decode(*bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->players[1]->modifiers);
    CHECK(decoded->players[1]->modifiers->flags == flags);
    CHECK(decoded->players[1]->weapon->shots == expectedShots);
}

TEST_CASE("Speed certification tracks native pace sums and animation-only cheat timing",
          "[netplay][play-replication][weapon-modifiers][speed-prediction][assets]") {
    const auto level = GENERATE(std::string_view{}, std::string_view{"G1"}, std::string_view{"G5"});
    Scene scene(level);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[1]);
    auto& inventory = runtime.actor.save().progress().inventory;
    inventory = {};
    runtime.actor.setPaceBonus(0);
    const f32 unboosted = runtime.actor.speed();
    f32 rate = 1;
    if (level.empty()) {
        rate = 0;
    } else if (level == "G5") {
        rate = 3;
    }
    const auto verify = [&] {
        const auto before = inventory;
        const auto worn = PowerupEffects::of(before);
        const bool speed = (worn.special & powerup::kSpeedBoost) != 0;
        const auto modifiers = scene.play.weaponModifiers(1);
        REQUIRE(modifiers);
        CHECK(modifiers->flags == worn.weapon);
        CHECK(modifiers->speed == speed);
        auto oracle = before;
        u32 ticks = 0;
        for (; ticks < PlayerWeaponModifiers::kMaxTicks; ++ticks) {
            oracle.advance((1.0f / 60) * rate);
            const auto next = PowerupEffects::of(oracle);
            if (next.weapon != worn.weapon || next.paceAdd != worn.paceAdd ||
                ((next.special & powerup::kSpeedBoost) != 0) != speed) {
                break;
            }
        }
        CHECK(modifiers->ticks == ticks);
        CHECK(scene.play.itemsPredictable(1));
        CHECK(scene.play.predictionSpeed(1) ==
              std::clamp(unboosted + worn.paceAdd, PlayerActor::kMinSpeed, PlayerActor::kMaxSpeed));
        CHECK(runtime.actor.speed() == unboosted);
        CHECK(inventory == before);
        return modifiers->ticks;
    };
    for (const f32 duration : {0.001f, 0.04f, 0.2f, -1.0f}) {
        for (const f32 charge : {0.0f, 2.0f, 20.0f}) {
            CAPTURE(level, duration, charge);
            inventory = {};
            inventory.powerups[0] = {duration, powerup::kSpeed, charge, 0, true};
            verify();
            inventory.powerups[1] = {-1, powerup::kWeapon, 0, PlayerMissiles::kPlainModifiers,
                                     true};
            verify();
        }
    }
    // Losing one duplicate can leave Speed active while changing movement pace.
    inventory = {};
    inventory.powerups[0] = {0.04f, powerup::kSpeed, 2, 0, true};
    inventory.powerups[1] = {-1, powerup::kSpeed, 1, 0, true};
    const auto shortened = verify();
    if (!level.empty()) {
        CHECK(shortened < PlayerWeaponModifiers::kMaxTicks);
    }
    // A zero-bonus duplicate may expire without changing either effective input.
    inventory.powerups[0].charge = 0;
    CHECK(verify() == PlayerWeaponModifiers::kMaxTicks);
    inventory = {};
    // XSPEED's special-kind charge does not enter SetPlayerVars' pace sum.
    inventory.powerups[0] = {0.04f, powerup::kSpecial, 4, powerup::kSpeedBoost, true};
    verify();
    CHECK(scene.play.predictionSpeed(1) == unboosted);
    inventory.powerups[0].on = false;
    CHECK_FALSE(scene.play.weaponModifiers(1));
    CHECK(scene.play.itemsPredictable(1));
    CHECK_FALSE(scene.play.predictionSpeed(-1));
    for (const f32 invalid :
         {std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity()}) {
        inventory = {};
        inventory.powerups[0] = {invalid, powerup::kSpeed, 2, 0, true};
        CHECK_FALSE(scene.play.weaponModifiers(1));
        CHECK_FALSE(scene.play.itemsPredictable(1));
        CHECK_FALSE(scene.play.predictionSpeed(1));
        inventory.powerups[0] = {-1, powerup::kSpeed, invalid, 0, true};
        CHECK_FALSE(scene.play.weaponModifiers(1));
        CHECK_FALSE(scene.play.itemsPredictable(1));
        CHECK_FALSE(scene.play.predictionSpeed(1));
        inventory.powerups[0] = {invalid, powerup::kSpecial, 4, powerup::kSpeedBoost, true};
        CHECK_FALSE(scene.play.weaponModifiers(1));
        CHECK_FALSE(scene.play.predictionSpeed(1));
    }
}

TEST_CASE("Speed pickup capture uses next-step pace without changing the native actor",
          "[netplay][play-replication][weapon-modifiers][speed-prediction][assets]") {
    CharacterSave save;
    save.progress().health = 1000;
    const std::array party{PartyMember{0, save}, PartyMember{1, save}};
    // G1's authored BOOSTICON is subtype 7, value 4, activeOn 40. The
    // Tower does not contain a Speed record in its native item table.
    Scene scene("G1", false, true, false, party);
    scene.settle();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[0]);
    runtime.actor.save().progress().inventory = {};
    runtime.actor.setPaceBonus(0);
    const f32 before = runtime.actor.speed();
    const auto& infos = scene.world.layout().itemInfos();
    const auto item = std::ranges::find_if(infos, [](const auto& info) {
        return info.type == ItemInfo::kPowerup &&
               info.subtype == static_cast<s32>(ItemKind::SpeedPowerup) && info.value > 0;
    });
    REQUIRE(item != infos.end());
    REQUIRE(scene.world.placeItem(scene.device, item->name, runtime.actor.position()));
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
    REQUIRE(driver.latest());
    const auto worn = PowerupEffects::of(runtime.actor.save().progress().inventory);
    REQUIRE(worn.paceAdd > 0);
    const auto& captured = *driver.latest()->players[0];
    REQUIRE(captured.modifiers);
    CHECK(captured.modifiers->speed);
    CHECK(captured.modifiers->flags == 0);
    REQUIRE(captured.rollback);
    CHECK_FALSE(captured.rollback->speed);
    CHECK(runtime.actor.speed() == before);
    const f32 expected =
        std::clamp(before + worn.paceAdd, PlayerActor::kMinSpeed, PlayerActor::kMaxSpeed);
    CHECK(captured.movement.speed == expected);
    // Outside the Tower, the post-motion pickup also schedules P_PICKUP.
    // This unresolved gesture must withhold replay, not discard the fresh pace.
    CHECK(runtime.gesture == PlayerDeed::Pick);
    CHECK_FALSE(captured.movement.current);
    CHECK_FALSE(captured.movement.enabled);
    REQUIRE(CombatPacket::encode(*driver.latest()));
    REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
    CHECK(runtime.actor.speed() == expected);
    CHECK(driver.latest()->players[0]->rollback->speed);
    REQUIRE(driver.latest()->players[0]->rollback->action == PlayerAnimator::Action::Pick);
    CHECK_FALSE(driver.latest()->players[0]->movement.enabled);
    for (s32 tick = 0;
         tick < 120 && runtime.figure->animator().action() == PlayerAnimator::Action::Pick;
         ++tick) {
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
    }
    REQUIRE(runtime.figure->animator().action() != PlayerAnimator::Action::Pick);
    CHECK(driver.latest()->players[0]->movement.enabled);
    CHECK(driver.latest()->players[0]->movement.speed == expected);
    REQUIRE(driver.latest()->players[0]->modifiers);
    CHECK(driver.latest()->players[0]->modifiers->speed);
}

TEST_CASE("passive invulnerability retains plain local attacks with Speed and spread",
          "[netplay][play-replication][weapon-modifiers][speed-prediction][assets]") {
    const u32 armor = GENERATE(powerup::kInvulnerable, powerup::kGoldInvulnerable,
                               powerup::kInvulnerable | powerup::kGoldInvulnerable);
    const bool defend = GENERATE(false, true);
    Scene scene;
    scene.settle();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[0]);
    auto& inventory = runtime.actor.save().progress().inventory;
    inventory = {};
    inventory.addPowerup(powerup::kArmor, armor, 0, -1);
    inventory.addPowerup(powerup::kSpeed, 0, 2, -1);
    inventory.addPowerup(powerup::kWeapon, PlayerMissiles::kPlainModifiers, 0, -1);
    const auto before = inventory;
    CHECK(scene.play.itemsPredictable(0));
    REQUIRE(scene.play.weaponPreview(0));
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    SessionInputs::Frame input;
    input[0].strongAttack = !defend;
    input[0].defendPressed = defend;
    REQUIRE(driver.advance(pair.host, input, resources) == Result::Advanced);
    REQUIRE(driver.latest());
    const auto& captured = *driver.latest()->players[0];
    CHECK(captured.movement.enabled);
    CHECK(captured.movement.ranged);
    REQUIRE(captured.modifiers);
    CHECK(captured.modifiers->speed);
    CHECK(captured.modifiers->flags == PlayerMissiles::kPlainModifiers);
    REQUIRE(captured.rollback);
    CHECK(captured.rollback->speed);
    CHECK_FALSE(captured.rollback->shielded);
    CHECK_FALSE(PlayerAnimator::locomotion(captured.rollback->action));
    REQUIRE(captured.weapon);
    CHECK(captured.weapon->shots == 5);
    CHECK(inventory == before);
    CHECK(CombatPacket::encode(*driver.latest()));
}

TEST_CASE("Speed with an unsupported weapon retains authority but cannot replay movement",
          "[netplay][play-replication][weapon-modifiers][speed-prediction][assets]") {
    Scene scene;
    scene.settle();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[0]);
    auto& inventory = runtime.actor.save().progress().inventory;
    inventory = {};
    inventory.addPowerup(powerup::kSpeed, 0, 2, -1);
    inventory.addPowerup(powerup::kWeapon, powerup::kThunderHammer, -1, -1);
    CHECK_FALSE(scene.play.weaponModifiers(0));
    CHECK_FALSE(scene.play.predictionSpeed(0));
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
    REQUIRE(driver.latest());
    const auto& captured = *driver.latest()->players[0];
    REQUIRE(captured.rollback);
    CHECK(captured.rollback->speed);
    CHECK(captured.movement.current);
    CHECK_FALSE(captured.movement.enabled);
    CHECK_FALSE(captured.movement.ranged);
    CHECK_FALSE(captured.modifiers);
    CHECK_FALSE(captured.weapon);
    CHECK_FALSE(captured.melee);
    CHECK_FALSE(captured.turbo);
    CHECK_FALSE(captured.turboGeometry);
    CHECK_FALSE(captured.shove);
    CHECK(captured.knockbackTicks == 0);
    CHECK(CombatPacket::encode(*driver.latest()));
}

TEST_CASE("unsupported equipment preserves walking but revokes active ordinary action replay",
          "[netplay][play-replication][weapon-modifiers][assets]") {
    const u32 armor = GENERATE(0U, 0x80000U, 0x80000000U);
    const bool defend = GENERATE(false, true);
    Scene scene;
    scene.settle();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[0]);
    auto& inventory = runtime.actor.save().progress().inventory;
    inventory = {};
    inventory.addPowerup(powerup::kWeapon, powerup::kRapidFire, 0, 30);
    inventory.addPowerup(armor != 0 ? powerup::kArmor : powerup::kWeapon, armor != 0 ? armor : 5U,
                         0, 30);
    CHECK_FALSE(scene.play.itemsPredictable(0));
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
    REQUIRE(driver.latest());
    const auto walking = *driver.latest()->players[0];
    CHECK(walking.movement.enabled);
    CHECK(walking.movement.current);
    CHECK_FALSE(walking.movement.ranged);
    CHECK_FALSE(walking.melee);
    CHECK_FALSE(walking.turbo);
    CHECK_FALSE(walking.weapon);
    SessionInputs::Frame input;
    input[0].defendPressed = defend;
    input[0].strongAttack = !defend;
    REQUIRE(driver.advance(pair.host, input, resources) == Result::Advanced);
    REQUIRE(driver.latest());
    const auto& active = *driver.latest()->players[0];
    REQUIRE(active.rollback);
    CHECK_FALSE(PlayerAnimator::locomotion(active.rollback->action));
    CHECK_FALSE(active.movement.enabled);
    CHECK(active.movement.current);
    CHECK_FALSE(active.movement.ranged);
    CHECK_FALSE(active.melee);
    CHECK_FALSE(active.turbo);
    CHECK_FALSE(active.weapon);
    CHECK(CombatPacket::encode(*driver.latest()));
}

TEST_CASE(
    "shield locomotion admits native gait but withholds attack and reaction replay",
    "[netplay][play-replication][weapon-modifiers][speed-prediction][shield-prediction][assets]") {
    const u32 armor =
        GENERATE(powerup::kReflectShield, powerup::kFireShield, powerup::kLightningShield);
    const bool speed = GENERATE(false, true);
    Scene scene;
    scene.settle();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[0]);
    auto& inventory = runtime.actor.save().progress().inventory;
    inventory = {};
    inventory.addPowerup(powerup::kArmor, armor, 0, -1);
    if (speed) {
        inventory.addPowerup(powerup::kSpeed, 0, 2, -1);
    }
    CHECK(scene.play.predictionSpeed(0));
    CHECK_FALSE(scene.play.itemsPredictable(0));
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    for (const bool moving : {false, true}) {
        SessionInputs::Frame input;
        if (moving) {
            input[0].move = {{0, 1}, 1};
        }
        for (s32 tick = 0; tick < 4; ++tick) {
            REQUIRE(driver.advance(pair.host, input, resources) == Result::Advanced);
        }
        REQUIRE(driver.latest());
        const auto& captured = *driver.latest()->players[0];
        REQUIRE(captured.rollback);
        CHECK(captured.rollback->shielded);
        CHECK(captured.rollback->speed == speed);
        CHECK(PlayerAnimator::locomotion(captured.rollback->action));
        CHECK(captured.movement.current);
        CHECK(captured.movement.enabled);
        REQUIRE(captured.modifiers);
        CHECK(captured.modifiers->shields == armor);
        CHECK(captured.modifiers->speed == speed);
        CHECK(captured.modifiers->ticks == PlayerWeaponModifiers::kMaxTicks);
        CHECK(captured.armEquipment == armor);
        CHECK_FALSE(captured.movement.ranged);
        CHECK_FALSE(captured.weapon);
        CHECK_FALSE(captured.melee);
        CHECK_FALSE(captured.turbo);
        CHECK_FALSE(captured.turboGeometry);
        CHECK_FALSE(captured.shove);
        CHECK(captured.knockbackTicks == 0);
        CHECK(CombatPacket::encode(*driver.latest()));
    }
    inventory.powerups[0].on = false;
    CHECK(scene.play.predictionSpeed(0));
    CHECK(scene.play.itemsPredictable(0));
}

TEST_CASE("four native rapid five-way players keep projectile capture running",
          "[netplay][play-replication][weapon-modifiers][projectile-capacity][assets]") {
    const bool speed = GENERATE(false, true);
    const bool strafe = GENERATE(false, true);
    const u32 element = GENERATE(0U, 1U, 2U, 3U, 4U);
    CharacterSave save;
    save.character = 7; // The native Jester has the shortest ordinary throw cycle.
    save.progress().health = 1000;
    save.progress().inventory.addPowerup(
        powerup::kWeapon, powerup::kRapidFire | powerup::kFiveWayShot | element, 0, -1);
    if (speed) {
        save.progress().inventory.addPowerup(powerup::kSpeed, 0, 2, -1);
    }
    const std::array party{PartyMember{0, save}, PartyMember{1, save}, PartyMember{2, save},
                           PartyMember{3, save}};
    Scene scene({}, false, false, false, party);
    scene.settle();
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    // Local seat ownership changes input routing, not native simulation or the
    // production snapshot/component limit used by a four-peer online room.
    MatchSession host;
    REQUIRE(host.open(1, {1, 1, 1, 1}, {}));
    REQUIRE(host.prepare(1));
    PlayReplication driver;
    REQUIRE(driver.bind(scene.play, host, scene.pickups, scene.fixtures));
    REQUIRE(host.loaded());
    REQUIRE(host.phase() == MatchSession::Phase::Running);
    SessionInputs::Frame input;
    for (auto& player : input) {
        player.attack = true;
        player.aimDirection = Vec3{0, 0, 1};
        player.strafe = strafe;
    }
    usize maxPhysical = 0;
    usize maxComponents = 0;
    usize maxParticleComponents = 0;
    usize maxRetiringComponents = 0;
    usize maxRawBytes = 0;
    usize maxCompressedBytes = 0;
    usize maxBurstBytes = 0;
    usize maxPackets = 0;
    usize reconstructed = 0;
    usize firstFailurePhysical = 0;
    usize firstFailureComponents = 0;
    s32 firstFailureTick = -1;
    std::array<s32, 4> lastRelease{-1, -1, -1, -1};
    std::array<s32, 4> minimumPeriod{600, 600, 600, 600};
    std::array<s32, 4> releases{};
    std::array<s32, 4> strafeTicks{};
    std::array<s32, 4> strafeReleases{};
    std::array<f32, 4> distance{};
    std::array<Vec3, 4> previous{};
    for (usize seat = 0; seat < previous.size(); ++seat) {
        REQUIRE(scene.play.actor(static_cast<s32>(seat)));
        previous[seat] = scene.play.actor(static_cast<s32>(seat))->position();
    }
    for (s32 tick = 0; tick < 600; ++tick) {
        if (strafe) {
            // Stay near the native spawn with short alternating sidesteps. Keep
            // actual input, movement, facing, collisions and animations in play.
            const f32 direction = (tick / 24) % 2 == 0 ? 1.0f : -1.0f;
            for (auto& player : input) {
                player.move = {{direction, 0}, 1};
            }
        }
        bool failedNow = false;
        if (firstFailureTick < 0) {
            const auto result = driver.advance(host, input, resources);
            if (result != Result::Advanced) {
                firstFailureTick = tick;
                failedNow = true;
            }
        } else {
            // Continue the unchanged native scene solely to measure its peak
            // after the replication owner fails; do not suppress real impacts.
            REQUIRE(scene.play.update(1.0 / 60, input) == PlayOutcome::Running);
        }
        for (usize seat = 0; seat < releases.size(); ++seat) {
            const auto* animator = scene.play.animator(static_cast<s32>(seat));
            REQUIRE(animator);
            CHECK(animator->checkpoint().speed == speed);
            CHECK(animator->checkpoint().rapid);
            const bool shootingStrafe =
                animator->action() >= PlayerAnimator::Action::StrafeShootForward1 &&
                animator->action() <= PlayerAnimator::Action::StrafeShootRight2;
            strafeTicks[seat] += shootingStrafe ? 1 : 0;
            const auto position = scene.play.actor(static_cast<s32>(seat))->position();
            distance[seat] += glm::distance(position, previous[seat]);
            previous[seat] = position;
            if (animator->released()) {
                strafeReleases[seat] += shootingStrafe ? 1 : 0;
                if (lastRelease[seat] >= 0) {
                    minimumPeriod[seat] = std::min(minimumPeriod[seat], tick - lastRelease[seat]);
                }
                lastRelease[seat] = tick;
                ++releases[seat];
            }
        }
        const auto& missiles = scene.play.missiles();
        usize components = 0;
        for (usize index = 0; index < missiles.count(); ++index) {
            const auto& missile = missiles.missile(index);
            if (missile.effect == 0 && missile.model != nullptr && missile.model->bound()) {
                ++components;
            }
            if (missile.streak.texture != nullptr) {
                ++components;
            }
        }
        for (usize index = 0; index < missiles.visuals().count(); ++index) {
            const auto& effect = missiles.visuals().effect(index);
            const auto* history = missiles.visualHistory(effect.id);
            if (effect.tree != nullptr && (!effect.retiring || history != nullptr) &&
                (effect.model.bound() || history != nullptr)) {
                ++components;
            }
        }
        if (firstFailureTick < 0) {
            REQUIRE(driver.latest());
            const auto& captured = driver.latest()->projectiles;
            // Use the actual replicated roster, including particle-only riders,
            // retained tails and any other scene effects. The native count above
            // remains a conservative witness if capture fails before publishing.
            components = captured.size();
            usize particleComponents = 0;
            usize retiringComponents = 0;
            for (const auto& shot : captured) {
                if (shot.particles && resources.hasParticles(shot.resource)) {
                    ++particleComponents;
                }
                if (shot.particles && shot.particles->flight.retiring) {
                    ++retiringComponents;
                }
            }
            maxParticleComponents = std::max(maxParticleComponents, particleComponents);
            maxRetiringComponents = std::max(maxRetiringComponents, retiringComponents);
        }
        const bool newPeak = components > maxComponents;
        maxPhysical = std::max(maxPhysical, missiles.count());
        maxComponents = std::max(maxComponents, components);
        if (failedNow) {
            firstFailurePhysical = missiles.count();
            firstFailureComponents = components;
        }
        if (firstFailureTick < 0 && (newPeak || tick % 120 == 0 || tick == 599)) {
            REQUIRE(driver.latest());
            const auto& snapshot = *driver.latest();
            const auto raw = CombatPacket::encode(snapshot);
            REQUIRE(raw);
            const auto compressed = SnapshotBlock::encode(*raw);
            REQUIRE(compressed);
            const auto packets =
                CombatReplica::packets(snapshot, SnapshotBlock::Compression::Automatic,
                                       CombatReplica::Recovery::SingleLoss);
            REQUIRE(packets);
            usize burstBytes = 0;
            CombatReplica replica;
            REQUIRE(replica.begin(1, snapshot.motion.epoch));
            for (const auto& packet : *packets) {
                burstBytes += packet.size();
                CHECK(packet.size() <= PacketTransport::kMaxPacketBytes);
                const auto admitted = replica.receive(1, packet);
                // Repair packets following the final data chunk are already stale.
                CHECK((admitted == CombatReplica::Admission::Pending ||
                       admitted == CombatReplica::Admission::Committed ||
                       admitted == CombatReplica::Admission::Stale));
            }
            REQUIRE(replica.latest());
            CHECK(CombatPacket::encode(*replica.latest()) == raw);
            CHECK(replica.latest()->projectiles.size() == snapshot.projectiles.size());
            maxRawBytes = std::max(maxRawBytes, raw->size());
            maxCompressedBytes = std::max(maxCompressedBytes, compressed->size());
            maxBurstBytes = std::max(maxBurstBytes, burstBytes);
            maxPackets = std::max(maxPackets, packets->size());
            ++reconstructed;
        }
    }
    CAPTURE(speed, strafe, element, strafeTicks, strafeReleases, distance, maxPhysical,
            maxComponents, maxParticleComponents, maxRetiringComponents, firstFailureTick,
            firstFailurePhysical, firstFailureComponents, minimumPeriod, releases, maxRawBytes,
            maxCompressedBytes, maxBurstBytes, maxPackets, reconstructed);
    CHECK(std::ranges::all_of(releases, [](s32 count) { return count > 10; }));
    if (strafe) {
        CHECK(std::ranges::all_of(strafeTicks, [](s32 count) { return count > 300; }));
        CHECK(std::ranges::all_of(strafeReleases, [](s32 count) { return count > 10; }));
        CHECK(std::ranges::all_of(distance, [](f32 travelled) { return travelled > 1; }));
    } else {
        CHECK(std::ranges::all_of(strafeTicks, [](s32 count) { return count == 0; }));
    }
    CHECK(firstFailureTick == -1);
    CHECK(reconstructed > 5);
    CHECK(maxBurstBytes <= static_cast<usize>(PacketTransport::kSendBufferBytes));
}

TEST_CASE("host certifies elemental throws with distinct native body rider and streak identities",
          "[netplay][play-replication][elemental-replay][assets]") {
    const u32 element = GENERATE(1U, 2U, 3U, 4U);
    const s32 character = GENERATE(0, 2, 6);
    const bool strong = GENERATE(false, true);
    CAPTURE(element, character, strong);
    CharacterSave save;
    save.character = character;
    save.progress().health = 1000;
    const u32 flags = element | powerup::kRapidFire | powerup::kFiveWayShot;
    save.progress().inventory.addPowerup(powerup::kWeapon, flags, 0, -1);
    save.progress().inventory.addPowerup(powerup::kSpeed, 0, 2, -1);
    const std::array party{PartyMember{0, save}, PartyMember{1, save}};
    Scene scene({}, false, false, false, party);
    scene.settle();
    const auto before = scene.play.actor(1)->save().progress().inventory;
    CHECK_FALSE(scene.play.itemsPredictable(1));
    CHECK(scene.play.rangedItemsPredictable(1));
    CHECK(scene.play.predictionSpeed(1));
    CHECK_FALSE(scene.play.meleePreview(1));
    CHECK_FALSE(scene.play.turboGeometry(1));
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
    REQUIRE(driver.latest());
    REQUIRE(driver.latest()->players[1]->weapon);
    const auto descriptor = *driver.latest()->players[1]->weapon;
    REQUIRE(descriptor.rider != 0);
    REQUIRE(resources.acceptsWeapon(descriptor));
    const auto& runtime = scene.play.participants()[1];
    const auto rider = scene.play.weaponRider(runtime.actor, *runtime.figure);
    REQUIRE(rider);
    CHECK(descriptor.rider == resources.treeId(rider->archive, rider->tree, rider->lenders));
    // No projectile has used either bank yet: zero is not an invented running clock.
    CHECK_FALSE(descriptor.textureFrame);
    CHECK_FALSE(descriptor.riderTextureFrame);
    SessionInputs::Frame inputs;
    inputs[0].attack = !strong;
    inputs[0].attackPressed = !strong;
    inputs[0].strongAttack = strong;
    inputs[0].aimDirection = Vec3{0, 0, 1};
    for (s32 tick = 0; tick < 180 && scene.play.missiles().count() == 0; ++tick) {
        REQUIRE(MatchInputs::sample(pair.guest, inputs));
        pair.pump();
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        pair.pump();
        inputs[0].attackPressed = false;
    }
    if (character == 6 && strong) {
        // Native Sorceress heavy actions do not release the ordinary weapon.
        // The checkpoint may carry its future quick-throw descriptor, but neither
        // native play nor replay may manufacture a missile from this animation.
        CHECK(scene.play.missiles().count() == 0);
        REQUIRE(driver.latest());
        CHECK(std::ranges::none_of(driver.latest()->projectiles,
                                   [](const auto& shot) { return shot.rollback.has_value(); }));
        REQUIRE(CombatPacket::encode(*driver.latest()));
        return;
    }
    REQUIRE(scene.play.missiles().count() == 5);
    REQUIRE(driver.latest());
    const auto& captured = *driver.latest();
    const auto& player = *captured.players[1];
    REQUIRE(player.modifiers);
    CHECK(player.modifiers->flags == flags);
    CHECK(player.modifiers->speed);
    REQUIRE(player.weapon);
    CHECK(player.movement.enabled);
    CHECK(player.movement.ranged);
    CHECK_FALSE(player.melee);
    CHECK_FALSE(player.turboGeometry);
    CHECK(player.weapon->rider == descriptor.rider);
    REQUIRE(player.weapon->riderTextureFrame);
    std::array<u32, 5> bodies{}, riders{}, streaks{};
    for (const auto& shot : captured.projectiles) {
        if (!shot.rollback) {
            continue;
        }
        const auto& origin = shot.rollback->origin;
        CHECK(origin.seat == 1);
        CHECK(origin.tick == captured.motion.tick);
        CHECK(origin.strong == strong);
        CHECK(origin.generation == player.rollback->animation.generation);
        REQUIRE(origin.ordinal < 5);
        REQUIRE(resources.accepts(shot));
        switch (shot.source) {
        case ProjectileSource::PlayerEffect:
        case ProjectileSource::Player: ++bodies[origin.ordinal]; break;
        case ProjectileSource::PlayerRider:
            ++riders[origin.ordinal];
            CHECK(shot.resource == descriptor.rider);
            break;
        case ProjectileSource::Streak: ++streaks[origin.ordinal]; break;
        default: FAIL("unrelated effect received an ordinary throw identity");
        }
    }
    for (usize ordinal = 0; ordinal < 5; ++ordinal) {
        CHECK(bodies[ordinal] == (WeaponGlow::throwsEffectAlone(character) ? 0U : 1U));
        CHECK(riders[ordinal] == 1);
        CHECK(streaks[ordinal] == (descriptor.streak == 0 ? 0U : 1U));
    }
    CHECK(scene.play.actor(1)->save().progress().inventory == before);
    const auto bytes = CombatPacket::encode(captured);
    REQUIRE(bytes);
    const auto decoded = CombatPacket::decode(*bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->players[1]->weapon);
    CHECK(decoded->players[1]->weapon->rider == descriptor.rider);
    CHECK(CombatPacket::encode(*decoded) == bytes);
}
TEST_CASE("native turbo volleys carry distinct persistent identities through host capture",
          "[netplay][play-replication][turbo-volley-identity][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const bool full = GENERATE(false, true);
    CAPTURE(character, full);
    CharacterSave save;
    save.character = character;
    save.progress().health = 1000;
    const std::array party{PartyMember{0, save}, PartyMember{1, save}};
    Scene scene({}, false, false, false, party);
    scene.settle();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[1]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& other = const_cast<PlayerRuntime&>(scene.play.participants()[0]);
    other.life = PlayerLife::InTower;
    runtime.turbo.reset();
    runtime.turbo.add(full ? TurboMeter::kFullCost : TurboMeter::kStrongCost);
    ClassDataSet classes;
    REQUIRE(classes.load(scene.root / "PDATA"));
    const auto* stats = classes.stats(character);
    REQUIRE(stats);
    auto rows = stats->strikesOf(full ? stats->moves.turboC1 : stats->moves.turboB);
    if (full) {
        const auto second = stats->strikesOf(stats->moves.turboC2);
        rows.insert(rows.end(), second.begin(), second.end());
    }
    const bool hasVolley = std::ranges::any_of(rows, [&](s32 row) {
        return stats->moveStrikes[static_cast<usize>(row)].type == MoveStrike::kVolley;
    });
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    SessionInputs::Frame input;
    input[0].turboAttackPressed = true;
    input[0].aimDirection = Vec3{0, 0, 1};
    const auto inventory = runtime.actor.save().progress().inventory;
    std::map<u64, ShotIdentity> physical;
    std::map<ShotIdentity, u64> identities;
    std::set<ShotIdentity> captured;
    std::map<u64, EffectIdentity> nativeEffects;
    std::set<EffectIdentity> capturedEffects;
    usize nativeEmissions = 0;
    usize tails = 0;
    bool active = false;
    for (s32 tick = 0; tick < 240; ++tick) {
        REQUIRE(MatchInputs::sample(pair.guest, input));
        pair.pump();
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        input[0].turboAttackPressed = false;
        const auto& snapshot = *driver.latest();
        const auto action = runtime.figure->animator().action();
        active = active || action == (full ? PlayerAnimator::Action::TurboFull
                                           : PlayerAnimator::Action::TurboStrong);
        const auto inspectNative = [&](u64 instance, s32 owner,
                                       const std::optional<MissileRelease>& release) {
            if (owner != 1 || !release || release->emission == 0) {
                return;
            }
            CHECK(release->character == character);
            CHECK_FALSE(release->strong);
            CHECK(release->ordinal == 0);
            const auto slot = (release->emission >> 16U) - 1U;
            REQUIRE(slot < rows.size());
            CHECK(stats->moveStrikes[static_cast<usize>(rows[slot])].type == MoveStrike::kVolley);
            auto known = physical.find(instance);
            if (known == physical.end()) {
                ShotIdentity origin{snapshot.motion.epoch,
                                    snapshot.motion.tick,
                                    release->generation,
                                    pair.host.context().grants[1],
                                    1,
                                    false,
                                    0,
                                    release->emission};
                REQUIRE(origin.valid());
                CHECK_FALSE(identities.contains(origin));
                identities.emplace(origin, instance);
                known = physical.emplace(instance, origin).first;
                ++nativeEmissions;
            }
            CHECK(known->second.emission == release->emission);
            CHECK(known->second.generation == release->generation);
        };
        const auto& missiles = scene.play.missiles();
        for (usize index = 0; index < missiles.count(); ++index) {
            const auto& missile = missiles.missile(index);
            inspectNative(missile.instance, missile.owner, missile.release);
        }
        for (const auto& history : missiles.visualHistory()) {
            inspectNative(history.instance, history.owner, history.release);
        }
        const auto& effects = scene.play.effects();
        for (usize index = 0; index < effects.count(); ++index) {
            const auto& effect = effects.effect(index);
            if (!effect.provenance) {
                continue;
            }
            const auto& tag = *effect.provenance;
            REQUIRE(tag.player == 1);
            REQUIRE(tag.occurrence < rows.size());
            s32 row = stats->moveStrikes[static_cast<usize>(rows[tag.occurrence])].effect;
            for (usize ordinal = 0; ordinal < tag.effectOrdinal && row >= 0; ++ordinal) {
                REQUIRE(static_cast<usize>(row) < stats->moveEffects.size());
                row = stats->moveEffects[static_cast<usize>(row)].next;
            }
            REQUIRE(row >= 0);
            REQUIRE(static_cast<usize>(row) < stats->moveEffects.size());
            if (!nativeEffects.contains(effect.instance)) {
                CHECK(effect.name == stats->moveEffects[static_cast<usize>(row)].tree);
            }
            const EffectIdentity origin{snapshot.motion.epoch,
                                        snapshot.motion.tick,
                                        tag.generation,
                                        pair.host.context().grants[1],
                                        1,
                                        tag.occurrence,
                                        tag.effectOrdinal};
            REQUIRE(origin.valid());
            nativeEffects.try_emplace(effect.instance, origin);
        }
        bool hasAttributed = false;
        std::set<std::pair<ProjectileSource, ShotIdentity>> components;
        for (const auto& shot : snapshot.projectiles) {
            if (shot.effectOrigin) {
                CHECK(shot.source == ProjectileSource::WorldEffect);
                REQUIRE(nativeEffects.contains(shot.instance));
                CHECK(shot.effectOrigin == nativeEffects.at(shot.instance));
                CHECK_FALSE(shot.releaseOrigin());
                capturedEffects.insert(*shot.effectOrigin);
                hasAttributed = true;
            }
            const auto* origin = shot.releaseOrigin();
            if (origin == nullptr || origin->emission == 0) {
                continue;
            }
            hasAttributed = true;
            CHECK(origin->seat == 1);
            REQUIRE(identities.contains(*origin));
            CHECK(components.emplace(shot.source, *origin).second);
            CHECK(resources.accepts(shot));
            captured.insert(*origin);
            if (shot.particles && shot.particles->flight.retiring) {
                CHECK_FALSE(shot.rollback);
                CHECK(shot.particles->origin == *origin);
                ++tails;
            }
        }
        if (hasAttributed) {
            const auto bytes = CombatPacket::encode(snapshot);
            REQUIRE(bytes);
            const auto decoded = CombatPacket::decode(*bytes);
            REQUIRE(decoded);
            CHECK(CombatPacket::encode(*decoded) == bytes);
        }
        pair.pump();
    }
    CAPTURE(nativeEmissions, tails, captured.size(), rows.size(), capturedEffects.size());
    CHECK(active);
    CHECK((nativeEmissions > 0) == hasVolley);
    CHECK((!captured.empty()) == hasVolley);
    if (character == 6 && !full) {
        CHECK_FALSE(capturedEffects.empty()); // Sorceress B has rendered cloud/rain trees.
    }
    CHECK(runtime.actor.save().progress().inventory == inventory);
}

TEST_CASE("host effect provenance fences tagged visuals that predate the match epoch",
          "[netplay][play-replication][effect-identity][assets]") {
    CharacterSave save;
    save.character = 6;
    save.progress().health = 1000;
    const std::array party{PartyMember{0, save}, PartyMember{1, save}};
    Scene scene({}, false, false, false, party);
    scene.settle();
    const auto& runtime = scene.play.participants()[1];
    REQUIRE(runtime.figure);
    auto* archive = runtime.figure->effects();
    REQUIRE(archive);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& effects = const_cast<EffectTrees&>(scene.play.effects());
    EffectTrees::Setting setting;
    setting.seconds = 10;
    const u32 id =
        effects.startSet(scene.device, *archive, "SOR_PWRB2", runtime.actor.position(), setting);
    REQUIRE(id != 0);
    REQUIRE(effects.tag(id, {1, runtime.figure->animator().player().generation(), 0, 0}));
    u64 instance = 0;
    for (usize index = 0; index < effects.count(); ++index) {
        if (effects.effect(index).id == id) {
            instance = effects.effect(index).instance;
        }
    }
    REQUIRE(instance != 0);
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    for (s32 tick = 0; tick < 5; ++tick) {
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        const auto& captured = driver.latest()->projectiles;
        const auto found = std::ranges::find_if(captured, [&](const auto& shot) {
            return shot.source == ProjectileSource::WorldEffect && shot.instance == instance;
        });
        REQUIRE(found != captured.end());
        CHECK_FALSE(found->effectOrigin);
        CHECK(CombatPacket::encode(*driver.latest()));
        pair.pump();
    }
}
TEST_CASE("native thrown potion capture retains its release identity and exact bottle resource",
          "[netplay][play-replication][potion-checkpoint][assets]") {
    const s32 kind = GENERATE(1, 2, 3, 4);
    CAPTURE(kind);
    Scene scene;
    scene.settle();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native fixture
    auto& runtime = const_cast<PlayerRuntime&>(scene.play.participants()[1]);
    runtime.actor.save().progress().inventory = {};
    runtime.actor.save().progress().inventory.potions = {kind};
    Pair pair;
    PlayReplication driver;
    ProjectileResources resources;
    REQUIRE(scene.play.bindSceneProjectiles(resources));
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    std::optional<PotionIdentity> identity;
    usize observed = 0;
    usize tails = 0;
    bool removed = false;
    for (u32 tick = 0; tick < 240; ++tick) {
        SessionInputs::Frame inputs;
        inputs[0].throwPotion = tick < 10;
        inputs[0].aimDirection = Vec3{-1, 0, 0};
        REQUIRE(MatchInputs::sample(pair.guest, inputs));
        pair.send(pair.guest, pair.host, 1);
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        const auto& snapshot = *driver.latest();
        bool live = false;
        for (usize index = 0; index < scene.play.missiles().count(); ++index) {
            const auto& missile = scene.play.missiles().missile(index);
            if (missile.owner != 1 || missile.potion != kind) {
                continue;
            }
            live = true;
            REQUIRE(missile.potionRelease);
            const auto& visuals = scene.play.missiles().visuals();
            const EffectTrees::Effect* bottle = nullptr;
            for (usize effect = 0; effect < visuals.count(); ++effect) {
                if (visuals.effect(effect).id == missile.effect) {
                    bottle = &visuals.effect(effect);
                    break;
                }
            }
            REQUIRE(bottle);
            CHECK(bottle->name == PotionEmission::look(kind).bottle);
            const auto found = std::ranges::find_if(snapshot.projectiles, [&](const auto& shot) {
                return shot.source == ProjectileSource::PlayerEffect &&
                       shot.instance == bottle->instance;
            });
            REQUIRE(found != snapshot.projectiles.end());
            REQUIRE(found->potion);
            CHECK(found->resource == resources.effectId(*bottle));
            CHECK(resources.hasParticles(found->resource));
            REQUIRE(found->particles);
            CHECK(found->particles->potion == kind);
            CHECK(found->particles->character == 0);
            CHECK_FALSE(found->particles->origin);
            CHECK(found->particles->potionOrigin == found->potion->origin);
            CHECK_FALSE(found->particles->flight.retiring);
            CHECK(found->potion->kind == kind);
            CHECK(found->potion->flight == PlayerMissiles::flightCheckpoint(missile));
            CHECK(found->potion->origin.epoch == snapshot.motion.epoch);
            CHECK(found->potion->origin.seat == 1);
            CHECK(found->potion->origin.grant == snapshot.motion.players[1]->grant);
            CHECK(found->potion->origin.generation == missile.potionRelease->generation);
            CHECK(found->potion->origin.kind == PotionIdentity::Kind::Throw);
            if (!identity) {
                identity = found->potion->origin;
                CHECK(identity->tick == snapshot.motion.tick);
                CHECK(runtime.figure->animator().potionThrown());
            } else {
                CHECK(found->potion->origin == *identity);
            }
            ++observed;
        }
        if (identity && !live) {
            removed = true;
            CHECK(std::ranges::none_of(snapshot.projectiles,
                                       [](const auto& shot) { return shot.potion.has_value(); }));
            for (const auto& shot : snapshot.projectiles) {
                if (shot.potionReleaseOrigin() == nullptr ||
                    *shot.potionReleaseOrigin() != *identity) {
                    continue;
                }
                REQUIRE(shot.particles);
                CHECK(shot.particles->potion == kind);
                CHECK(shot.particles->potionOrigin == identity);
                CHECK(shot.particles->flight.retiring);
                CHECK(shot.particles->flight.totalSteps > shot.particles->flight.activeSteps);
                ++tails;
            }
        }
        REQUIRE(CombatPacket::encode(snapshot));
        pair.pump();
    }
    CHECK(identity.has_value());
    CHECK(observed > 1);
    CHECK(tails > 0);
    CHECK(removed);
    REQUIRE(driver.latest());
    CHECK(std::ranges::none_of(driver.latest()->projectiles, [](const auto& shot) {
        return shot.potionReleaseOrigin() != nullptr;
    }));
    CHECK(runtime.actor.save().progress().inventory.potions.empty());
}
} // namespace
