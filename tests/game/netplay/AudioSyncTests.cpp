#include <algorithm>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "game/netplay/AudioSync.h"
#include "game/screens/ReplicaAudio.h"

namespace {
using namespace gdl;
using namespace gdl::game;
SoundPlayer::Event effect() {
    return {SoundPlayer::Event::Kind::Play, 9, kNoSound, "COMMON", 3, 0.625f,
            SoundCategory::Effects};
}
void put32(std::vector<u8>& bytes, usize offset, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes[offset + shift / 8] = static_cast<u8>(value >> shift);
    }
}
void transfer(AudioSync& host, AudioSync& guest) {
    while (!host.outgoing().empty()) {
        const auto delivery = host.outgoing().front();
        REQUIRE(delivery.bytes.size() == AudioSync::kPacketBytes);
        REQUIRE(guest.receive(1, delivery.bytes) == AudioSync::Admission::Accepted);
        CHECK(guest.receive(1, delivery.bytes) == AudioSync::Admission::Stale);
        host.sent();
    }
}
TEST_CASE("sound commands are ordered host-only and scoped to a match epoch",
          "[netplay][audio-sync]") {
    AudioSync host;
    AudioSync guest;
    host.begin(1, 1, {1, 2, 0, 0});
    guest.begin(1, 2, {1, 2, 0, 0});
    SoundPlayer::Event event;
    event.bank = "COMMON";
    event.handle = 9;
    event.call = 3;
    REQUIRE(host.emit(event));
    CHECK_FALSE(guest.emit(event));
    const auto bytes = host.outgoing().front().bytes;
    CHECK(guest.receive(3, bytes) == AudioSync::Admission::WrongPeer);
    CHECK(host.receive(2, bytes) == AudioSync::Admission::WrongPeer);
    for (usize size = 0; size < bytes.size(); ++size) {
        CHECK(guest.receive(1, std::span{bytes}.first(size)) == AudioSync::Admission::Invalid);
    }
    event.bank = "../COMMON";
    CHECK_FALSE(host.emit(event));
    event.bank = "COMMON";
    transfer(host, guest);
    const auto received = guest.take();
    REQUIRE(received);
    CHECK(received->event.bank == "COMMON");
    CHECK(received->event.handle == 9);
    CHECK(received->event.call == 3);
    CHECK_FALSE(received->origin);
    CHECK_FALSE(guest.take());
    guest.begin(2, 2, {1, 2, 0, 0});
    CHECK(guest.receive(1, bytes) == AudioSync::Admission::Stale);
}
TEST_CASE("sound attribution roundtrips the complete ordinary release without changing its cue",
          "[netplay][audio-sync]") {
    constexpr u64 kEpoch = 0x0123456789abcdefULL;
    const ShotIdentity origin{kEpoch,
                              std::numeric_limits<u64>::max(),
                              0xfedcba9876543210ULL,
                              std::numeric_limits<u32>::max(),
                              3,
                              true,
                              0};
    AudioSync host;
    AudioSync guest;
    host.begin(kEpoch, 1, {1, 2, 0, 2});
    guest.begin(kEpoch, 2, {1, 2, 0, 2});
    const auto event = effect();
    REQUIRE(host.emit(event, origin));
    REQUIRE(host.outgoing().size() == 1); // One stream per peer, not per local seat.
    const auto bytes = host.outgoing().front().bytes;
    CHECK(bytes[4] == 3);
    CHECK(bytes[27] == 1);
    CHECK(bytes[104] == 3);
    CHECK(bytes[105] == 1);
    CHECK(bytes[106] == 0);
    CHECK(bytes[107] == 0);
    transfer(host, guest);
    const auto cue = guest.take();
    REQUIRE(cue);
    CHECK(cue->origin == origin);
    CHECK(cue->event.kind == event.kind);
    CHECK(cue->event.handle == event.handle);
    CHECK(cue->event.after == event.after);
    CHECK(cue->event.bank == event.bank);
    CHECK(cue->event.call == event.call);
    CHECK(cue->event.value == event.value);
    CHECK(cue->event.category == event.category);
    CHECK_FALSE(guest.take());
    // A new epoch discards pending cues, even when their shot identity was valid.
    REQUIRE(host.emit(event, origin));
    transfer(host, guest);
    guest.begin(kEpoch + 1, 2, {1, 2, 0, 2});
    CHECK_FALSE(guest.take());
    CHECK(guest.receive(1, bytes) == AudioSync::Admission::Stale);
}

TEST_CASE("only unchained effects can carry the ordinal-zero fan release identity",
          "[netplay][audio-sync]") {
    AudioSync host;
    AudioSync guest;
    host.begin(17, 1, {1, 2, 0, 0});
    guest.begin(17, 2, {1, 2, 0, 0});
    auto event = effect();
    const ShotIdentity origin{17, 41, 99, 7, 1, false, 0};
    for (u8 ordinal = 1; ordinal < ShotIdentity::kMaxShots; ++ordinal) {
        auto fan = origin;
        fan.ordinal = ordinal;
        REQUIRE(fan.valid());
        CHECK_FALSE(host.emit(event, fan));
    }
    for (const auto category : {SoundCategory::Music, SoundCategory::Movie}) {
        event.category = category;
        CHECK_FALSE(host.emit(event, origin));
        REQUIRE(host.emit(event)); // Existing untagged categories remain valid.
    }
    event = effect();
    event.after = 8;
    CHECK_FALSE(host.emit(event, origin));
    REQUIRE(host.emit(event));
    for (const auto kind : {SoundPlayer::Event::Kind::Stop, SoundPlayer::Event::Kind::Volume,
                            SoundPlayer::Event::Kind::Pan}) {
        event = effect();
        event.kind = kind;
        event.bank.clear();
        event.call = 0;
        CHECK_FALSE(host.emit(event, origin));
        REQUIRE(host.emit(event));
    }
    event = effect();
    for (const bool strong : {false, true}) {
        auto valid = origin;
        valid.strong = strong;
        REQUIRE(host.emit(event, valid));
    }
    transfer(host, guest); // Rejected emissions did not consume stream sequence numbers.
    for (usize index = 0; index < 6; ++index) {
        const auto cue = guest.take();
        REQUIRE(cue);
        CHECK_FALSE(cue->origin);
    }
    for (const bool strong : {false, true}) {
        const auto cue = guest.take();
        REQUIRE(cue);
        REQUIRE(cue->origin);
        CHECK(cue->origin->ordinal == 0);
        CHECK(cue->origin->strong == strong);
    }
    CHECK_FALSE(guest.take());
    CHECK_FALSE(host.failed());
}

TEST_CASE("turbo sound origins preserve distinct same-tick volley emissions",
          "[netplay][audio-sync][turbo-volley-identity]") {
    AudioSync host;
    AudioSync guest;
    host.begin(17, 1, {1, 2, 0, 0});
    guest.begin(17, 2, {1, 2, 0, 0});
    const ShotIdentity base{17, 41, 99, 7, 1, false};
    for (const u32 emission : {0x00010000U, 0x00010001U, 0x0080FFFFU}) {
        auto origin = base;
        origin.emission = emission;
        REQUIRE(host.emit(effect(), origin));
        const auto bytes = host.outgoing().front().bytes;
        REQUIRE(bytes.size() == AudioSync::kPacketBytes);
        CHECK(bytes.size() == 112);
        transfer(host, guest);
        const auto cue = guest.take();
        REQUIRE(cue);
        CHECK(cue->origin == origin);
    }
    auto origin = base;
    origin.emission = 0x00010000;
    origin.strong = true;
    CHECK_FALSE(host.emit(effect(), origin));
    origin.strong = false;
    origin.ordinal = 1;
    CHECK_FALSE(host.emit(effect(), origin));

    AudioSync first;
    first.begin(17, 1, {1, 2, 0, 0});
    REQUIRE(first.emit(effect(), base));
    const auto valid = first.outgoing().front().bytes;
    for (const u32 emission : {1U, 65535U, 0x00810000U, 0xFFFFFFFFU}) {
        auto bytes = valid;
        put32(bytes, 108, emission);
        AudioSync receiver;
        receiver.begin(17, 2, {1, 2, 0, 0});
        CHECK(receiver.receive(1, bytes) == AudioSync::Admission::Invalid);
        CHECK_FALSE(receiver.take());
        CHECK(receiver.receive(1, valid) == AudioSync::Admission::Accepted);
    }
}

TEST_CASE("sound attribution emission rejects invalid epochs seats grants and identities",
          "[netplay][audio-sync]") {
    AudioSync host;
    host.begin(17, 1, {1, 2, 0, 0});
    const ShotIdentity valid{17, 41, 99, 7, 1, false, 0};
    auto reject = [&](const ShotIdentity& origin) {
        CHECK_FALSE(host.emit(effect(), origin));
        CHECK(host.outgoing().empty());
        CHECK_FALSE(host.failed());
    };
    for (const u64 epoch : {u64{0}, u64{16}, u64{18}}) {
        auto origin = valid;
        origin.epoch = epoch;
        reject(origin);
    }
    auto origin = valid;
    origin.generation = 0;
    reject(origin);
    origin = valid;
    origin.grant = 0;
    reject(origin);
    for (const u8 seat : {u8{2}, u8{3}, u8{4}, u8{255}}) {
        origin = valid;
        origin.seat = seat;
        reject(origin);
    }
    origin = valid;
    origin.ordinal = 255;
    reject(origin);
    REQUIRE(host.emit(effect(), valid));
    AudioSync guest;
    guest.begin(17, 2, {1, 2, 0, 0});
    transfer(host, guest);
    const auto cue = guest.take();
    REQUIRE(cue);
    CHECK(cue->origin == valid);
}

TEST_CASE("attributed sound packets reject malformed version lengths flags and identity fields "
          "atomically",
          "[netplay][audio-sync]") {
    AudioSync host;
    host.begin(17, 1, {1, 2, 0, 0});
    const ShotIdentity origin{17, 41, 99, 7, 1, false, 0};
    REQUIRE(host.emit(effect(), origin));
    const auto valid = host.outgoing().front().bytes;
    auto reject = [&](std::span<const u8> bytes) {
        AudioSync guest;
        guest.begin(17, 2, {1, 2, 0, 0});
        CHECK(guest.receive(1, bytes) == AudioSync::Admission::Invalid);
        CHECK_FALSE(guest.take());
        REQUIRE(guest.receive(1, valid) == AudioSync::Admission::Accepted);
        const auto cue = guest.take();
        REQUIRE(cue);
        CHECK(cue->origin == origin);
    };
    for (usize size = 0; size < valid.size(); ++size) {
        reject(std::span{valid}.first(size));
    }
    auto bytes = valid;
    bytes.push_back(0);
    reject(bytes);
    for (const u32 version : {0U, 1U, 2U, 4U, std::numeric_limits<u32>::max()}) {
        bytes = valid;
        put32(bytes, 4, version);
        reject(bytes);
    }
    bytes = valid;
    put32(bytes, 4, 1);
    bytes[27] = 0;
    bytes.resize(76); // Former schemas cannot be mistaken for the current identity shape.
    reject(bytes);
    for (const u8 flags : {u8{0}, u8{2}, u8{3}, u8{128}, u8{255}}) {
        bytes = valid;
        bytes[27] = flags;
        reject(bytes);
    }
    for (usize offset = 50; offset < 76; ++offset) {
        bytes = valid;
        bytes[offset] = 1; // Unused bank-name storage must remain zero.
        reject(bytes);
    }
    for (const u32 epoch : {0U, 16U, 18U}) {
        bytes = valid;
        put32(bytes, 76, epoch);
        reject(bytes);
    }
    bytes = valid;
    put32(bytes, 92, 0); // Zero generation, including its high word.
    put32(bytes, 96, 0);
    reject(bytes);
    bytes = valid;
    put32(bytes, 100, 0);
    reject(bytes);
    for (const u8 seat : {u8{2}, u8{3}, u8{4}, u8{255}}) {
        bytes = valid;
        bytes[104] = seat;
        reject(bytes);
    }
    bytes = valid;
    bytes[105] = 2;
    reject(bytes);
    for (const u8 ordinal : {u8{1}, u8{4}, u8{5}, u8{255}}) {
        bytes = valid;
        bytes[106] = ordinal;
        reject(bytes);
    }
    bytes = valid;
    bytes[107] = 1;
    reject(bytes);
    bytes = valid;
    bytes[25] = static_cast<u8>(SoundCategory::Music);
    reject(bytes);
    bytes = valid;
    put32(bytes, 32, 8); // Chained narration cannot claim a local weapon origin.
    reject(bytes);
    bytes = valid;
    bytes[24] = static_cast<u8>(SoundPlayer::Event::Kind::Stop);
    bytes[26] = 0;
    put32(bytes, 36, 0);
    std::fill(bytes.begin() + 44, bytes.begin() + 76, u8{0});
    reject(bytes);
}

TEST_CASE("unattributed sound packets require an entirely zero optional identity",
          "[netplay][audio-sync]") {
    AudioSync host;
    host.begin(17, 1, {1, 2, 0, 0});
    REQUIRE(host.emit(effect()));
    const auto valid = host.outgoing().front().bytes;
    REQUIRE(valid[27] == 0);
    CHECK(std::ranges::all_of(std::span{valid}.subspan(76), [](u8 value) { return value == 0; }));
    for (usize offset = 76; offset < valid.size(); ++offset) {
        CAPTURE(offset);
        auto bytes = valid;
        bytes[offset] = 1;
        AudioSync guest;
        guest.begin(17, 2, {1, 2, 0, 0});
        CHECK(guest.receive(1, bytes) == AudioSync::Admission::Invalid);
        CHECK_FALSE(guest.take());
        REQUIRE(guest.receive(1, valid) == AudioSync::Admission::Accepted);
        const auto cue = guest.take();
        REQUIRE(cue);
        CHECK_FALSE(cue->origin);
    }
}

TEST_CASE("attributed cues cannot bypass stream ordering or the bounded guest queue",
          "[netplay][audio-sync]") {
    AudioSync host;
    AudioSync guest;
    host.begin(17, 1, {1, 2, 0, 0});
    guest.begin(17, 2, {1, 2, 0, 0});
    ShotIdentity origin{17, 41, 99, 7, 1, false, 0};
    REQUIRE(host.emit(effect(), origin));
    const auto first = host.outgoing().front().bytes;
    host.sent();
    ++origin.tick;
    REQUIRE(host.emit(effect(), origin));
    const auto second = host.outgoing().front().bytes;
    host.sent();
    CHECK(guest.receive(1, second) == AudioSync::Admission::Invalid);
    CHECK_FALSE(guest.take());
    REQUIRE(guest.receive(1, first) == AudioSync::Admission::Accepted);
    REQUIRE(guest.receive(1, second) == AudioSync::Admission::Accepted);
    CHECK(guest.receive(1, first) == AudioSync::Admission::Stale);
    CHECK(guest.receive(1, second) == AudioSync::Admission::Stale);
    for (usize count = 2; count < AudioSync::kBudget; ++count) {
        ++origin.tick;
        REQUIRE(host.emit(effect(), origin));
        REQUIRE(guest.receive(1, host.outgoing().front().bytes) == AudioSync::Admission::Accepted);
        host.sent();
    }
    ++origin.tick;
    REQUIRE(host.emit(effect(), origin));
    const auto overflow = host.outgoing().front().bytes;
    CHECK(guest.receive(1, overflow) == AudioSync::Admission::Invalid);
    const auto earliest = guest.take();
    REQUIRE(earliest);
    REQUIRE(earliest->origin);
    CHECK(earliest->origin->tick == 41);
    REQUIRE(guest.receive(1, overflow) == AudioSync::Admission::Accepted);
    guest.clear();
    CHECK_FALSE(guest.take());
    CHECK(guest.receive(1, overflow) == AudioSync::Admission::WrongPeer);
}

TEST_CASE("sound congestion is bounded without losing a loop stop silently",
          "[netplay][audio-sync]") {
    AudioSync host;
    host.begin(1, 1, {1, 2, 3, 4});
    SoundPlayer::Event event;
    event.kind = SoundPlayer::Event::Kind::Stop;
    event.handle = 1;
    for (usize index = 0; index < AudioSync::kBudget / 3; ++index) {
        REQUIRE(host.emit(event));
    }
    CHECK_FALSE(host.emit(event));
    CHECK(host.failed());
    CHECK(host.outgoing().size() <= AudioSync::kBudget);
}
TEST_CASE("native guest audio plays effects and queued narration with local gain and stops loops",
          "[netplay][audio-sync][assets][guest-audio]") {
    const auto root = test::assetOrSkip("AUDIO/COMMON.VBK").parent_path().parent_path();
    AudioMixer hostMixer(48000);
    AudioMixer guestMixer(48000);
    SoundPlayer hostOutput(hostMixer);
    SoundPlayer guestOutput(guestMixer);
    guestOutput.setCategoryVolume(SoundCategory::Effects, 0.7f);
    AudioSync host;
    AudioSync guest;
    host.begin(1, 1, {1, 2, 0, 0});
    guest.begin(1, 2, {1, 2, 0, 0});
    ReplicaAudio replica;
    replica.open(root, &guestOutput, nullptr, 1);
    SoundSet bank;
    REQUIRE(bank.load(root / "AUDIO/COMMON"));
    const auto call = bank.find("S_PICKUPMAGIC");
    REQUIRE(call);
    SoundHandle first = kNoSound;
    SoundHandle second = kNoSound;
    {
        const SoundPlayer::Capture capture(&hostOutput,
                                           [&](const auto& event) { REQUIRE(host.emit(event)); });
        first = hostOutput.play(bank.sequence(*call));
        second = hostOutput.playAfter(first, bank.sequence(*call));
        hostOutput.setPan(first, -0.5f);
        hostOutput.setVolume(first, 0.8f);
    }
    REQUIRE(first != kNoSound);
    REQUIRE(second != kNoSound);
    transfer(host, guest);
    REQUIRE(replica.consume(guest));
    CHECK(replica.voices() == 2);
    CHECK(guestOutput.voiceCount() == 1); // The second cue waits, not overlaps.
    std::vector<f32> pcm(usize{4800} * 2);
    guestMixer.mix(pcm);
    CHECK(std::ranges::any_of(pcm, [](f32 value) { return value != 0; }));
    CHECK(guestOutput.categoryVolume(SoundCategory::Effects) == 0.7f);
    {
        const SoundPlayer::Capture capture(&hostOutput,
                                           [&](const auto& event) { REQUIRE(host.emit(event)); });
        hostOutput.stop(first);
        hostOutput.stop(second);
    }
    transfer(host, guest);
    REQUIRE(replica.consume(guest));
    CHECK(replica.voices() == 0);
    // Local menu sounds outside the host simulation scope must not leak online.
    hostOutput.play(bank.sequence(*call));
    CHECK(host.outgoing().empty());
    SoundSet ambience;
    REQUIRE(ambience.load(root / "AUDIO/TOWAMB"));
    std::optional<u32> looping;
    for (u32 index = 0; index < ambience.size(); ++index) {
        const auto& steps = ambience.entry(index).sequence;
        if (std::ranges::any_of(steps, [](const auto& step) { return step.loopBack; })) {
            looping = index;
            break;
        }
    }
    REQUIRE(looping);
    SoundHandle loop = kNoSound;
    {
        const SoundPlayer::Capture capture(&hostOutput,
                                           [&](const auto& event) { REQUIRE(host.emit(event)); });
        loop = hostOutput.play(ambience.sequence(*looping));
    }
    transfer(host, guest);
    REQUIRE(replica.consume(guest));
    CHECK(replica.voices() == 1);
    {
        const SoundPlayer::Capture capture(&hostOutput,
                                           [&](const auto& event) { REQUIRE(host.emit(event)); });
        hostOutput.stop(loop);
    }
    transfer(host, guest);
    REQUIRE(replica.consume(guest));
    CHECK(replica.voices() == 0);
    replica.clear();
    hostOutput.stopAll();
}
} // namespace
