#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"
#include "game/netplay/MatchSession.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/ReplicaAudio.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Result = ReplicaAudio::Prediction;

struct Fixture {
    std::filesystem::path root = test::scratchDirectory("replica-audio-prediction");
    AudioMixer mixer{48000};
    SoundPlayer output{mixer};
    ReplicaAudio replica;
    AudioSync host;
    AudioSync guest;
    MatchContext context;
    CombatSnapshot authority;

    explicit Fixture(bool loop = false, bool voice = true, bool device = true) {
        const std::array<test::NativeSoundSample, 1> samples{
            {{48000, std::vector<s16>(4800, 16384)}}};
        if (voice) {
            test::writeNativeSoundBank(root / "AUDIO/WAR", R"({"sounds":[
                {"name":"S_WARTHROW","duration":0.1,"volume":127,"sequence":[{"sample":0}]},
                {"name":"S_OTHER","duration":0.1,"volume":127,"sequence":[{"sample":0}]}
            ]})",
                                       samples);
        }
        const auto common = loop ? R"({"sounds":[
            {"name":"S_SUPERSHOT","duration":-1,"volume":127,
             "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]}
        ]})"
                                 : R"({"sounds":[
            {"name":"S_SUPERSHOT","duration":0.1,"volume":127,"sequence":[{"sample":0}]},
            {"name":"S_AMULETFIRE","duration":0.1,"volume":127,"sequence":[{"sample":0}]},
            {"name":"S_AMULETLIGHTNI","duration":0.1,"volume":127,"sequence":[{"sample":0}]},
            {"name":"S_AMULETLIGHT","duration":0.1,"volume":127,"sequence":[{"sample":0}]},
            {"name":"S_AMULETACID","duration":0.1,"volume":127,"sequence":[{"sample":0}]}
        ]})";
        test::writeNativeSoundBank(root / "AUDIO/COMMON", common, samples);
        context.epoch = 3;
        context.scene = 1;
        context.owners = {1, 2, 2, 0};
        context.grants = {1, 4, 2, 0};
        host.begin(context.epoch, 1, context.owners);
        guest.begin(context.epoch, 2, context.owners);
        replica.open(root, device ? &output : nullptr, nullptr, 1);
        REQUIRE(replica.beginPrediction(context, 2));
        if (voice && device) {
            REQUIRE(replica.prepareWeapon(1, 0));
            REQUIRE(replica.prepareWeapon(2, 8)); // secret class shares the native base voice
        }
        authority.motion.epoch = context.epoch;
        authority.motion.tick = 10;
        REQUIRE(replica.reconcile(authority, {}));
    }
    ShotIdentity origin(u8 seat = 1, u64 tick = 11) const {
        return {context.epoch, tick, tick + 10, context.grants[seat], seat, false};
    }
    static SoundPlayer::Event play(std::string bank = "WAR", u32 call = 0,
                                   SoundHandle handle = 100) {
        SoundPlayer::Event event;
        event.bank = std::move(bank);
        event.call = call;
        event.handle = handle;
        event.value = LevelSoundscape::kStepVolume;
        return event;
    }
    bool deliver(const SoundPlayer::Event& event,
                 std::optional<ShotIdentity> identity = std::nullopt) {
        REQUIRE(host.emit(event, identity));
        while (!host.outgoing().empty()) {
            const auto packet = host.outgoing().front();
            REQUIRE(packet.peer == 2);
            REQUIRE(guest.receive(1, packet.bytes) == AudioSync::Admission::Accepted);
            host.sent();
        }
        return replica.consume(guest);
    }
    void drain() {
        std::array<f32, 960> pcm{};
        for (usize frame = 0; frame < 30; ++frame) {
            mixer.mix(pcm);
            output.update();
        }
    }
};

TEST_CASE("local release audio reaches PCM once before delayed authority",
          "[netplay][audio-prediction]") {
    Fixture fixture;
    fixture.output.setCategoryVolume(SoundCategory::Effects, 0.7f);
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture capture(&fixture.output,
                                       [&](const auto& event) { events.push_back(event); });
    const auto origin = fixture.origin();
    REQUIRE(fixture.replica.predict(origin, 1) == Result::Played);
    REQUIRE(events.size() == 1);
    const auto local = events.front().handle;
    CHECK(events.front().bank == "WAR");
    CHECK(events.front().value == LevelSoundscape::kStepVolume);
    std::array<f32, 960> pcm{};
    fixture.mixer.mix(pcm);
    CHECK(std::ranges::any_of(pcm, [](f32 sample) { return sample != 0; }));
    CHECK(fixture.replica.predict(origin, 1) == Result::Duplicate);
    REQUIRE(fixture.deliver(Fixture::play(), origin));
    CHECK(events.size() == 1); // host confirmation adopts the same active voice
    CHECK(fixture.replica.voices() == 1);
    auto command = Fixture::play();
    command.bank.clear();
    command.kind = SoundPlayer::Event::Kind::Pan;
    command.value = -0.75f;
    REQUIRE(fixture.deliver(command));
    CHECK(events.back().kind == SoundPlayer::Event::Kind::Pan);
    CHECK(events.back().handle == local);
    command.kind = SoundPlayer::Event::Kind::Volume;
    command.value = 0.1f;
    REQUIRE(fixture.deliver(command));
    CHECK(events.back().kind == SoundPlayer::Event::Kind::Volume);
    CHECK(events.back().handle == local);
    command.kind = SoundPlayer::Event::Kind::Stop;
    REQUIRE(fixture.deliver(command));
    CHECK_FALSE(fixture.output.isPlaying(local));
    CHECK(fixture.replica.voices() == 0);
    CHECK(fixture.output.categoryVolume(SoundCategory::Effects) == 0.7f);
}

TEST_CASE("finished local release audio is not replayed when host sound arrives late",
          "[netplay][audio-prediction]") {
    Fixture fixture;
    usize plays = 0;
    const SoundPlayer::Capture capture(&fixture.output, [&](const auto& event) {
        plays += event.kind == SoundPlayer::Event::Kind::Play ? 1 : 0;
    });
    const auto origin = fixture.origin();
    REQUIRE(fixture.replica.predict(origin, 1) == Result::Played);
    fixture.drain();
    CHECK(fixture.output.voiceCount() == 0);
    fixture.authority.motion.tick = origin.tick + ReplicaAudio::kPredictionTicks + 10;
    REQUIRE(fixture.replica.reconcile(fixture.authority, {}));
    REQUIRE(fixture.deliver(Fixture::play(), origin));
    CHECK(plays == 1);
    CHECK(fixture.output.voiceCount() == 0);
    CHECK(fixture.replica.predict(origin, 1) == Result::Duplicate);
    REQUIRE(fixture.deliver(Fixture::play("WAR", 0, 101), origin));
    CHECK(plays == 1);
}

TEST_CASE("host first release audio suppresses later replay without suppressing other seats",
          "[netplay][audio-prediction]") {
    Fixture fixture;
    REQUIRE(fixture.deliver(Fixture::play(), fixture.origin()));
    CHECK(fixture.replica.predict(fixture.origin(), 1) == Result::Duplicate);
    CHECK(fixture.replica.predict(fixture.origin(2), 1) == Result::Played);
    CHECK(fixture.output.voiceCount() == 2);
    REQUIRE(fixture.deliver(Fixture::play("WAR", 0, 101), fixture.origin(0)));
    CHECK(fixture.output.voiceCount() == 3); // remote player audio remains authoritative
    CHECK(fixture.replica.predict(fixture.origin(0), 1) == Result::Invalid);
}

TEST_CASE("a three or five way release makes one native fan cue", "[netplay][audio-prediction]") {
    for (const u8 shots : {u8{3}, u8{5}}) {
        Fixture fixture;
        std::vector<SoundPlayer::Event> events;
        const SoundPlayer::Capture capture(&fixture.output,
                                           [&](const auto& event) { events.push_back(event); });
        auto origin = fixture.origin();
        REQUIRE(fixture.replica.predict(origin, shots) == Result::Played);
        REQUIRE(events.size() == 1);
        CHECK(events.front().bank == "COMMON");
        for (u8 ordinal = 1; ordinal < shots; ++ordinal) {
            origin.ordinal = ordinal;
            CHECK(fixture.replica.predict(origin, shots) == Result::Invalid);
        }
        origin.ordinal = 0;
        CHECK(fixture.replica.predict(origin, shots) == Result::Duplicate);
        REQUIRE(fixture.deliver(Fixture::play("COMMON"), origin));
        CHECK(events.size() == 1);
    }
}

TEST_CASE("only vanished future releases cancel and resolved sounds wait for reliable audio",
          "[netplay][audio-prediction]") {
    Fixture fixture;
    const auto future = fixture.origin();
    REQUIRE(fixture.replica.predict(future, 1) == Result::Played);
    SECTION("absent still-future release was corrected out") {
        REQUIRE(fixture.replica.reconcile(fixture.authority, {}));
        CHECK_FALSE(fixture.output.isPlaying(1));
        CHECK(fixture.replica.predict(future, 1) == Result::Duplicate);
        REQUIRE(fixture.deliver(Fixture::play(), future));
        CHECK_FALSE(fixture.output.isPlaying(1));
        CHECK_FALSE(fixture.output.isPlaying(2));
    }
    SECTION("resolved release may be accepted despite no live projectile snapshot") {
        fixture.authority.motion.tick = future.tick;
        REQUIRE(fixture.replica.reconcile(fixture.authority, {}));
        CHECK(fixture.output.isPlaying(1));
        REQUIRE(fixture.deliver(Fixture::play(), future));
        CHECK(fixture.output.isPlaying(1));
        CHECK(fixture.output.voiceCount() == 1);
    }
    SECTION("complete retained replay leaves its cue alive") {
        const std::array pending{future};
        REQUIRE(fixture.replica.reconcile(fixture.authority, pending));
        CHECK(fixture.output.isPlaying(1));
    }
}

TEST_CASE("audio prediction isolates epochs grants and invalid release metadata",
          "[netplay][audio-prediction]") {
    Fixture fixture;
    const auto original = fixture.origin();
    REQUIRE(fixture.replica.predict(original, 1) == Result::Played);
    auto invalid = original;
    invalid.grant += 1;
    CHECK(fixture.replica.predict(invalid, 1) == Result::Invalid);
    REQUIRE(fixture.deliver(Fixture::play(), invalid));
    CHECK(fixture.replica.voices() == 0); // invalid attribution cannot adopt another voice
    invalid = original;
    ++invalid.epoch;
    CHECK(fixture.replica.predict(invalid, 1) == Result::Invalid);
    CHECK(fixture.replica.predict(original, 2) == Result::Invalid);
    invalid = original;
    invalid.tick = fixture.authority.motion.tick + ReplicaAudio::kPredictionTicks + 1;
    CHECK(fixture.replica.predict(invalid, 1) == Result::Invalid);
    ++fixture.context.grants[1];
    REQUIRE(fixture.replica.beginPrediction(fixture.context, 2));
    CHECK_FALSE(fixture.output.isPlaying(1));
    REQUIRE(fixture.replica.prepareWeapon(1, 16)); // Sumner follows the native base voice
    CHECK(fixture.replica.predict(original, 1) == Result::Invalid);
    CHECK(fixture.replica.predict(fixture.origin(), 1) == Result::Played);
    ++fixture.context.epoch;
    REQUIRE(fixture.replica.beginPrediction(fixture.context, 2));
    CHECK_FALSE(fixture.output.isPlaying(2));
    CHECK(fixture.replica.predict(original, 1) == Result::Invalid);
    REQUIRE(fixture.replica.prepareWeapon(1, 0));
    CHECK(fixture.replica.predict(fixture.origin(), 1) == Result::Played);
    fixture.replica.clear();
    fixture.drain(); // No sequence may still borrow released bank clips.
    CHECK(fixture.output.voiceCount() == 0);
}

TEST_CASE("looping or missing optional throw preparation falls back to host playback",
          "[netplay][audio-prediction]") {
    SECTION("native fan loop never predicts but still follows the host") {
        Fixture fixture(true);
        CHECK(fixture.replica.predict(fixture.origin(), 5) == Result::Unavailable);
        fixture.authority.motion.tick = 200; // visual snapshots overtake reliable audio
        REQUIRE(fixture.replica.reconcile(fixture.authority, {}));
        REQUIRE(fixture.deliver(Fixture::play("COMMON"), fixture.origin()));
        CHECK(fixture.replica.voices() == 1);
        fixture.drain();
        CHECK(fixture.output.isPlaying(1));
        auto stop = Fixture::play();
        stop.bank.clear();
        stop.kind = SoundPlayer::Event::Kind::Stop;
        REQUIRE(fixture.deliver(stop));
        CHECK_FALSE(fixture.output.isPlaying(1));
    }
    SECTION("missing voice does not remove fan audio") {
        Fixture fixture(false, false);
        CHECK_FALSE(fixture.replica.prepareWeapon(1, 0));
        CHECK(fixture.replica.predict(fixture.origin(), 1) == Result::Unavailable);
        CHECK(fixture.replica.predict(fixture.origin(), 5) == Result::Played);
    }
    SECTION("disabled output consumes the host stream without preparation") {
        Fixture fixture(false, true, false);
        CHECK_FALSE(fixture.replica.prepareWeapon(1, 0));
        CHECK(fixture.replica.predict(fixture.origin(), 1) == Result::Unavailable);
        REQUIRE(fixture.deliver(Fixture::play(), fixture.origin()));
        CHECK(fixture.replica.voices() == 0);
        CHECK_FALSE(fixture.guest.take());
    }
    SECTION("unprepared valid native call remains authoritative") {
        Fixture fixture;
        fixture.replica.open(fixture.root, &fixture.output, nullptr, 1);
        REQUIRE(fixture.replica.beginPrediction(fixture.context, 2));
        REQUIRE(fixture.deliver(Fixture::play(), fixture.origin()));
        CHECK(fixture.replica.voices() == 1);
    }
}

TEST_CASE("never-predicted authority audio survives arbitrarily newer visual checkpoints",
          "[netplay][audio-prediction]") {
    Fixture fixture;
    fixture.authority.motion.tick = 500;
    REQUIRE(fixture.replica.reconcile(fixture.authority, {}));
    REQUIRE(fixture.deliver(Fixture::play(), fixture.origin()));
    CHECK(fixture.output.isPlaying(1));
    CHECK(fixture.replica.voices() == 1);
    std::array<f32, 960> pcm{};
    fixture.mixer.mix(pcm);
    CHECK(std::ranges::any_of(pcm, [](f32 sample) { return sample != 0; }));
}

TEST_CASE("another bank call cannot falsely confirm a predicted throw",
          "[netplay][audio-prediction]") {
    Fixture fixture;
    REQUIRE(fixture.replica.predict(fixture.origin(), 1) == Result::Played);
    CHECK_FALSE(fixture.deliver(Fixture::play("WAR", 1), fixture.origin()));
    CHECK(fixture.replica.voices() == 0);
    CHECK(fixture.output.voiceCount() == 1);
    REQUIRE(fixture.deliver(Fixture::play("WAR", 1, 101)));
    CHECK(fixture.output.voiceCount() == 2); // untagged native effects stay independent
}

TEST_CASE("authoritative modifier correction replaces a conflicting speculative release cue",
          "[netplay][audio-prediction]") {
    for (const u8 predicted : {u8{1}, u8{5}}) {
        Fixture fixture;
        const auto origin = fixture.origin();
        REQUIRE(fixture.replica.predict(origin, predicted) == Result::Played);
        REQUIRE(fixture.output.isPlaying(1));
        REQUIRE(fixture.deliver(Fixture::play(predicted == 1 ? "COMMON" : "WAR"), origin));
        CHECK_FALSE(fixture.output.isPlaying(1));
        CHECK(fixture.output.isPlaying(2));
        CHECK(fixture.replica.voices() == 1);
        CHECK(fixture.replica.predict(origin, predicted) == Result::Duplicate);
        CHECK(fixture.replica.predict(origin, predicted == 1 ? 5 : 1) == Result::Duplicate);
    }
}

TEST_CASE("audio preparation honors the first native level bank match",
          "[netplay][audio-prediction]") {
    Fixture fixture;
    const std::array<test::NativeSoundSample, 1> samples{{{48000, std::vector<s16>(4800, 16384)}}};
    test::writeNativeSoundBank(fixture.root / "AUDIO/LEVEL", R"({"sounds":[
        {"name":"S_OTHER","duration":0.1,"volume":127,"sequence":[{"sample":0}]},
        {"name":"S_SUPERSHOT","duration":0.1,"volume":127,"sequence":[{"sample":0}]}
    ]})",
                               samples);
    const LevelAudioInfo info{.bank = "LEVEL"};
    fixture.replica.open(fixture.root, &fixture.output, &info, 1);
    REQUIRE(fixture.replica.beginPrediction(fixture.context, 2));
    REQUIRE(fixture.replica.prepareWeapon(1, 0));
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture capture(&fixture.output,
                                       [&](const auto& event) { events.push_back(event); });
    REQUIRE(fixture.replica.predict(fixture.origin(), 5) == Result::Played);
    REQUIRE(events.size() == 1);
    CHECK(events.front().bank == "LEVEL");
    CHECK(events.front().call == 1);
}

TEST_CASE("audio deduplication is bounded and retired origins cannot replay",
          "[netplay][audio-prediction]") {
    Fixture fixture;
    auto identity = fixture.origin();
    for (usize index = 0; index < ReplicaAudio::kPredictionCapacity; ++index) {
        identity.generation = index + 1;
        REQUIRE(fixture.replica.predict(identity, 1) == Result::Played);
        fixture.drain(); // bounded ledger must retain even finished cues
    }
    ++identity.generation;
    CHECK(fixture.replica.predict(identity, 1) == Result::Capacity);
    fixture.authority.motion.tick = identity.tick + ReplicaAudio::kPredictionTicks;
    REQUIRE(fixture.replica.reconcile(fixture.authority, {}));
    CHECK(fixture.replica.predict(identity, 1) == Result::Duplicate);
    REQUIRE(fixture.deliver(Fixture::play(), identity));
    CHECK(fixture.output.voiceCount() == 1); // never-heard authority is not evicted by pressure
    fixture.drain();
    // A real confirmation frees only its finished key, not unconfirmed heard ones.
    auto confirmed = identity;
    confirmed.generation = 1;
    SECTION("matching authority confirms its key") {
        REQUIRE(fixture.deliver(Fixture::play("WAR", 0, 101), confirmed));
    }
    SECTION("corrected fan authority resolves the already-finished wrong cue") {
        REQUIRE(fixture.deliver(Fixture::play("COMMON", 0, 101), confirmed));
    }
    fixture.drain();
    REQUIRE(fixture.replica.reconcile(fixture.authority, {}));
    CHECK(fixture.output.voiceCount() == 0);
    identity.tick = fixture.authority.motion.tick + 1;
    SoundHandle fresh = kNoSound;
    const SoundPlayer::Capture capture(&fixture.output, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            fresh = event.handle;
        }
    });
    CHECK(fixture.replica.predict(identity, 1) == Result::Played);
    auto stale = fixture.authority;
    --stale.motion.tick;
    CHECK_FALSE(fixture.replica.reconcile(stale, {}));
    CHECK(fresh != kNoSound);
    CHECK(fixture.output.isPlaying(fresh));
}

TEST_CASE("elemental release audio uses native cue priority once per complete fan",
          "[netplay][audio-prediction][elemental-rollback]") {
    const u32 element = GENERATE(1U, 2U, 3U, 4U);
    const u32 spread = GENERATE(0U, powerup::kThreeWayShot, powerup::kFiveWayShot);
    const bool strong = GENERATE(false, true);
    Fixture fixture;
    auto origin = fixture.origin();
    origin.strong = strong;
    const u32 flags = element | spread | powerup::kRapidFire;
    PowerupEffects effects;
    effects.weapon = flags;
    const auto shots = static_cast<u8>(effects.shots());
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture capture(&fixture.output,
                                       [&](const auto& event) { events.push_back(event); });
    REQUIRE(fixture.replica.predict(origin, shots, flags) == Result::Played);
    REQUIRE(events.size() == 1);
    CHECK(events.front().bank == "COMMON");
    CHECK(events.front().call == (spread == 0 ? element : 0));
    std::array<f32, 960> pcm{};
    fixture.mixer.mix(pcm);
    CHECK(std::ranges::any_of(pcm, [](f32 sample) { return sample != 0; }));
    REQUIRE(fixture.deliver(Fixture::play("COMMON", spread == 0 ? element : 0), origin));
    CHECK(events.size() == 1);
    CHECK(fixture.replica.predict(origin, shots, flags) == Result::Duplicate);
    for (u8 ordinal = 1; ordinal < shots; ++ordinal) {
        auto side = origin;
        side.ordinal = ordinal;
        CHECK(fixture.replica.predict(side, shots, flags) == Result::Invalid);
    }
}

TEST_CASE("elemental audio corrections withdraw the wrong voice without restarting heard cues",
          "[netplay][audio-prediction][elemental-rollback]") {
    Fixture fixture;
    const auto origin = fixture.origin();
    REQUIRE(fixture.replica.predict(origin, 1, 1) == Result::Played);
    REQUIRE(fixture.output.isPlaying(1));
    REQUIRE(fixture.replica.predict(origin, 1, 4) == Result::Played);
    CHECK_FALSE(fixture.output.isPlaying(1));
    CHECK(fixture.output.isPlaying(2));
    CHECK(fixture.replica.predict(origin, 1, 1) == Result::Duplicate);
    REQUIRE(fixture.deliver(Fixture::play("COMMON", 2), origin));
    CHECK_FALSE(fixture.output.isPlaying(2));
    CHECK(fixture.output.isPlaying(3));
    // Once authority has picked this release's cue, another replay cannot invent
    // a different elemental recording even when that key was never heard.
    CHECK(fixture.replica.predict(origin, 1, 3) == Result::Duplicate);
    fixture.drain();
    fixture.authority.motion.tick += ReplicaAudio::kPredictionTicks + 10;
    REQUIRE(fixture.replica.reconcile(fixture.authority, {}));
    CHECK(fixture.replica.predict(origin, 1, 4) == Result::Duplicate);
}

TEST_CASE("elemental preparation follows native level common ambient order and rejects loops",
          "[netplay][audio-prediction][elemental-rollback]") {
    const bool loop = GENERATE(false, true);
    Fixture fixture;
    const std::array<test::NativeSoundSample, 1> samples{{{48000, std::vector<s16>(4800, 16384)}}};
    test::writeNativeSoundBank(fixture.root / "AUDIO/TOWAMB", R"({"sounds":[
        {"name":"S_AMULETACID","duration":0.1,"volume":127,"sequence":[{"sample":0}]}
    ]})",
                               samples);
    const auto level = loop ? R"({"sounds":[
        {"name":"S_AMULETACID","duration":-1,"volume":127,
         "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]}
    ]})"
                            : R"({"sounds":[
        {"name":"S_AMULETACID","duration":0.1,"volume":127,"sequence":[{"sample":0}]}
    ]})";
    test::writeNativeSoundBank(fixture.root / "AUDIO/LEVEL", level, samples);
    const LevelAudioInfo info{.bank = "LEVEL"};
    fixture.replica.open(fixture.root, &fixture.output, &info, 1);
    REQUIRE(fixture.replica.beginPrediction(fixture.context, 2));
    REQUIRE(fixture.replica.prepareWeapon(1, 0));
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture capture(&fixture.output,
                                       [&](const auto& event) { events.push_back(event); });
    CHECK(fixture.replica.predict(fixture.origin(), 1, 4) ==
          (loop ? Result::Unavailable : Result::Played));
    CHECK(events.size() == (loop ? 0 : 1));
    REQUIRE(fixture.deliver(Fixture::play("LEVEL"), fixture.origin()));
    REQUIRE(events.size() == 1);
    CHECK(events.front().bank == "LEVEL");
    CHECK(events.front().call == 0);
}

TEST_CASE("unsupported elemental masks cannot select a cue or stop another seat",
          "[netplay][audio-prediction][elemental-rollback]") {
    Fixture fixture;
    REQUIRE(fixture.replica.predict(fixture.origin(2), 1, 4) == Result::Played);
    for (const u32 flags : {5U, 15U, powerup::kSuperShot | 1U, 0x80000001U}) {
        CHECK(fixture.replica.predict(fixture.origin(), 1, flags) == Result::Invalid);
    }
    CHECK(fixture.output.voiceCount() == 1);
    CHECK(fixture.replica.predict(fixture.origin(0), 1, 1) == Result::Invalid);
    CHECK(fixture.output.isPlaying(1));
}

TEST_CASE("turbo volleys keep native fan cue priority without multiplying shots or sounds",
          "[netplay][audio-prediction][turbo-volley-rollback]") {
    const u32 spread = GENERATE(powerup::kThreeWayShot, powerup::kFiveWayShot);
    Fixture fixture;
    auto first = fixture.origin();
    first.emission = 0x10000;
    auto second = first;
    second.emission = 0x10001;
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture capture(&fixture.output,
                                       [&](const auto& event) { events.push_back(event); });
    REQUIRE(fixture.replica.predict(first, 1, spread) == Result::Played);
    REQUIRE(fixture.replica.predict(second, 1, spread) == Result::Played);
    REQUIRE(events.size() == 2);
    for (const auto& event : events) {
        CHECK(event.bank == "COMMON");
        CHECK(event.call == 0);
    }
    REQUIRE(fixture.deliver(Fixture::play("COMMON", 0, 101), first));
    REQUIRE(fixture.deliver(Fixture::play("COMMON", 0, 102), second));
    CHECK(events.size() == 2);
    CHECK(fixture.replica.predict(first, 1, spread) == Result::Duplicate);
    CHECK(fixture.replica.predict(second, 1, spread) == Result::Duplicate);
    auto otherRow = first;
    otherRow.emission = 0x20000;
    CHECK(fixture.replica.predict(otherRow, 1, spread) == Result::Played);
    CHECK(events.size() == 3);
}
} // namespace
