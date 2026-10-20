#include <array>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/LockstepInputs.h"
#include "game/screens/SessionInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;

InputCommand topicCommand(u8 topic, u8 revision) {
    InputCommand command;
    command.epoch = 9;
    command.tick = 1;
    command.grant = 2;
    command.seat = 3;
    command.hintTopic = topic;
    command.hintRevision = revision;
    return command;
}
} // namespace

TEST_CASE("Sumner commands use bounded explicit wire bytes and preserve revision zero",
          "[netplay][protocol][hint-input]") {
    for (u8 topic = 0; topic <= 4; ++topic) {
        for (u8 const revision : {u8{0}, u8{1}, u8{255}}) {
            CAPTURE(topic, revision);
            const auto command = topicCommand(topic, revision);
            const auto bytes = InputPacket::encode(std::array{command});
            if (topic == 0 && revision != 0) {
                CHECK_FALSE(command.valid());
                CHECK_FALSE(bytes);
                continue;
            }
            REQUIRE(bytes);
            CHECK(bytes->size() == InputPacket::kHeaderBytes + InputPacket::kCommandBytes);
            CHECK((*bytes)[30] == topic);
            CHECK((*bytes)[31] == revision);
            const auto decoded = InputPacket::decode(*bytes);
            REQUIRE(decoded);
            CHECK(decoded->front().hintTopic == topic);
            CHECK(decoded->front().hintRevision == revision);
            CHECK(InputPacket::encode(*decoded) == bytes);
            auto invalid = *bytes;
            invalid[30] = 5;
            CHECK_FALSE(InputPacket::decode(invalid));
            invalid = *bytes;
            invalid[4] = 3;
            CHECK_FALSE(InputPacket::decode(invalid));
        }
    }
    CHECK_FALSE(topicCommand(5, 0).valid());
    CHECK_FALSE(topicCommand(255, 1).valid());
}

TEST_CASE("Sumner choices round trip without pointers and never repeat in concealment",
          "[netplay][hint-input]") {
    PlayInput input;
    input.hintTopic = 4;
    input.hintRevision = 200;
    input.menu.pointer = Vec2{40, 80};
    input.menu.pointerPressed = true;
    input.menu.buttonPressed = true;
    const auto command = SessionInputs::command(input, 9, 1, 2, 3);
    CHECK_FALSE(command.pressed(CommandPress::MenuAny));
    const auto replay = SessionInputs::playInput(command);
    CHECK(replay.hintTopic == input.hintTopic);
    CHECK(replay.hintRevision == input.hintRevision);
    CHECK_FALSE(replay.menu.pointer);
    CHECK_FALSE(replay.menu.pointerPressed);
    const auto held = InputTimeline::heldOrNeutral(command, 9, 2, 2, 3);
    CHECK(held.hintTopic == 0);
    CHECK(held.hintRevision == 0);
    CHECK(held.valid());
}

TEST_CASE("native lockstep detects conflicting semantic choices without changing the queued one",
          "[netplay][hint-input]") {
    LockstepInputs host;
    const std::array links{LockstepInputs::Link{2, 7}};
    REQUIRE(host.open(1, 9, {0, 1, 0, 2}, {0, 1, 0, 2}, 0, links));
    const auto command = topicCommand(2, 17);
    const auto bytes = InputPacket::encode(std::array{command});
    REQUIRE(bytes);
    CHECK(host.receive(7, *bytes) == LockstepInputs::Admission::Accepted);
    CHECK(host.receive(7, *bytes) == LockstepInputs::Admission::Duplicate);
    auto conflict = command;
    ++conflict.hintRevision;
    CHECK(host.receive(7, *InputPacket::encode(std::array{conflict})) ==
          LockstepInputs::Admission::Conflict);
    auto local = topicCommand(0, 0);
    local.seat = 1;
    local.grant = 1;
    REQUIRE(host.sample(std::array{local}));
    const auto frame = host.advance();
    REQUIRE(frame);
    CHECK((*frame)[3].hintTopic == command.hintTopic);
    CHECK((*frame)[3].hintRevision == command.hintRevision);
}
