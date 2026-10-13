#include <set>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/OnlinePair.h"
#include "game/screens/OnlinePlay.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct RuntimePair {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice hostDevice;
    test::FakeRenderDevice guestDevice;
    GameConfig config;
    StringTable strings;
    LevelCatalog catalog;
    GameContext context;
    test::OnlinePair wire{{}, false, 2, 2};
    OnlineParty hostParty;
    OnlineParty guestParty;
    OnlinePlay host;
    OnlinePlay guest;

    explicit RuntimePair(s32 classOffset = 0, bool pendingPromotion = false,
                         bool pendingRelic = false) {
        REQUIRE(catalog.load(root));
        context.config = &config;
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        context.strings = &strings;
        context.levels = &catalog;
        context.unpackedRoot = root;
        const auto choice = [pendingPromotion, pendingRelic](s32 device, s32 character, s32 color) {
            CharacterSave save;
            save.name = "ONLINE";
            save.character = character;
            save.color = color;
            save.progress().experience = levelExperience(60);
            save.progress().promotedLevel = pendingPromotion ? 20 : -1;
            if (pendingRelic) {
                save.progress().relics.runes = save.progress().relics.pendingRunes = 1;
            }
            save.progress().health = 9000;
            save.classUnlock = 0xFFFF;
            save.moviesSeen = {"PRIVATE"};
            save.classes[static_cast<usize>((character + 1) % kClassCount)].experience = 123456;
            return PartyMember{device, save, static_cast<usize>(10 + device)};
        };
        REQUIRE(hostParty.select(wire.hostSession, std::array{choice(1, classOffset, 0),
                                                              choice(3, classOffset + 1, 1)}));
        REQUIRE(guestParty.select(wire.guestSession, std::array{choice(0, classOffset + 2, 2),
                                                                choice(2, classOffset + 3, 3)}));
        wire.start();
    }
    void open(const LevelRef& level = LevelRef::tower(), PlayOptions options = {}) {
        options.welcome = false;
        if (level.isTower() && !options.position) {
            options.position = Vec3{0, 0, 0}; // bypass the tower entry pan for immediate input
        }
        REQUIRE(guest.open(guestDevice, context, wire.guestSession, guestParty));
        REQUIRE(host.open(hostDevice, context, wire.hostSession, hostParty, level, options));
        // The host cannot simulate before the guest has loaded its own resources.
        CHECK(host.update({}) == OnlinePlay::Phase::Loading);
        CHECK(wire.host.tick() == 0);
        CHECK_FALSE(host.shown());
        for (s32 i = 0; i < 3600 && host.phase() != OnlinePlay::Phase::Playing; ++i) {
            step();
        }
        REQUIRE(host.phase() == OnlinePlay::Phase::Playing);
        REQUIRE(guest.phase() == OnlinePlay::Phase::Playing);
    }
    void step(const SessionInputs::Frame& hostInputs = {},
              const SessionInputs::Frame& guestInputs = {}) {
        REQUIRE(host.update(hostInputs) != OnlinePlay::Phase::Failed);
        REQUIRE(guest.update(guestInputs) != OnlinePlay::Phase::Failed);
        if (host.entryVisible()) {
            hostDevice.draws.clear();
            host.render(hostDevice, Mat4{1}, 1280, 720, 1.0 / 60, 1);
        }
        if (guest.entryVisible()) {
            draw();
        }
    }
    void draw() {
        guestDevice.draws.clear();
        guest.render(guestDevice, Mat4{1}, 1280, 720, 1.0 / 60, 1);
        REQUIRE(guest.phase() != OnlinePlay::Phase::Failed);
    }
};

TEST_CASE("native online updates submit new input and snapshots before returning",
          "[netplay][online-play][assets][latency]") {
    RuntimePair pair;
    pair.open();
    for (s32 tick = 0; tick < 150; ++tick) {
        pair.step();
    }
    const auto sentBefore = pair.wire.guest.latency().sendQueue().summary().count;
    const auto hostPolls = pair.wire.hostWire.polls;
    const auto guestPolls = pair.wire.guestWire.polls;
    REQUIRE(pair.guest.update({}) != OnlinePlay::Phase::Failed);
    const auto sampled = pair.wire.guest.sampledInputs();
    REQUIRE(sampled.size() == 2);
    const u64 inputTick = sampled.front().tick;
    CHECK(pair.wire.guest.latency().sendQueue().summary().count == sentBefore + 2);
    CHECK(pair.wire.guestWire.polls == guestPolls + 1);
    CHECK(pair.wire.hostWire.polls == hostPolls);
    // Receiving only, with no second guest update, must admit that exact sample.
    pair.wire.hostSession.update(0);
    CHECK(pair.wire.host.inputDiagnostics(2).received == inputTick);

    while (pair.wire.host.tick() % PlayReplication::kSnapshotStride != 0) {
        pair.step();
    }
    const auto capturedTick = pair.wire.host.tick();
    REQUIRE(pair.host.update({}) != OnlinePlay::Phase::Failed);
    pair.wire.guestSession.update(0);
    REQUIRE(pair.wire.guest.playback().latest());
    CHECK(pair.wire.guest.playback().latest()->motion.tick == capturedTick);
}

TEST_CASE("native guest movement starts locally before the host consumes its input",
          "[netplay][online-play][assets][prediction]") {
    const u64 latency = GENERATE(0, 3, 6, 9);
    RuntimePair pair;
    pair.open();
    for (s32 tick = 0; tick < 150; ++tick) {
        pair.step();
    }
    pair.draw();
    REQUIRE(pair.guest.shown());
    const auto* confirmed = pair.wire.guest.playback().latest();
    REQUIRE(confirmed);
    REQUIRE(confirmed->players[2]->movement.enabled);
    pair.wire.hostWire.latency = latency;
    pair.wire.guestWire.latency = latency;
    const auto before = confirmed->motion.players[2]->position;
    const auto hostBefore = pair.host.hostScene()->actor(2)->position();
    SessionInputs::Frame input;
    input[0].move = {{1, 0}, 1};
    pair.step({}, input);
    pair.draw();
    CHECK(pair.guest.shown()->motion.players[2]->position != before);
    CHECK(pair.host.hostScene()->actor(2)->position() == hostBefore);
    CHECK(pair.guest.predictionMetrics().samples > 0);
    CHECK_FALSE(pair.guest.hostScene());
    s32 hostConfirmedAt = -1;
    for (s32 tick = 1; tick <= 60; ++tick) {
        pair.step({}, input);
        pair.draw();
        const auto* latest = pair.wire.guest.playback().latest();
        if (hostConfirmedAt < 0 && latest->motion.players[2]->position != before) {
            hostConfirmedAt = tick;
        }
    }
    CAPTURE(latency, hostConfirmedAt, pair.guest.predictionMetrics().largestCorrection);
    CHECK(hostConfirmedAt >= static_cast<s32>(latency));
    CHECK(hostConfirmedAt < 60);
    CHECK(pair.guest.predictionMetrics().resolvedDeadlineMs > 0);
}

TEST_CASE("guest tower audio and promotion figures follow the host without a gameplay scene",
          "[netplay][online-play][assets][guest-presentation]") {
    AudioMixer hostMixer(48000);
    AudioMixer guestMixer(48000);
    SoundPlayer hostSounds(hostMixer);
    SoundPlayer guestSounds(guestMixer);
    guestSounds.setCategoryVolume(SoundCategory::Music, 0);
    RuntimePair pair(0, true);
    const AssetLocator assets(pair.root);
    auto hostContext = pair.context;
    auto guestContext = pair.context;
    hostContext.sounds = &hostSounds;
    guestContext.sounds = &guestSounds;
    hostContext.assets = guestContext.assets = &assets;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{0};
    REQUIRE(
        pair.guest.open(pair.guestDevice, guestContext, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, hostContext, pair.wire.hostSession, pair.hostParty,
                           LevelRef::tower(), options));
    bool audible = false;
    bool sumner = false;
    bool promotion = false;
    bool caption = false;
    std::vector<f32> pcm(usize{800} * 2);
    for (usize tick = 0; tick < 240; ++tick) {
        pair.step();
        pair.draw();
        hostMixer.mix(pcm);
        guestMixer.mix(pcm);
        audible = audible || std::ranges::any_of(pcm, [](f32 value) { return value != 0; });
        hostSounds.update();
        guestSounds.update();
        const auto* shown = pair.guest.shown();
        if (shown == nullptr) {
            continue;
        }
        for (const auto& figure : shown->projectiles) {
            if (figure.source == ProjectileSource::Sumner) {
                sumner = true;
                CHECK(Vec3{figure.placement[3]} == pair.host.hostScene()->sumner().position());
            }
            if (figure.source == ProjectileSource::Promotion) {
                promotion = true;
                CHECK(figure.placement == pair.host.hostScene()->promotion().wizardTransform());
                CHECK(figure.flags == (ProjectileState::kUnlit | ProjectileState::kAdditive));
            }
        }
        CHECK(shown->musicArea == pair.host.shown()->musicArea);
        caption = caption || (shown->hud &&
                              shown->hud->ceremony.kind == HudSnapshot::Ceremony::Kind::Promotion);
    }
    CHECK(audible);
    CHECK(sumner);
    CHECK(promotion);
    CHECK(caption);
    CHECK_FALSE(pair.guest.hostScene());
    // Music and effects are independent local mixer categories. Prove that
    // the audible result above was not just the soundtrack masking absent cues.
    guestSounds.setCategoryVolume(SoundCategory::Effects, 0);
    guestSounds.setCategoryVolume(SoundCategory::Music, 1);
    bool music = false;
    for (usize tick = 0; tick < 30; ++tick) {
        pair.step();
        pair.draw();
        hostMixer.mix(pcm);
        guestMixer.mix(pcm);
        music = music || std::ranges::any_of(pcm, [](f32 value) { return value != 0; });
        hostSounds.update();
        guestSounds.update();
    }
    CHECK(music);
    pair.guest.close();
    pair.host.close();
}

TEST_CASE("guest native throw audio precedes reliable confirmation without playing twice",
          "[netplay][online-play][assets][guest-audio][predicted-audio]") {
    const bool spread = GENERATE(false, true);
    const bool predict = GENERATE(false, true);
    const s32 holdTicks = GENERATE(60, 180);
    CAPTURE(spread, predict, holdTicks);
    AudioMixer hostMixer(48000);
    AudioMixer guestMixer(48000);
    SoundPlayer hostSounds(hostMixer);
    SoundPlayer guestSounds(guestMixer);
    RuntimePair pair;
    const AssetLocator assets(pair.root);
    auto hostContext = pair.context;
    auto guestContext = pair.context;
    hostContext.sounds = &hostSounds;
    guestContext.sounds = &guestSounds;
    hostContext.assets = guestContext.assets = &assets;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{0};
    REQUIRE(
        pair.guest.open(pair.guestDevice, guestContext, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, hostContext, pair.wire.hostSession, pair.hostParty,
                           LevelRef::tower(), options));
    pair.guest.setMovementPrediction(predict);
    std::vector<f32> pcm(usize{800} * 2);
    const auto mix = [&] {
        hostMixer.mix(pcm);
        guestMixer.mix(pcm);
        hostSounds.update();
        guestSounds.update();
    };
    for (s32 tick = 0; tick < 180; ++tick) {
        pair.step();
        pair.draw();
        mix();
    }
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.host.hostScene());
    // This fixture's guest device zero owns global seat two.
    constexpr u8 kSeat = 2;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned native scene fixture
    auto& shooter = const_cast<PlayerRuntime&>(pair.host.hostScene()->participants()[kSeat]);
    REQUIRE(shooter.actor.player() == kSeat);
    if (spread) {
        shooter.actor.save().progress().inventory.addPowerup(powerup::kWeapon,
                                                             powerup::kFiveWayShot, 0, -1);
    }
    for (s32 tick = 0; tick < 30; ++tick) {
        pair.step();
        pair.draw();
        mix();
    }
    const auto* authority = pair.wire.guest.playback().latest();
    REQUIRE(authority);
    REQUIRE(authority->players[kSeat]);
    REQUIRE(authority->players[kSeat]->movement.ranged);
    REQUIRE(authority->players[kSeat]->weapon);
    REQUIRE(authority->players[kSeat]->weapon->shots == (spread ? 5 : 1));
    std::string expectedBank;
    u32 expectedCall = 0;
    if (spread) {
        SoundSet common;
        REQUIRE(common.load(pair.root / "AUDIO/COMMON"));
        const auto call = common.find("S_SUPERSHOT");
        REQUIRE(call);
        const auto sequence = common.sequence(*call);
        expectedBank = sequence.bank;
        expectedCall = sequence.call;
    } else {
        REQUIRE(shooter.figure);
        const auto call = shooter.figure->throwSound();
        REQUIRE(call);
        const auto sequence = shooter.figure->voice().sequence(*call);
        expectedBank = sequence.bank;
        expectedCall = sequence.call;
    }
    REQUIRE_FALSE(expectedBank.empty());
    std::vector<SoundPlayer::Event> heard;
    std::vector<PacketTransport::Event> heldAudio;
    const auto holdAudio = [&] {
        auto& incoming = pair.wire.guestWire.events;
        for (auto event = incoming.begin(); event != incoming.end();) {
            if (event->type == PacketTransport::EventType::Message &&
                AudioSync::recognizes(event->bytes)) {
                heldAudio.push_back(*event);
                event = incoming.erase(event);
            } else {
                ++event;
            }
        }
    };
    std::set<ShotIdentity> nativeReleases;
    {
        const SoundPlayer::Capture observe(&guestSounds, [&](const auto& event) {
            if (event.kind == SoundPlayer::Event::Kind::Play && event.bank == expectedBank &&
                event.call == expectedCall) {
                heard.push_back(event);
            }
        });
        // Hold only reliable sound commands. Native simulation, guest input and
        // authoritative visual checkpoints continue normally. This isolates a
        // predicted mixer start from accidentally hearing the confirmed event.
        // The long hold outlives the rollback window: never-predicted authority
        // must still play, while an already-heard predicted cue must stay silent.
        for (s32 tick = 0; tick < holdTicks; ++tick) {
            REQUIRE(pair.host.update({}) != OnlinePlay::Phase::Failed);
            holdAudio();
            SessionInputs::Frame input;
            if (tick == 0) {
                input[0].attack = true;
                input[0].attackPressed = true;
            }
            REQUIRE(pair.guest.update(input) != OnlinePlay::Phase::Failed);
            pair.draw();
            mix();
            if (const auto* state = pair.host.shown()) {
                for (const auto& projectile : state->projectiles) {
                    if (projectile.rollback && projectile.rollback->origin.seat == kSeat &&
                        projectile.rollback->origin.ordinal == 0) {
                        nativeReleases.insert(projectile.rollback->origin);
                    }
                }
            }
        }
        REQUIRE(nativeReleases.size() == 1);
        REQUIRE(heard.size() == (predict ? 1 : 0));
        const auto attributed = std::ranges::count_if(heldAudio, [&](const auto& event) {
            REQUIRE(event.bytes.size() == AudioSync::kPacketBytes);
            if (event.bytes[27] == 0) {
                return false;
            }
            CHECK(event.bytes[104] == kSeat);
            CHECK(event.bytes[106] == 0); // One launch sound, not one per fan member.
            return true;
        });
        REQUIRE(attributed == 1);
        // Preserve every reliable cue and its original sequence order.
        auto& incoming = pair.wire.guestWire.events;
        incoming.insert(incoming.begin(), heldAudio.begin(), heldAudio.end());
        for (s32 tick = 0; tick < 30; ++tick) {
            pair.step();
            pair.draw();
            mix();
        }
        REQUIRE(heard.size() == 1);
        CHECK(heard.front().value == LevelSoundscape::kStepVolume);
        CHECK(heard.front().category == SoundCategory::Effects);
        CHECK(heard.front().after == kNoSound);
    }
    CHECK_FALSE(pair.guest.hostScene());
    pair.guest.close();
    pair.host.close();
}

TEST_CASE("guest rune ceremony shows the wizard and caption then returns to gameplay",
          "[netplay][online-play][assets][guest-presentation]") {
    RuntimePair pair(0, false, true);
    pair.open();
    bool wizard = false;
    bool caption = false;
    bool placement = false;
    for (usize tick = 0; tick < 2400; ++tick) {
        CAPTURE(tick);
        pair.step();
        pair.draw();
        const auto* shown = pair.guest.shown();
        if (shown == nullptr) {
            continue;
        }
        REQUIRE(shown->hud);
        if (shown->hud->ceremony.kind == HudSnapshot::Ceremony::Kind::Rune) {
            caption = caption || shown->hud->ceremony.letters > 0;
            const auto found = std::ranges::find(shown->projectiles, ProjectileSource::TowerWizard,
                                                 &ProjectileState::source);
            REQUIRE(found != shown->projectiles.end());
            wizard = true;
            CHECK((found->flags & ProjectileState::kAdditive) != 0);
            const auto* host = pair.host.shown();
            if (host->motion.cameraContinuity == shown->motion.cameraContinuity) {
                CHECK(shown->motion.camera.position == host->motion.camera.position);
                CHECK(shown->motion.camera.pitch == host->motion.camera.pitch);
                CHECK(shown->motion.camera.yaw == host->motion.camera.yaw);
            }
        }
        placement = placement || std::ranges::any_of(shown->projectiles, [](const auto& effect) {
                        return effect.source == ProjectileSource::TowerRelic;
                    });
        if (wizard && caption && placement && !pair.host.hostScene()->towerRelics().active() &&
            shown->hud->ceremony.kind == HudSnapshot::Ceremony::Kind::None) {
            break;
        }
    }
    CHECK(wizard);
    CHECK(caption);
    CHECK(placement);
    CHECK_FALSE(pair.host.hostScene()->towerRelics().active());
    REQUIRE(pair.guest.shown());
    CHECK(pair.guest.shown()->hud->ceremony.kind == HudSnapshot::Ceremony::Kind::None);
}

TEST_CASE("online secret visits preserve the parent scene results and local-only earned unlocks",
          "[netplay][online-secret][online-play][assets]") {
    const bool win = GENERATE(false, true);
    const bool autoSave = GENERATE(false, true);
    RuntimePair pair;
    pair.config.game.autoSave = autoSave;
    u16 hostUnlock = 0;
    u16 guestUnlock = 0;
    usize hostAwards = 0;
    usize guestAwards = 0;
    usize saves = 0;
    auto hostContext = pair.context;
    auto guestContext = pair.context;
    hostContext.unlockClasses = [&](u16 mask) {
        hostUnlock |= mask;
        ++hostAwards;
    };
    guestContext.unlockClasses = [&](u16 mask) {
        guestUnlock |= mask;
        ++guestAwards;
    };
    const auto saveParty = [&](std::span<PartyMember> party) {
        ++saves;
        REQUIRE(win);
        REQUIRE(autoSave);
        REQUIRE(party.size() == 2);
        for (auto& member : party) {
            CHECK(member.save.gold >= 375);
            member.slot = static_cast<usize>(100 + member.player);
        }
    };
    hostContext.saveLocalParty = [&](std::span<PartyMember> party) {
        CHECK(pair.host.phase() == OnlinePlay::Phase::Loading);
        CHECK(party[0].player == 1);
        CHECK(party[1].player == 3);
        CHECK(hostAwards == 1);
        saveParty(party);
    };
    guestContext.saveLocalParty = [&](std::span<PartyMember> party) {
        CHECK(pair.guest.phase() == OnlinePlay::Phase::Loading);
        CHECK(party[0].player == 0);
        CHECK(party[1].player == 2);
        CHECK(guestAwards == 1);
        saveParty(party);
    };
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{-36.3671875f, 90.1640625f, -85.0234375f};
    REQUIRE(
        pair.guest.open(pair.guestDevice, guestContext, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, hostContext, pair.wire.hostSession, pair.hostParty,
                           *pair.catalog.byName("B2"), options));
    for (s32 tick = 0; tick < 3600 && pair.host.phase() != OnlinePlay::Phase::Playing; ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.hostScene());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable native scene fixture
    auto* parent = const_cast<PlayScene*>(pair.host.hostScene());
    const auto* parentWorld = parent->world();
    REQUIRE(parentWorld);
    REQUIRE(parentWorld->ref().name == "B2");
    const auto initialEpoch = pair.wire.host.context().epoch;
    for (s32 seat = 0; seat < 4; ++seat) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable native actor fixture
        auto& actor = *const_cast<PlayerActor*>(parent->actor(seat));
        actor.save().gold += 250 + seat;
        parent->awardExperience(seat, 100, true);
    }
    const auto before = parent->levelResults();
    usize portal = 0;
    while (portal < parent->portals().size() && !parent->portals().portal(portal).secret) {
        ++portal;
    }
    REQUIRE(portal < parent->portals().size());
    const auto portalPosition = parent->portals().portal(portal).position;
    SessionInputs::Frame accept;
    for (auto& input : accept) {
        input.menu.select = true;
    }
    for (s32 tick = 0; tick < 1200 && pair.wire.host.context().epoch == initialEpoch; ++tick) {
        for (s32 seat = 0; seat < 4; ++seat) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): native contact fixture
            const_cast<PlayerActor*>(parent->actor(seat))->place(portalPosition);
        }
        pair.step(accept, accept);
    }
    REQUIRE(pair.wire.host.context().transition == MatchTransition::SecretEnter);
    CHECK(pair.wire.host.tick() == 0);
    CHECK(parent->portals().portal(portal).consumed);
    const auto frozenResults = parent->levelResults();
    const auto frozenGeometry = parentWorld->scene().geometry();
    for (s32 tick = 0; tick < 3600 && (pair.host.phase() != OnlinePlay::Phase::Playing ||
                                       pair.guest.phase() != OnlinePlay::Phase::Playing);
         ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.hostScene());
    REQUIRE(pair.host.hostScene()->world()->ref().name == "S4");
    CHECK_FALSE(pair.guest.hostScene());
    CHECK(pair.host.hostScene() != parent);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable native scene fixture
    auto* challenge = const_cast<PlayScene*>(pair.host.hostScene());
    REQUIRE(challenge->challenge().totalCoins() > 0);
    for (s32 tick = 0; tick < 600 && challenge->spawning(); ++tick) {
        pair.step(accept, accept);
        pair.draw();
    }
    REQUIRE_FALSE(challenge->spawning());
    for (s32 tick = 0; tick < 12; ++tick) {
        pair.step();
        pair.draw();
    }
    REQUIRE(pair.guest.shown());
    REQUIRE(pair.guest.shown()->hud);
    CHECK(pair.guest.shown()->hud->hourglass.has_value());
    for (s32 seat = 0; seat < 4; ++seat) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable native actor fixture
        const_cast<PlayerActor*>(challenge->actor(seat))->save().gold += 125;
        challenge->awardExperience(seat, 80, true);
    }
    if (win) {
        const auto& items = challenge->world()->placedItems();
        for (usize item = 0; item < items.size(); ++item) {
            const auto& coin = items.item(item);
            if (coin.subtype != 1 || !coin.visible || coin.taken) {
                continue;
            }
            for (s32 seat = 0; seat < 4; ++seat) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): native pickup contacts
                const_cast<PlayerActor*>(challenge->actor(seat))->place(coin.position);
            }
            pair.step();
            pair.step();
            pair.draw();
        }
        REQUIRE(challenge->challenge().coinsLeft() == 0);
    }
    const auto challengeResults = challenge->levelResults();
    const auto reward = challenge->challenge().rewardMask();
    for (s32 tick = 0;
         tick < 12000 && pair.wire.host.context().transition != MatchTransition::SecretReturn;
         ++tick) {
        pair.step(accept, accept);
        pair.draw();
    }
    REQUIRE(pair.wire.host.context().transition == MatchTransition::SecretReturn);
    CHECK(pair.wire.host.context().challengeWon == win);
    // This is the very same suspended world, not a reload which restores enemies/items.
    const auto heldGeometry = parentWorld->scene().geometry();
    CHECK(heldGeometry.layout == frozenGeometry.layout);
    CHECK(heldGeometry.darken == frozenGeometry.darken);
    REQUIRE(heldGeometry.objects.size() == frozenGeometry.objects.size());
    for (usize index = 0; index < heldGeometry.objects.size(); ++index) {
        CHECK(heldGeometry.objects[index].local == frozenGeometry.objects[index].local);
        CHECK(heldGeometry.objects[index].visible == frozenGeometry.objects[index].visible);
    }
    CHECK(parent->levelResults()[0].totals == frozenResults[0].totals);
    for (s32 tick = 0; tick < 3600 && (pair.host.phase() != OnlinePlay::Phase::Playing ||
                                       pair.guest.phase() != OnlinePlay::Phase::Playing);
         ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    CHECK(pair.host.hostScene() == parent);
    CHECK(parent->world() == parentWorld);
    CHECK(parent->world()->ref().name == "B2");
    CHECK(parent->portals().portal(portal).consumed);
    CHECK_FALSE(parent->secretTravel());
    CHECK(hostUnlock == (win ? reward : 0));
    CHECK(guestUnlock == hostUnlock);
    CHECK(hostAwards == (win ? 1 : 0));
    CHECK(guestAwards == hostAwards);
    CHECK(saves == (win && autoSave ? 2 : 0));
    if (win && autoSave) {
        CHECK(parent->party()[0].slot == 101);
        CHECK(parent->party()[1].slot == 103);
        CHECK_FALSE(parent->party()[2].slot);
        CHECK_FALSE(parent->party()[3].slot);
    }
    for (usize seat = 0; seat < 4; ++seat) {
        const auto result = parent->levelResults()[seat];
        CHECK(result.totals == challengeResults[seat].totals);
        CHECK(result.totals[0] >= before[seat].totals[0] + 125);
        CHECK(result.totals[1] == before[seat].totals[1] + 1);
        CHECK(result.totals[2] > before[seat].totals[2]);
    }
    for (s32 tick = 0; tick < 120; ++tick) {
        pair.step();
        pair.draw();
    }
    CHECK(hostAwards == (win ? 1 : 0));
    CHECK(guestAwards == hostAwards);
    CHECK(pair.wire.host.context() == pair.wire.guest.context());
    CHECK(saves == (win && autoSave ? 2 : 0));
    REQUIRE(pair.guest.shown());
    CHECK(pair.guest.shown()->motion.epoch == pair.wire.host.context().epoch);
}

TEST_CASE("level exit synchronizes results purchases local checkpoints and tower return",
          "[netplay][online-shop][online-play][assets]") {
    RuntimePair pair;
    pair.config.game.expandedShop = true;
    GameConfig guestConfig = pair.config;
    guestConfig.game.expandedShop = false; // The authenticated host owns the shop roster.
    usize hostSaved = 0;
    usize guestSaved = 0;
    auto hostContext = pair.context;
    auto guestContext = pair.context;
    guestContext.config = &guestConfig;
    hostContext.saveLocalParty = [&](std::span<PartyMember> members) {
        ++hostSaved;
        REQUIRE(members.size() == 2);
        CHECK(members[0].player == 1);
        CHECK(members[1].player == 3);
        for (auto& member : members) {
            CHECK(member.slot == static_cast<usize>(10 + member.player));
            CHECK(member.save.moviesSeen == std::vector<std::string>{"PRIVATE"});
            member.slot = static_cast<usize>(100 + member.player); // local slot allocation
        }
    };
    guestContext.saveLocalParty = [&](std::span<PartyMember> members) {
        ++guestSaved;
        REQUIRE(members.size() == 2);
        CHECK(members[0].player == 0);
        CHECK(members[1].player == 2);
        for (const auto& member : members) {
            CHECK(member.slot == static_cast<usize>(10 + member.player));
            CHECK(member.save.moviesSeen == std::vector<std::string>{"PRIVATE"});
        }
    };
    PlayOptions options;
    options.welcome = false;
    REQUIRE(
        pair.guest.open(pair.guestDevice, guestContext, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, hostContext, pair.wire.hostSession, pair.hostParty,
                           *pair.catalog.byName("G1"), options));
    for (s32 tick = 0; tick < 3600 && pair.host.phase() != OnlinePlay::Phase::Playing; ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.hostScene());
    const auto& exits = pair.host.hostScene()->portals();
    usize exit = 0;
    // Authored normal exit tags point at the next stage; leaveBy redirects the journey to Tower.
    while (exit < exits.size() && (exits.portal(exit).secret || !exits.portal(exit).destination)) {
        ++exit;
    }
    REQUIRE(exit < exits.size());
    for (s32 seat = 0; seat < 4; ++seat) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable scene test fixture
        auto& actor = *const_cast<PlayerActor*>(pair.host.hostScene()->actor(seat));
        actor.save().gold = 10000 + seat;
        actor.place(exits.portal(exit).position);
    }
    for (s32 tick = 0; tick < 1800 && (pair.host.phase() != OnlinePlay::Phase::Shopping ||
                                       pair.guest.phase() != OnlinePlay::Phase::Shopping);
         ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.shopScene());
    REQUIRE(pair.guest.shopScene());
    CHECK(pair.host.shopScene()->session().catalog().items().size() == 39);
    CHECK(pair.guest.shopScene()->session().catalog().items().size() == 39);
    CHECK(hostSaved == 0);
    CHECK(guestSaved == 0);
    for (const auto& lane : pair.guest.shopScene()->session().lanes()) {
        CHECK(lane.tally.results().totals[0] == 10000 + lane.member.player);
    }
    const auto allShopping = [&] {
        return pair.host.shopScene() != nullptr &&
               std::ranges::all_of(
                   pair.host.shopScene()->session().lanes(),
                   [](const ShopLane& lane) { return lane.phase == ShopPhase::Shopping; });
    };
    for (s32 tick = 0; tick < 1500 && !allShopping(); ++tick) {
        SessionInputs::Frame hostInputs;
        SessionInputs::Frame guestInputs;
        if (tick % 60 == 0) {
            hostInputs[1].menu.select = hostInputs[3].menu.select = true;
            guestInputs[0].menu.select = guestInputs[2].menu.select = true;
        }
        pair.step(hostInputs, guestInputs);
    }
    REQUIRE(allShopping());
    for (s32 tick = 0; tick < 60; ++tick) {
        pair.step();
    }
    const auto lane = [&](const OnlinePlay& play, usize seat) -> const ShopLane& {
        REQUIRE(play.shopScene());
        return play.shopScene()->session().lanes().at(seat);
    };
    const auto& catalog = pair.host.shopScene()->session().catalog().items();
    for (usize attempt = 0;
         attempt < catalog.size() && catalog[lane(pair.host, 2).cursor].type != 3; ++attempt) {
        SessionInputs::Frame guestInputs;
        guestInputs[0].menu.down = true;
        pair.step({}, guestInputs);
        for (s32 tick = 0; tick < 45; ++tick) {
            pair.step();
        }
    }
    REQUIRE(catalog[lane(pair.host, 2).cursor].type == 3);
    const auto price = catalog[lane(pair.host, 2).cursor].price;
    const auto beforeGold = lane(pair.host, 2).member.save.gold;
    SessionInputs::Frame buy;
    buy[0].menu.select = true;
    // A slow peer's stale page/row confirmations cannot become purchases on the current row.
    REQUIRE(pair.wire.guest.shop().sample(2, buy[0].menu, -1, static_cast<u8>(ShopPhase::Tally),
                                          lane(pair.host, 2).cursor));
    for (s32 tick = 0; tick < 30; ++tick) {
        pair.step();
    }
    CHECK(lane(pair.host, 2).member.save.gold == beforeGold);
    REQUIRE(
        pair.wire.guest.shop().sample(2, buy[0].menu, -1, static_cast<u8>(ShopPhase::Shopping), 0));
    for (s32 tick = 0; tick < 30; ++tick) {
        pair.step();
    }
    CHECK(lane(pair.host, 2).member.save.gold == beforeGold);
    pair.step({}, buy);
    for (s32 tick = 0; tick < 30; ++tick) {
        pair.step();
    }
    CHECK(lane(pair.host, 2).member.save.gold == beforeGold - price);
    CHECK(lane(pair.guest, 2).member.save.gold == beforeGold - price);
    CHECK(lane(pair.host, 2).member.save.progress().inventory.potions ==
          lane(pair.guest, 2).member.save.progress().inventory.potions);
    // Finish the host's seats first. A remote machine still shopping holds the barrier.
    for (s32 tick = 0; tick < 900; ++tick) {
        SessionInputs::Frame inputs;
        if (tick % 60 == 0) {
            inputs[1].menu.start = inputs[3].menu.start = true;
        } else if (tick % 60 == 30) {
            inputs[1].menu.select = inputs[3].menu.select = true;
        }
        pair.step(inputs);
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Shopping);
    CHECK(hostSaved == 0);
    for (s32 tick = 0; tick < 3600; ++tick) {
        SessionInputs::Frame inputs;
        if (tick % 60 == 0) {
            inputs[0].menu.start = inputs[2].menu.start = true;
        } else if (tick % 60 == 30) {
            inputs[0].menu.select = inputs[2].menu.select = true;
        }
        pair.step({}, inputs);
        if (pair.host.phase() == OnlinePlay::Phase::Playing &&
            pair.guest.phase() == OnlinePlay::Phase::Playing) {
            break;
        }
    }
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.host.hostScene());
    CHECK(pair.host.hostScene()->world()->isTower());
    CHECK(pair.wire.host.context() == pair.wire.guest.context());
    CHECK(hostSaved == 1);
    CHECK(guestSaved == 1);
    CHECK(pair.host.hostScene()->party()[0].slot == 101);
    CHECK(pair.host.hostScene()->party()[1].slot == 103);
    CHECK_FALSE(pair.host.hostScene()->party()[2].slot);
    CHECK_FALSE(pair.host.hostScene()->party()[3].slot);
    CHECK(pair.guestParty.localMembers()[0].save.gold == beforeGold - price);
}

TEST_CASE("an online tower portal loads the same stage and retained party on both machines",
          "[netplay][online-play][online-travel][online-departure][assets]") {
    RuntimePair pair;
    PlayOptions options;
    options.position = Vec3{45.1f, -6.5f, -112.7f};
    pair.open(LevelRef::tower(), options);
    const auto before = pair.host.hostScene()->party();
    const auto initialEpoch = pair.wire.host.context().epoch;
    std::set<u8> departingSeats;
    bool finishedDeparture = false;
    for (s32 tick = 0; tick < 3600; ++tick) {
        pair.step();
        pair.draw();
        if (const auto* shown = pair.guest.shown();
            shown != nullptr && shown->motion.epoch == initialEpoch) {
            for (usize seat = 0; seat < shown->players.size(); ++seat) {
                if (shown->players[seat] && shown->players[seat]->portalPhase) {
                    const auto phase = *shown->players[seat]->portalPhase;
                    if (phase > 0 && phase < 1) {
                        departingSeats.insert(static_cast<u8>(seat));
                        CHECK(pair.guest.replica()->actors().visiblePlayers() == before.size());
                        CHECK(std::ranges::any_of(pair.guestDevice.draws, [](const auto& draw) {
                            return draw.state.maskedTexture != nullptr;
                        }));
                    } else if (phase == 1) {
                        finishedDeparture = true;
                        CHECK(pair.guest.replica()->actors().visiblePlayers() == 0);
                    }
                }
            }
        }
        if (pair.wire.host.context().epoch > initialEpoch &&
            pair.host.phase() == OnlinePlay::Phase::Playing &&
            pair.guest.phase() == OnlinePlay::Phase::Playing) {
            break;
        }
    }
    REQUIRE(pair.wire.host.context().epoch > initialEpoch);
    CHECK(departingSeats.size() == before.size());
    CHECK(finishedDeparture);
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    CHECK(pair.host.hostScene()->world()->ref().name == "G1");
    CHECK(OnlinePlay::levelOf(pair.catalog, pair.wire.guest.context().scene)->name == "G1");
    const auto after = pair.host.hostScene()->party();
    REQUIRE(after.size() == before.size());
    for (usize i = 0; i < before.size(); ++i) {
        CHECK(after[i].player == before[i].player);
        CHECK(after[i].save.name == before[i].save.name);
        CHECK(after[i].save.gold == before[i].save.gold);
        CHECK(after[i].save.character == before[i].save.character);
    }
    for (s32 tick = 0; tick < 90; ++tick) {
        pair.step();
        pair.draw();
    }
    REQUIRE(pair.guest.shown());
    CHECK(pair.guest.shown()->motion.epoch == pair.wire.host.context().epoch);
    // Loading a stage and drawing its walls is not enough: its exit artwork
    // must survive travel into the independently loaded guest resource catalog.
    pair.hostDevice.draws.clear();
    pair.host.hostScene()->portals().draw(pair.hostDevice, Mat4{1}, {});
    REQUIRE_FALSE(pair.hostDevice.draws.empty());
    for (const auto& expected : pair.hostDevice.draws) {
        const auto& texture = dynamic_cast<const test::FakeTexture&>(*expected.texture);
        CHECK(std::ranges::any_of(pair.guestDevice.draws, [&](const auto& actual) {
            return actual.vertices.size() == expected.vertices.size() &&
                   actual.blend() == expected.blend() &&
                   dynamic_cast<const test::FakeTexture&>(*actual.texture).pixels == texture.pixels;
        }));
    }
}

TEST_CASE("G1 plays through arrival and accepts remote movement and attacks",
          "[netplay][online-play][assets]") {
    RuntimePair pair;
    const auto level = pair.catalog.byName("G1");
    REQUIRE(level);
    pair.open(*level);
    for (s32 tick = 0; tick < 600; ++tick) {
        pair.step();
        pair.draw();
    }
    REQUIRE(pair.host.shown());
    REQUIRE(pair.guest.shown());
    for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
        const auto& state = pair.guest.shown()->players[seat];
        REQUIRE(state);
        REQUIRE(state->shadow);
        REQUIRE(pair.host.shown()->players[seat]->shadow);
        const auto* figure = pair.guest.replica()->actors().playerFigure(static_cast<u8>(seat));
        REQUIRE(figure);
        REQUIRE(figure->hasShadow());
        const auto& camera = pair.guest.shown()->motion.camera;
        const Mat4 clip =
            camera.clipTransform(pair.config.horizontalFovRadians(), 1280, 720, Mat4{1});
        const auto& shadow = *state->shadow;
        const auto rendered = pair.guestDevice.draws;
        pair.guestDevice.draws.clear();
        figure->drawShadow(pair.guestDevice, clip, camera.position, shadow.ground, shadow.normal,
                           pair.host.hostScene()->world()->lighting(), shadow.alpha);
        REQUIRE_FALSE(pair.guestDevice.draws.empty());
        for (const auto& expected : pair.guestDevice.draws) {
            CHECK_FALSE(expected.state.depthWrite);
            CHECK(expected.state.depthTest);
            CHECK(std::ranges::any_of(rendered, [&](const auto& actual) {
                return actual.texture == expected.texture &&
                       actual.transform == expected.transform &&
                       actual.vertices.size() == expected.vertices.size() &&
                       actual.vertices.front().position == expected.vertices.front().position &&
                       actual.vertices.front().color == expected.vertices.front().color &&
                       !actual.state.depthWrite && actual.state.depthTest;
            }));
        }
        pair.guestDevice.draws = rendered;
    }
    const auto before = pair.host.shown()->motion.players[2]->position;
    SessionInputs::Frame guest;
    // Guest device 0 owns room seat 2. W follows the transmitted world direction,
    // regardless of the host camera or the guest's older interpolated position.
    guest[0].move = {{0, 1}, 1};
    guest[0].aimDirection = Vec3{1, 0, 0};
    for (s32 tick = 0; tick < 45; ++tick) {
        pair.step({}, guest);
        pair.draw();
        if (tick > 10) {
            const auto* actor = pair.host.hostScene()->actor(2);
            REQUIRE(actor);
            CHECK(glm::distance(actor->facing(), Vec3{1, 0, 0}) < 0.001f);
        }
    }
    CHECK(glm::distance(pair.host.shown()->motion.players[2]->position, before) > 0.05f);
    guest[0].move = {};
    guest[0].attack = true;
    usize projectiles = 0;
    for (s32 tick = 0; tick < 180; ++tick) {
        pair.step({}, guest);
        pair.draw();
        if (const auto* shown = pair.guest.shown()) {
            projectiles = std::max(projectiles, shown->projectiles.size());
        }
    }
    CHECK(projectiles > 0);
    CHECK(pair.host.phase() == OnlinePlay::Phase::Playing);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Playing);
}

TEST_CASE("online entry shows the native map before either peer can simulate",
          "[netplay][online-entry][assets]") {
    RuntimePair pair;
    const auto level = pair.catalog.byName("G1");
    REQUIRE(level);
    REQUIRE(
        pair.guest.open(pair.guestDevice, pair.context, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, pair.context, pair.wire.hostSession, pair.hostParty,
                           *level));
    for (s32 tick = 0; tick < 120; ++tick) {
        pair.step();
        pair.draw();
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Loading);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Loading);
    CHECK(pair.wire.host.tick() == 0);
    CHECK_FALSE(pair.host.hostScene());
    REQUIRE_FALSE(pair.guestDevice.draws.empty());
    CHECK(pair.guestDevice.draws.front().vertices.front().color == Color::black());
}

TEST_CASE("the guest renders every native golden arrival circle",
          "[netplay][online-arrival][assets]") {
    RuntimePair pair;
    pair.open();
    for (s32 tick = 0; tick < 12; ++tick) {
        pair.step();
        pair.draw();
    }
    // ClientClock intentionally presents several ticks behind the host. Exact
    // geometry/flipbook parity at the same tick is checked in ReplicaProjectilesTests.
    REQUIRE(pair.guest.shown());
    CHECK(std::ranges::count_if(pair.guest.shown()->projectiles, [](const auto& effect) {
              return effect.source == ProjectileSource::Arrival;
          }) == 4);
    for (const auto& effect : pair.guest.shown()->projectiles) {
        if (effect.source == ProjectileSource::Arrival) {
            REQUIRE(effect.instance >= 1);
            REQUIRE(effect.instance <= 4);
            CHECK(Vec3{effect.placement[3]} ==
                  pair.host.hostScene()->actor(static_cast<s32>(effect.instance - 1))->position());
        }
    }
    REQUIRE(pair.guest.pause());
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
        pair.draw();
    }
    const auto before = pair.wire.host.tick();
    for (s32 tick = 0; tick < 30; ++tick) {
        pair.step();
        pair.draw();
    }
    CHECK(pair.wire.host.tick() > before);
    REQUIRE(pair.guest.resume());
    for (s32 tick = 0; tick < 150; ++tick) {
        pair.step();
        pair.draw();
    }
    CHECK(std::ranges::none_of(pair.guest.shown()->projectiles, [](const auto& effect) {
        return effect.source == ProjectileSource::Arrival;
    }));
}

TEST_CASE("entry movies keep playing until all four local and remote players vote to skip",
          "[netplay][online-entry][assets]") {
    RuntimePair pair;
    REQUIRE(
        pair.guest.open(pair.guestDevice, pair.context, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, pair.context, pair.wire.hostSession, pair.hostParty,
                           *pair.catalog.byName("G1")));
    for (s32 tick = 0; tick < 900 && pair.guest.entryPhase() != OnlineLevelEntry::Phase::Movie;
         ++tick) {
        pair.step();
        CHECK(pair.wire.host.tick() == 0);
    }
    REQUIRE(pair.host.entryPhase() == OnlineLevelEntry::Phase::Movie);
    REQUIRE(pair.guest.entryPhase() == OnlineLevelEntry::Phase::Movie);
    const auto uploads = pair.guestDevice.textureUpdates;
    // An unassigned device does not get to skip either machine's movie.
    SessionInputs::Frame keys;
    keys[0].menu.start = true;
    keys[0].movieSkipPressed = true;
    for (s32 tick = 0; tick < 15; ++tick) {
        pair.step(keys);
    }
    CHECK(pair.host.entryPhase() == OnlineLevelEntry::Phase::Movie);
    CHECK(pair.guestDevice.textureUpdates > uploads);
    keys[1].menu.pointerPressed = true;
    keys[1].movieSkipPressed = true;
    pair.step(keys);
    REQUIRE(pair.host.entryPhase() == OnlineLevelEntry::Phase::Movie);
    CHECK(pair.guest.entryPhase() == OnlineLevelEntry::Phase::Movie);
    for (s32 tick = 0; tick < 60; ++tick) {
        pair.step();
        CHECK(pair.wire.host.tick() == 0);
        CHECK(pair.host.phase() == OnlinePlay::Phase::Loading);
    }
    SessionInputs::Frame guestKeys;
    guestKeys[0].menu.select = true;
    guestKeys[0].movieSkipPressed = true;
    pair.step({}, guestKeys);
    keys = {};
    keys[3].menu.start = true;
    keys[3].movieSkipPressed = true;
    pair.step(keys);
    for (s32 tick = 0; tick < 15; ++tick) {
        pair.step();
        CHECK(pair.host.entryPhase() == OnlineLevelEntry::Phase::Movie);
        CHECK(pair.guest.entryPhase() == OnlineLevelEntry::Phase::Movie);
        CHECK(pair.wire.host.tick() == 0);
    }
    CHECK(pair.wire.host.movieSkipVotes() == 7);
    // The last guest controller owns room seat 3, not physical device 3.
    guestKeys = {};
    guestKeys[2].menu.start = true;
    guestKeys[2].movieSkipPressed = true;
    pair.step({}, guestKeys);
    for (s32 tick = 0; tick < 15 && pair.host.phase() != OnlinePlay::Phase::Playing; ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    bool circle = false;
    REQUIRE(pair.host.hostScene()->arrival().camera().active());
    const auto openingCamera = pair.host.hostScene()->viewCamera();
    bool cameraMoved = false;
    bool title = false;
    for (s32 tick = 0; tick < 240; ++tick) {
        pair.step();
        pair.draw();
        if (const auto* shown = pair.guest.shown()) {
            circle = circle || std::ranges::any_of(shown->projectiles, [](const auto& effect) {
                         return effect.source == ProjectileSource::Arrival;
                     });
            title = title || (shown->hud && shown->hud->screen.titleScale > 0);
            if (tick < 90) {
                CHECK(shown->motion.camera.position == openingCamera.position);
            }
            cameraMoved = cameraMoved || shown->motion.camera.position != openingCamera.position;
        }
    }
    CHECK(circle);
    CHECK(title);
    CHECK(cameraMoved);
    REQUIRE(pair.guest.pause());
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
    }
    REQUIRE(pair.guest.resume());
    for (s32 tick = 0; tick < 60; ++tick) {
        pair.step();
        pair.draw();
        CHECK_FALSE(pair.host.entryVisible());
        CHECK_FALSE(pair.guest.entryVisible());
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Playing);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Playing);
}

TEST_CASE("an online boss entry camera needs every player's vote to skip its hold",
          "[netplay][online-entry][movie-skip][assets]") {
    RuntimePair pair;
    pair.open(*pair.catalog.byName("G5"));
    const auto& camera = pair.host.hostScene()->arrival().camera();
    REQUIRE(camera.mode() == StartCamera::Mode::Legacy);
    for (s32 tick = 0; tick < 180 && camera.ticksLeft() > 35; ++tick) {
        pair.step();
    }
    REQUIRE(camera.phase() == StartCamera::Phase::Hold);
    REQUIRE(camera.ticksLeft() > 20);
    SessionInputs::Frame hostKeys;
    SessionInputs::Frame guestKeys;
    hostKeys[1].menu.select = true;
    hostKeys[3].menu.select = true;
    guestKeys[0].menu.select = true;
    pair.step(hostKeys, guestKeys);
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
        CHECK(camera.phase() == StartCamera::Phase::Hold);
    }
    guestKeys = {};
    guestKeys[2].menu.select = true;
    pair.step({}, guestKeys);
    for (s32 tick = 0; tick < 10 && camera.phase() == StartCamera::Phase::Hold; ++tick) {
        pair.step();
    }
    CHECK(camera.phase() == StartCamera::Phase::Ride);
    CHECK(camera.ticksLeft() == 0);
}

TEST_CASE("disconnect during an entry movie releases the local presentation",
          "[netplay][online-entry][assets]") {
    RuntimePair pair;
    REQUIRE(
        pair.guest.open(pair.guestDevice, pair.context, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, pair.context, pair.wire.hostSession, pair.hostParty,
                           *pair.catalog.byName("G1")));
    for (s32 tick = 0; tick < 900 && pair.guest.entryPhase() != OnlineLevelEntry::Phase::Movie;
         ++tick) {
        pair.step();
    }
    REQUIRE(pair.guest.entryPhase() == OnlineLevelEntry::Phase::Movie);
    pair.wire.guestSession.leave();
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.host.update({});
        pair.guest.update({});
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Failed);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Failed);
    CHECK_FALSE(pair.host.entryVisible());
    CHECK_FALSE(pair.guest.entryVisible());
    CHECK(pair.wire.host.tick() == 0);
}

TEST_CASE("native online encounter movies fit inside the bounded loading barrier",
          "[netplay][online-entry][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const AssetLocator assets(root);
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize movies = 0;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto level = catalog.byName(name);
            REQUIRE(level);
            if (level->isTower()) {
                continue;
            }
            WorldData data;
            REQUIRE(data.load(root / level->worldDataFile()));
            const auto* info = data.level(name);
            REQUIRE(info);
            if (info->movie.empty()) {
                continue;
            }
            CAPTURE(name, info->movie);
            const auto file = assets.find("VQMOVIES/" + info->movie + ".avi");
            REQUIRE(file);
            MoviePlayback movie;
            REQUIRE(movie.open(*file));
            const auto& video = movie.info();
            REQUIRE(video.framesPerSecond > 0);
            const auto seconds = static_cast<f64>(video.frameCount) / video.framesPerSecond;
            // Longest possible route, the movie, and 30 seconds for resource loading.
            CHECK(seconds + 10 + 30 < MatchSession::kLoadTimeout);
            ++movies;
        }
    }
    CHECK(movies > 0);
}

TEST_CASE("missing native encounter media fails closed instead of entering unseen gameplay",
          "[netplay][online-entry][assets]") {
    RuntimePair pair;
    const AssetLocator missing(pair.root / "missing-movies");
    auto context = pair.context;
    context.assets = &missing;
    OnlineLevelEntry entry;
    const auto members = pair.hostParty.members(pair.wire.hostSession);
    REQUIRE(members);
    entry.open(pair.hostDevice, context, *pair.catalog.byName("G1"), *members);
    for (s32 tick = 0; tick < 900 && entry.phase() != OnlineLevelEntry::Phase::Failed; ++tick) {
        pair.hostDevice.draws.clear();
        entry.render(pair.hostDevice, Mat4{1}, 1280, 720);
        entry.update(pair.hostDevice, context, true);
    }
    CHECK(entry.phase() == OnlineLevelEntry::Phase::Failed);
    entry.close();
    CHECK_FALSE(entry.visible());
}

TEST_CASE("online linked boss entrances carry stage rewards and private local save state",
          "[netplay][online-play][online-travel][assets]") {
    const std::string stage = GENERATE(std::string{"E1"}, std::string{"F1"});
    const std::string arena = stage == "E1" ? "E2" : "F2";
    RuntimePair pair;
    const auto level = pair.catalog.byName(stage);
    REQUIRE(level);
    PlayOptions options;
    options.position = stage == "E1" ? Vec3{0.25f, 5.12f, -175.57f} : Vec3{-21.75f, 5.04f, -421.0f};
    pair.open(*level, options);
    const auto original = pair.host.hostScene()->party();
    for (s32 seat = 0; seat < 4; ++seat) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable scene test fixture
        auto& save = const_cast<PlayerActor*>(pair.host.hostScene()->actor(seat))->save();
        save.gold += 250 + seat;
        save.progress().experience += 500;
        save.progress().inventory.keys = 8;
        save.progress().inventory.potions = {1, 3};
        save.progress().relics.runes = 3;
    }
    const auto initialEpoch = pair.wire.host.context().epoch;
    for (s32 tick = 0; tick < 3600; ++tick) {
        pair.step();
        pair.draw();
        if (pair.wire.host.context().epoch > initialEpoch &&
            pair.host.phase() == OnlinePlay::Phase::Playing &&
            pair.guest.phase() == OnlinePlay::Phase::Playing) {
            break;
        }
    }
    REQUIRE(pair.wire.host.context().epoch > initialEpoch);
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.host.hostScene()->world()->ref().name == arena);
    REQUIRE(pair.wire.guest.travelParty());
    const auto carried = pair.host.hostScene()->party();
    REQUIRE(carried.size() == 4);
    const auto results = pair.host.hostScene()->levelResults();
    REQUIRE(results.size() == 4);
    for (usize seat = 0; seat < carried.size(); ++seat) {
        const auto& member = carried[seat];
        const auto& profile = (*pair.wire.guest.travelParty())[seat];
        REQUIRE(profile);
        CHECK(member.save.gold == 250 + static_cast<s32>(seat));
        CHECK(profile->gold == member.save.gold);
        CHECK(profile->progress.inventory.keys == 8);
        CHECK(profile->progress.inventory.potions == std::vector<s32>{1, 3});
        CHECK(profile->progress.relics.runes == 3);
        CHECK(member.save.progress().levels.beaten[static_cast<usize>(level->realmId)] != 0);
        CHECK(member.slot == original[seat].slot);
        CHECK(member.save.classUnlock == original[seat].save.classUnlock);
        CHECK(member.save.moviesSeen == original[seat].save.moviesSeen);
        REQUIRE(member.resultsCheckpoint);
        CHECK(member.resultsCheckpoint->gold == 0);
        CHECK(results[seat].totals[0] == 250 + static_cast<s32>(seat));
        CHECK(results[seat].totals[2] == 500);
        const auto other = static_cast<usize>((member.save.character + 1) % kClassCount);
        CHECK(member.save.classes[other].experience ==
              original[seat].save.classes[other].experience);
        CHECK(profile->gameplayCopy().classes[other].experience == 0);
        CHECK(profile->gameplayCopy().classUnlock == 0);
    }
    // A resume after travel must retain the newly loaded scene and its checkpoint.
    REQUIRE(pair.guest.pause());
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
    }
    REQUIRE(pair.guest.resume());
    for (s32 tick = 0; tick < 90; ++tick) {
        pair.step();
        pair.draw();
    }
    REQUIRE(pair.guest.shown());
    CHECK(pair.host.hostScene()->world()->ref().name == arena);
    CHECK(pair.host.hostScene()->actor(0)->save().gold == 250);
    CHECK(pair.guest.shown()->motion.epoch == pair.wire.host.context().epoch);
}

TEST_CASE("online scene IDs round-trip every playable native level without accepting paths",
          "[netplay][online-play][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    std::set<u32> seen;
    const auto tower = OnlinePlay::sceneId(catalog, LevelRef::tower());
    REQUIRE(tower);
    seen.insert(*tower);
    CHECK(OnlinePlay::levelOf(catalog, *tower) == LevelRef::tower());
    for (const auto& realm : catalog.realms()) {
        if (realm.id == LevelRef::kTowerRealm) {
            continue;
        }
        for (const auto& name : realm.levels) {
            const auto level = catalog.byName(name);
            REQUIRE(level);
            const auto id = OnlinePlay::sceneId(catalog, *level);
            REQUIRE(id);
            CHECK(seen.insert(*id).second);
            CHECK(OnlinePlay::levelOf(catalog, *id) == level);
            auto spoof = *level;
            spoof.directory = "../../foreign";
            CHECK_FALSE(OnlinePlay::sceneId(catalog, spoof));
        }
    }
    for (u32 id = 0; id <= 65536; ++id) {
        CHECK(OnlinePlay::levelOf(catalog, id).has_value() == seen.contains(id));
    }
}

TEST_CASE("the online scene owner drives four native actors while the guest only presents",
          "[netplay][online-play][assets]") {
    RuntimePair pair;
    pair.open();
    REQUIRE(pair.host.hostScene());
    REQUIRE(pair.host.hostScene()->actorCount() == 4);
    CHECK_FALSE(pair.guest.hostScene());
    REQUIRE(pair.guest.replica());
    const auto created = pair.guestDevice.texturesCreated;
    SessionInputs::Frame hostInputs;
    SessionInputs::Frame guestInputs;
    // Non-selected physical slots cannot accidentally drive any party member.
    hostInputs[0].move = {{1, 0}, 1};
    guestInputs[1].move = {{1, 0}, 1};
    hostInputs[1].attack = true;
    guestInputs[2].attack = true;
    usize projectiles = 0;
    for (s32 tick = 0; tick < 240; ++tick) {
        CAPTURE(tick);
        pair.step(hostInputs, guestInputs);
        pair.draw();
        if (const auto* shown = pair.guest.shown()) {
            CHECK(shown->motion.players[0]);
            CHECK(shown->motion.players[1]);
            CHECK(shown->motion.players[2]);
            CHECK(shown->motion.players[3]);
            projectiles = std::max(projectiles, shown->projectiles.size());
        }
    }
    CHECK(projectiles > 0);
    CHECK_FALSE(pair.guestDevice.draws.empty());
    CHECK(pair.guestDevice.texturesCreated == created);
    REQUIRE(pair.host.shown());
    REQUIRE(pair.guest.shown());
    CHECK(pair.guest.shown()->motion.tick <= pair.host.shown()->motion.tick);
    CHECK(pair.guest.hostScene() == nullptr);

    REQUIRE(pair.guest.pause());
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Paused);
    const auto pausedTick = pair.wire.host.tick();
    const auto oldEpoch = pair.wire.host.context().epoch;
    for (s32 tick = 0; tick < 180; ++tick) {
        pair.step(hostInputs, guestInputs);
        pair.draw();
    }
    CHECK(pair.wire.host.tick() > pausedTick);
    REQUIRE(pair.guest.resume());
    CHECK_FALSE(pair.host.resume());
    for (s32 tick = 0; tick < 30; ++tick) {
        pair.step();
        pair.draw();
    }
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.shown());
    CHECK(pair.guest.shown()->motion.epoch == oldEpoch);
    CHECK(pair.guestDevice.texturesCreated == created);
    pair.wire.guestSession.leave();
    CHECK(pair.guest.update({}) == OnlinePlay::Phase::Failed);
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.host.update({});
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Failed);
    const auto stoppedTick = pair.wire.host.tick();
    pair.host.update(hostInputs);
    CHECK(pair.wire.host.tick() == stoppedTick);
}

TEST_CASE("online pause captures only the owning device and never stops other seats",
          "[netplay][online-play][online-pause][assets]") {
    RuntimePair pair;
    pair.open();
    for (s32 tick = 0; tick < 200; ++tick) {
        pair.step();
    }
    REQUIRE_FALSE(pair.host.pause(0)); // unassigned physical slot, not room seat zero
    REQUIRE_FALSE(pair.guest.pause(1));
    REQUIRE(pair.host.pause(1));  // room seat zero
    REQUIRE(pair.guest.pause(2)); // room seat three
    const auto epoch = pair.wire.host.context().epoch;
    const auto before = pair.wire.host.tick();
    SessionInputs::Frame controls;
    for (auto& input : controls) {
        input.attack = true;
    }
    std::set<s32> shooters;
    for (s32 tick = 0; tick < 180; ++tick) {
        pair.step(controls, controls);
        pair.draw();
        const auto& missiles = pair.host.hostScene()->missiles();
        for (usize i = 0; i < missiles.count(); ++i) {
            shooters.insert(missiles.missile(i).owner);
        }
    }
    CHECK(pair.wire.host.tick() >= before + 175);
    CHECK(pair.wire.host.context().epoch == epoch);
    CHECK(pair.wire.host.phase() == MatchSession::Phase::Running);
    CHECK(pair.wire.guest.phase() == MatchSession::Phase::Running);
    CHECK(shooters == std::set<s32>{1, 2});
    REQUIRE(pair.host.resume(1));
    REQUIRE(pair.guest.resume(2));
    for (s32 tick = 0; tick < 180; ++tick) {
        pair.step(controls, controls);
        const auto& missiles = pair.host.hostScene()->missiles();
        for (usize i = 0; i < missiles.count(); ++i) {
            shooters.insert(missiles.missile(i).owner);
        }
    }
    CHECK(shooters == std::set<s32>{0, 1, 2, 3});
}

TEST_CASE("a failed guest native load cannot release the host simulation barrier",
          "[netplay][online-play][assets]") {
    RuntimePair pair;
    auto missing = pair.context;
    missing.unpackedRoot /= "missing-online-assets";
    REQUIRE(pair.guest.open(pair.guestDevice, missing, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, pair.context, pair.wire.hostSession, pair.hostParty));
    CHECK(pair.host.update({}) == OnlinePlay::Phase::Loading);
    CHECK(pair.guest.update({}) == OnlinePlay::Phase::Failed);
    CHECK(pair.guest.failure() == OnlinePlay::Failure::Assets);
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.guest.update({});
        pair.host.update({});
    }
    CHECK(pair.wire.host.tick() == 0);
    CHECK(pair.host.phase() == OnlinePlay::Phase::Failed);
    CHECK_FALSE(pair.guest.shown());
}
TEST_CASE("all sixteen online classes can repeatedly throw and recover on an independent client",
          "[netplay][online-play][assets]") {
    for (s32 offset = 0; offset < 16; offset += 4) {
        CAPTURE(offset);
        RuntimePair pair(offset);
        pair.open();
        SessionInputs::Frame inputs;
        for (auto& input : inputs) {
            input.attack = true;
        }
        for (s32 tick = 0; tick < 360; ++tick) {
            CAPTURE(tick);
            pair.step(inputs, inputs);
            pair.draw();
        }
        REQUIRE(pair.guest.shown());
        CHECK(pair.host.outcome() == PlayReplication::Result::Advanced);
    }
}

TEST_CASE("guest scene attachment accepts an initial Prepare that overtook local setup",
          "[netplay][online-play][assets]") {
    RuntimePair pair;
    REQUIRE(pair.host.open(pair.hostDevice, pair.context, pair.wire.hostSession, pair.hostParty));
    pair.wire.pump();
    REQUIRE(pair.wire.guest.phase() == MatchSession::Phase::Loading);
    REQUIRE(
        pair.guest.open(pair.guestDevice, pair.context, pair.wire.guestSession, pair.guestParty));
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Playing);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Playing);
}
} // namespace
