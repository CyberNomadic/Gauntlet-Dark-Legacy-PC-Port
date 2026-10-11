#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/ControlProfiles.h"
#include "game/netplay/OnlinePair.h"
#include "game/screens/OnlinePlay.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct FeedbackPair {
    struct Pulse {
        s32 device = -1;
        ControlFeedback cue = ControlFeedback::Damage;
        ControlVibration motors;
    };
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice hostDevice;
    test::FakeRenderDevice guestDevice;
    GameConfig hostConfig;
    GameConfig guestConfig;
    Input guestInput;
    StringTable strings;
    LevelCatalog catalog;
    GameContext hostContext;
    GameContext guestContext;
    std::vector<Pulse> pulses;
    s32 hostStops = 0;
    s32 guestStops = 0;
    test::OnlinePair wire{{}, false, 2, 2};
    OnlineParty hostParty;
    OnlineParty guestParty;
    OnlinePlay host;
    OnlinePlay guest;

    FeedbackPair() {
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        hostContext.config = &hostConfig;
        hostContext.strings = &strings;
        hostContext.levels = &catalog;
        hostContext.unpackedRoot = root;
        // Deliberately no host vibrate callback or physical controller. The host's
        // simulation must still originate cues for its guests.
        hostContext.stopVibration = [this] { ++hostStops; };
        guestContext = hostContext;
        guestContext.config = &guestConfig;
        guestContext.stopVibration = [this] { ++guestStops; };
        for (auto& profile : guestConfig.controls) {
            profile.device = "none";
        }
        guestConfig.controls[0].device = "guest-first";
        guestConfig.controls[2].device = "guest-second";
        guestInput.setPad(7, pad("guest-first"));
        guestInput.setPad(4, pad("guest-second"));
        guestContext.vibrate = [this](s32 device, s32 frames, ControlFeedback cue) {
            const auto motors = controlVibration(guestConfig, guestInput, device, frames, cue);
            if (motors) {
                // A headless capability-checking motor sink, not a hardware test.
                const auto* physical = guestInput.padDevice(motors->pad);
                if (physical != nullptr && physical->rumbleSupported) {
                    pulses.push_back({device, cue, *motors});
                }
            }
        };
        const auto choice = [](s32 device, s32 color) {
            CharacterSave save;
            save.name = "PULSE";
            save.character = 0;
            save.color = color;
            save.progress().health = 9000;
            save.progress().experience = levelExperience(60);
            return PartyMember{device, save};
        };
        REQUIRE(hostParty.select(wire.hostSession, std::array{choice(1, 0), choice(3, 1)}));
        REQUIRE(guestParty.select(wire.guestSession, std::array{choice(0, 2), choice(2, 3)}));
        wire.start();
    }
    static PadSnapshot pad(const char* guid, bool capable = true) {
        PadSnapshot result;
        result.connected = true;
        result.guid = guid;
        result.name = "Headless feedback controller";
        result.rumbleSupported = capable;
        return result;
    }
    void open(const LevelRef& level, Vec3 position = {18.8f, 0.2f, 0.7f}) {
        PlayOptions options;
        options.welcome = false;
        options.position = position;
        options.yaw = kPi / 2;
        REQUIRE(guest.open(guestDevice, guestContext, wire.guestSession, guestParty));
        REQUIRE(host.open(hostDevice, hostContext, wire.hostSession, hostParty, level, options));
        for (s32 i = 0; i < 3600 && (host.phase() != OnlinePlay::Phase::Playing ||
                                     guest.phase() != OnlinePlay::Phase::Playing);
             ++i) {
            step();
        }
        REQUIRE(host.phase() == OnlinePlay::Phase::Playing);
        REQUIRE(guest.phase() == OnlinePlay::Phase::Playing);
        settle();
        pulses.clear();
    }
    void step(const SessionInputs::Frame& guestInputs = {}) {
        REQUIRE(host.update({}) != OnlinePlay::Phase::Failed);
        REQUIRE(guest.update(guestInputs) != OnlinePlay::Phase::Failed);
        if (host.entryVisible()) {
            hostDevice.draws.clear();
            host.render(hostDevice, Mat4{1}, 1280, 720, 1.0 / 60, 1);
        }
        if (guest.entryVisible()) {
            guestDevice.draws.clear();
            guest.render(guestDevice, Mat4{1}, 1280, 720, 1.0 / 60, 1);
        }
    }
    void pump() {
        for (s32 i = 0; i < 8; ++i) {
            step();
        }
    }
    PlayScene& scene() {
        REQUIRE(host.hostScene());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): native gameplay fixture
        return *const_cast<PlayScene*>(host.hostScene());
    }
    PlayerActor& actor(s32 seat) {
        REQUIRE(scene().actor(seat));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): native gameplay fixture
        return *const_cast<PlayerActor*>(scene().actor(seat));
    }
    void settle() {
        const auto entering = [this] {
            if (scene().spawning()) {
                return true;
            }
            for (s32 seat = 0; seat < 4; ++seat) {
                if (scene().animator(seat)->entering()) {
                    return true;
                }
            }
            return false;
        };
        for (s32 i = 0; i < 420 && entering(); ++i) {
            step();
        }
        REQUIRE_FALSE(entering());
    }
    usize count(ControlFeedback cue) const {
        return static_cast<usize>(std::ranges::count(pulses, cue, &Pulse::cue));
    }
};

TEST_CASE("online native damage and hammer cues reach the guest's assigned physical pad",
          "[netplay][online-feedback][rumble][assets]") {
    FeedbackPair pair;
    pair.open(*pair.catalog.byName("G1"));
    REQUIRE_FALSE(pair.hostContext.vibrate);
    pair.scene().harm(2, 5, HurtKind::Blow);
    pair.scene().harm(3, 5, HurtKind::Blow);
    pair.scene().harm(0, 5, HurtKind::Blow); // the host's cue cannot leak to a guest
    pair.pump();
    REQUIRE(pair.pulses.size() == 2);
    CHECK(pair.pulses[0].device == 0);
    CHECK(pair.pulses[0].motors.pad == 7);
    CHECK(pair.pulses[1].device == 2);
    CHECK(pair.pulses[1].motors.pad == 4);
    for (const auto& pulse : pair.pulses) {
        CHECK(pulse.cue == ControlFeedback::Damage);
        CHECK(pulse.motors.milliseconds == 367);
        CHECK(pulse.motors.low == 0xFFFF);
        CHECK(pulse.motors.high == 0);
    }
    // Let the damage reaction finish before asking the native animator to strike.
    for (s32 i = 0; i < 120; ++i) {
        pair.step();
    }
    pair.pulses.clear();
    auto& inventory = pair.actor(2).save().progress().inventory;
    inventory.addPowerup(powerup::kWeapon, powerup::kThunderHammer, 2, -1);
    SessionInputs::Frame attack;
    attack[0].attack = true;
    attack[0].attackPressed = true;
    pair.step(attack);
    for (s32 i = 0; i < 180 && pair.count(ControlFeedback::ThunderHammer) == 0; ++i) {
        pair.step();
    }
    REQUIRE(pair.count(ControlFeedback::ThunderHammer) == 1);
    const auto hit =
        std::ranges::find(pair.pulses, ControlFeedback::ThunderHammer, &FeedbackPair::Pulse::cue);
    REQUIRE(hit != pair.pulses.end());
    CHECK(hit->device == 0);
    CHECK(hit->motors.pad == 7);
    CHECK(hit->motors.milliseconds == 300);
    CHECK(hit->motors.low == 0xFFFF);
    CHECK(hit->motors.high == 0xA000);
    REQUIRE(inventory.powerup(powerup::kWeapon, powerup::kThunderHammer));
    CHECK(inventory.powerup(powerup::kWeapon, powerup::kThunderHammer)->charge == 1);
    pair.pump();
    CHECK(pair.count(ControlFeedback::ThunderHammer) == 1);
}

TEST_CASE("online feedback preserves independent local pause gates without a resume backlog",
          "[netplay][online-feedback][rumble][assets]") {
    FeedbackPair pair;
    // The tower keeps unrelated native combat damage out of this gate-only test.
    pair.open(LevelRef::tower(), {0, 0, 0});
    auto& hostFeedback = pair.wire.host.feedback();
    REQUIRE(hostFeedback.emit(2, 10, ControlFeedback::Damage));
    REQUIRE_FALSE(hostFeedback.outgoing().empty());
    const auto old = hostFeedback.outgoing().front().bytes;
    const auto stopped = pair.guestStops;
    REQUIRE(pair.guest.pause(0));
    CHECK(pair.guestStops == stopped + 1);
    CHECK_FALSE(pair.guest.pause(1)); // this device belongs to no guest seat
    REQUIRE(hostFeedback.emit(3, 0, ControlFeedback::MeleeHit));
    pair.pump();
    REQUIRE(pair.pulses.size() == 1);
    CHECK(pair.pulses.front().device == 2);
    CHECK(pair.pulses.front().motors.pad == 4);
    REQUIRE(pair.guest.resume(0));
    CHECK(pair.wire.guest.receive(99, old) == MatchSession::Admission::Stale);
    pair.pump();
    CHECK(pair.pulses.size() == 1);
    REQUIRE(hostFeedback.emit(2, 0, ControlFeedback::ThunderHammer));
    pair.pump();
    REQUIRE(pair.pulses.size() == 2);
    CHECK(pair.pulses.back().device == 0);
    REQUIRE(pair.guest.pause(2));
    REQUIRE(hostFeedback.emit(2, 0, ControlFeedback::GeneratorDestroyed));
    REQUIRE(hostFeedback.emit(3, 0, ControlFeedback::GeneratorDestroyed));
    pair.pump();
    REQUIRE(pair.pulses.size() == 3);
    CHECK(pair.pulses.back().device == 0);
    REQUIRE(pair.guest.resume(2));
    pair.pump();
    CHECK(pair.pulses.size() == 3);
}

TEST_CASE("online feedback keeps the receiving machine's rumble setting and capability checks",
          "[netplay][online-feedback][rumble][assets]") {
    FeedbackPair pair;
    pair.open(LevelRef::tower(), {0, 0, 0});
    pair.guestConfig.controls[0].rumble = false;
    REQUIRE(pair.wire.host.feedback().emit(2, 10, ControlFeedback::Damage));
    REQUIRE(pair.wire.host.feedback().emit(3, 10, ControlFeedback::Damage));
    pair.pump();
    REQUIRE(pair.pulses.size() == 1);
    CHECK(pair.pulses.front().device == 2);
    pair.guestConfig.controls[0].rumble = true;
    pair.guestInput.setPad(7, FeedbackPair::pad("guest-first", false));
    REQUIRE(pair.wire.host.feedback().emit(2, 0, ControlFeedback::ThunderHammer));
    pair.pump();
    CHECK(pair.pulses.size() == 1);
    pair.guestInput.setPad(7, {});
    REQUIRE(pair.wire.host.feedback().emit(2, 0, ControlFeedback::ThunderHammer));
    pair.pump();
    CHECK(pair.pulses.size() == 1);
    pair.guestInput.setPad(7, FeedbackPair::pad("guest-first"));
    REQUIRE(pair.wire.host.feedback().emit(2, 0, ControlFeedback::ThunderHammer));
    pair.pump();
    REQUIRE(pair.pulses.size() == 2);
    REQUIRE(pair.count(ControlFeedback::ThunderHammer) == 1);
    const auto hammer =
        std::ranges::find(pair.pulses, ControlFeedback::ThunderHammer, &FeedbackPair::Pulse::cue);
    REQUIRE(hammer != pair.pulses.end());
    CHECK(hammer->device == 0);
    CHECK(hammer->motors.pad == 7);
}

TEST_CASE("online travel and disconnect stop motors and reject prior scene feedback",
          "[netplay][online-feedback][rumble][assets]") {
    FeedbackPair pair;
    pair.open(LevelRef::tower(), {0, 0, 0});
    REQUIRE(pair.wire.host.feedback().emit(2, 10, ControlFeedback::Damage));
    const auto old = pair.wire.host.feedback().outgoing().front().bytes;
    const auto epoch = pair.wire.host.context().epoch;
    const auto hostStops = pair.hostStops;
    const auto guestStops = pair.guestStops;
    SessionInputs::Frame accept;
    for (auto& input : accept) {
        input.menu.select = true;
    }
    for (s32 i = 0; i < 1200 && pair.wire.host.context().epoch == epoch; ++i) {
        for (s32 seat = 0; seat < 4; ++seat) {
            pair.actor(seat).place({45.1f, -6.5f, -112.7f});
        }
        pair.step(accept);
    }
    REQUIRE(pair.wire.host.context().epoch != epoch);
    for (s32 i = 0; i < 3600 && (pair.host.phase() != OnlinePlay::Phase::Playing ||
                                 pair.guest.phase() != OnlinePlay::Phase::Playing);
         ++i) {
        pair.step();
    }
    REQUIRE(pair.scene().world()->ref().name == "G1");
    CHECK(pair.hostStops > hostStops);
    CHECK(pair.guestStops > guestStops);
    CHECK(pair.wire.guest.receive(99, old) == MatchSession::Admission::Stale);
    pair.settle();
    pair.pulses.clear();
    pair.scene().harm(2, 5, HurtKind::Blow);
    pair.pump();
    REQUIRE(pair.pulses.size() == 1);
    CHECK(pair.pulses.front().motors.pad == 7);
    const auto beforeDisconnect = pair.guestStops;
    pair.wire.guest.disconnected(99);
    CHECK(pair.guest.update({}) == OnlinePlay::Phase::Failed);
    CHECK(pair.guestStops > beforeDisconnect);
    CHECK(pair.wire.guest.feedback().outgoing().empty());
    CHECK_FALSE(pair.wire.guest.feedback().take());
    CHECK_FALSE(pair.wire.guest.feedback().emit(2, 0, ControlFeedback::ThunderHammer));
    pair.guest.close();
    const auto afterClose = pair.guestStops;
    pair.guest.close();
    CHECK(pair.guestStops == afterClose);
}
} // namespace
